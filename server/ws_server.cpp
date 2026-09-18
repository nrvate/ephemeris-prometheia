// SPDX-License-Identifier: GPL-2.0-or-later
#include "ws_server.hpp"

#include <App.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "metrics.hpp"
#include "tls.hpp"

namespace prometheia::server {
namespace {

using Clock = std::chrono::steady_clock;

// Bytes a connection may have buffered unsent before its replies wait for
// the drain callback. Applied here rather than as uWS's maxBackpressure,
// past which uWS drops sends outright, ERROR replies included.
constexpr unsigned kBackpressureBytes = 4u << 20;
constexpr unsigned short kIdleTimeoutSeconds = 30;
constexpr int kDrainTickMs = 50;

// The per-connection data uWS keeps. Constructed once, at the upgrade, with
// its address; moved into the socket by uWS. The connection's place under
// the caps is released when the Conn goes, however it goes.
struct Conn {
    std::unique_ptr<Session> session;
    std::string addr;
    Limits* limits = nullptr; // set while this Conn holds a place
    Clock::time_point opened = Clock::now();
    bool close_when_sent = false;

    Conn() = default;
    Conn(Conn&& o) noexcept
        : session(std::move(o.session)), addr(std::move(o.addr)), limits(o.limits),
          opened(o.opened), close_when_sent(o.close_when_sent) {
        o.limits = nullptr;
    }
    Conn& operator=(Conn&&) = delete;
    ~Conn() {
        if (limits) {
            limits->release(addr);
        }
    }
};

// A port the kernel reports free, for port 0: uSockets sets SO_REUSEPORT
// only on a nonzero port, and every loop must share one.
int free_port(const std::string& bind) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!bind.empty()) {
        inet_pton(AF_INET, bind.c_str(), &addr.sin_addr);
    }
    socklen_t len = sizeof addr;
    int port = -1;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 &&
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        port = ntohs(addr.sin_port);
    }
    ::close(fd);
    return port;
}

// Whether something already accepts TCP connections on the port. With
// SO_REUSEPORT on every listener a second server would bind the same port
// without an error and share its traffic, so look first.
bool port_in_use(const std::string& bind, int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (!bind.empty() && bind != "0.0.0.0") {
        inet_pton(AF_INET, bind.c_str(), &addr.sin_addr);
    }
    const bool in_use = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
    ::close(fd);
    return in_use;
}

} // namespace

struct WsServer::Impl {
    WsOptions options;
    EngineFactory make_engine;
    std::unique_ptr<Limits> limits;
    bool tls = false;
    Log log{LogLevel::Quiet}; // set from options in the constructor

    // One event loop. The uWS objects are touched only on the loop's thread
    // (through Loop::defer from elsewhere); `loop` is non-null, under `mu`,
    // exactly while the loop may still run deferred work.
    struct LoopState {
        size_t index = 0;
        Metrics metrics;
        uWS::Loop* loop = nullptr;
        void* app = nullptr; // uWS::App* or uWS::SSLApp*
        us_listen_socket_t* listener = nullptr;
        us_timer_t* hello_timer = nullptr;
        std::unordered_set<void*> sockets;
        Impl* impl = nullptr;
        Clock::time_point drain_deadline{};
        // One deferred pump per loop at a time: it works each session's
        // computes a bounded slice at a time and reschedules itself while
        // any work remains, so an incoming CANCEL is read between slices
        // (3.4) instead of after a whole-request callback.
        bool pump_scheduled = false;
        uint64_t next_conn = 0; // connection ids in log lines: "<loop>.<n>"
    };
    std::vector<std::unique_ptr<LoopState>> loops;
    std::vector<std::thread> threads;
    mutable std::mutex mu;
    std::condition_variable cv;
    size_t ready = 0;
    std::atomic<int> listening{0};
    int port = -1;
    std::string error;
    std::atomic<bool> draining{false};
    bool stopped = false;

    template <bool SSL>
    using Ws = uWS::WebSocket<SSL, true, Conn>;
    template <bool SSL>
    using App = uWS::TemplatedApp<SSL>;

    // A request still computing is worked a bounded slice at a time between
    // loop turns: the deferred callback runs after whatever messages are
    // already waiting have been read, which is the window a CANCEL needs.
    // Schedules at most one pass at a time; the pass reschedules itself
    // while any session still has work.
    template <bool SSL>
    static void schedule_pump(LoopState& ls) {
        if (ls.pump_scheduled || !ls.loop) {
            return;
        }
        ls.pump_scheduled = true;
        LoopState* p = &ls;
        ls.loop->defer([p] {
            p->pump_scheduled = false;
            if (!p->app || p->sockets.empty()) {
                return;
            }
            // Copied at run time: anything that closed before this callback
            // ran was erased from the set first (all on this thread).
            std::vector<void*> sockets(p->sockets.begin(), p->sockets.end());
            bool more = false;
            for (void* q : sockets) {
                auto* ws = static_cast<Ws<SSL>*>(q);
                Conn* c = ws->getUserData();
                if (!c->session) {
                    continue;
                }
                if (c->session->has_work()) {
                    c->session->work(); // one bounded slice
                    more = true;
                }
                flush<SSL>(ws, *p); // send what the slice finished
            }
            if (more) {
                schedule_pump<SSL>(*p);
            }
        });
    }

    template <bool SSL>
    static void flush(Ws<SSL>* ws, LoopState& ls) {
        Conn* c = ws->getUserData();
        std::vector<uint8_t> msg;
        // BACKPRESSURE means the frame was taken and buffered; with no uWS
        // limit a send is never DROPPED.
        while (c->session->pending()) {
            if (ws->getBufferedAmount() >= kBackpressureBytes) {
                ++ls.metrics.backpressure_waits;
                break;
            }
            c->session->next(msg);
            ws->send(std::string_view(reinterpret_cast<const char*>(msg.data()), msg.size()),
                     uWS::OpCode::BINARY);
            ls.metrics.bytes_sent += msg.size();
        }
        if (c->close_when_sent && !c->session->pending()) {
            ws->end(1008, "protocol error");
        }
    }

    std::string ready_problem() const {
        if (draining) {
            return "draining\n";
        }
        if (listening.load() < int(loops.size())) {
            return "not every loop is listening\n";
        }
        return "";
    }

    template <bool SSL>
    void wire(App<SSL>& app, LoopState& ls, LoopContext& ctx) {
        app.get("/healthz", [](auto* res, auto*) {
            res->writeHeader("Content-Type", "text/plain")->end("ok\n");
        });
        app.get("/readyz", [this](auto* res, auto*) {
            const std::string why = ready_problem();
            if (why.empty()) {
                res->writeHeader("Content-Type", "text/plain")->end("ready\n");
            } else {
                res->writeStatus("503 Service Unavailable")
                    ->writeHeader("Content-Type", "text/plain")
                    ->end(why);
            }
        });
        app.get("/metrics", [this](auto* res, auto*) {
            res->writeHeader("Content-Type", "text/plain; version=0.0.4")->end(metrics_text());
        });

        typename App<SSL>::template WebSocketBehavior<Conn> behavior;
        behavior.compression = uWS::DISABLED;
        behavior.maxPayloadLength = ctx.config().max_payload + 65536;
        behavior.idleTimeout = kIdleTimeoutSeconds;
        behavior.maxBackpressure = 0;
        behavior.sendPingsAutomatically = true;
        // The caps, before a WebSocket exists: a refused client gets an HTTP
        // 503 with the reason and nothing is allocated for it.
        behavior.upgrade = [this, &ls](uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req,
                                       us_socket_context_t* context) {
            std::string addr(res->getRemoteAddressAsText());
            if (const char* why = limits->admit(addr)) {
                ++ls.metrics.refused_connections;
                log.write(LogLevel::Info, "refused addr=%s \"%s\"", log_addr(addr).c_str(), why);
                res->writeStatus("503 Service Unavailable")
                    ->writeHeader("Content-Type", "text/plain")
                    ->end(std::string(why) + "\n");
                return;
            }
            Conn conn;
            conn.addr = std::move(addr);
            conn.limits = limits.get();
            res->template upgrade<Conn>(std::move(conn), req->getHeader("sec-websocket-key"),
                                        req->getHeader("sec-websocket-protocol"),
                                        req->getHeader("sec-websocket-extensions"), context);
        };
        behavior.open = [this, &ls, &ctx](Ws<SSL>* ws) {
            Conn* c = ws->getUserData();
            const std::string id = std::to_string(ls.index) + "." + std::to_string(ls.next_conn++);
            c->session = std::make_unique<Session>(ctx, c->addr, id);
            c->opened = Clock::now();
            ls.sockets.insert(ws);
            ++ls.metrics.connections_open;
            ++ls.metrics.connections_total;
            log.write(LogLevel::Info, "c=%s open addr=%s%s", id.c_str(), log_addr(c->addr).c_str(),
                      SSL ? " tls" : "");
        };
        behavior.message = [&ls](Ws<SSL>* ws, std::string_view message, uWS::OpCode op) {
            Conn* c = ws->getUserData();
            if (c->close_when_sent) {
                return;
            }
            const bool keep = c->session->on_message(message, op == uWS::OpCode::BINARY);
            // Read before the flush: flush may end() the connection for a
            // closing error, and the socket's memory does not survive that.
            const bool has_work = c->session->has_work();
            if (!keep) {
                c->close_when_sent = true;
            }
            flush<SSL>(ws, ls);
            // A REQUEST this message accepted is worked by the pump, not in
            // this callback (3.4): a CANCEL that follows is read first.
            if (has_work) {
                schedule_pump<SSL>(ls);
            }
        };
        behavior.drain = [&ls](Ws<SSL>* ws) {
            Conn* c = ws->getUserData();
            const bool has_work = c->session->has_work();
            flush<SSL>(ws, ls);
            if (has_work) {
                schedule_pump<SSL>(ls);
            }
        };
        behavior.close = [this, &ls](Ws<SSL>* ws, int code, std::string_view) {
            const Conn* c = ws->getUserData();
            if (c->session) {
                const double ms =
                    std::chrono::duration<double, std::milli>(Clock::now() - c->opened).count();
                log.write(LogLevel::Info, "c=%s close code=%d requests=%u ms=%.0f",
                          c->session->conn().c_str(), code, c->session->requests_seen(), ms);
            }
            ls.sockets.erase(ws);
            --ls.metrics.connections_open;
        };
        app.template ws<Conn>("/*", std::move(behavior));
    }

    // Once a period: close what connected and has not said HELLO in time.
    // Not a fallthrough timer: it keeps the loop alive until stop or drain
    // closes it (uSockets decrements the loop's poll count for every closed
    // timer, fallthrough or not).
    template <bool SSL>
    static void hello_sweep(us_timer_t* t) {
        LoopState& ls = **static_cast<LoopState**>(us_timer_ext(t));
        const auto limit =
            Clock::now() - std::chrono::milliseconds(ls.impl->options.hello_timeout_ms);
        std::vector<Ws<SSL>*> late;
        for (void* p : ls.sockets) {
            auto* ws = static_cast<Ws<SSL>*>(p);
            const Conn* c = ws->getUserData();
            if (c->session && c->session->version() == 0 && c->opened < limit) {
                late.push_back(ws);
            }
        }
        for (Ws<SSL>* ws : late) {
            ++ls.metrics.hello_timeouts;
            ls.impl->log.write(LogLevel::Info, "c=%s no HELLO in %u ms, closing",
                               ws->getUserData()->session->conn().c_str(),
                               ls.impl->options.hello_timeout_ms);
            ws->end(1008, "no HELLO");
        }
    }

    template <bool SSL>
    static void close_timers(LoopState& ls) {
        if (ls.hello_timer) {
            us_timer_close(ls.hello_timer);
            ls.hello_timer = nullptr;
        }
    }

    template <bool SSL>
    static bool idle(LoopState& ls) {
        for (void* p : ls.sockets) {
            auto* ws = static_cast<Ws<SSL>*>(p);
            const Conn* c = ws->getUserData();
            // In-flight computes too: letting them finish caches the answer
            // for whoever asks the same question after the restart.
            if (ws->getBufferedAmount() > 0 || (c->session && c->session->pending()) ||
                (c->session && c->session->has_work())) {
                return false;
            }
        }
        return true;
    }

    template <bool SSL>
    static void drain_tick(us_timer_t* t) {
        LoopState& ls = **static_cast<LoopState**>(us_timer_ext(t));
        const bool late = Clock::now() >= ls.drain_deadline;
        if (!late && !idle<SSL>(ls)) {
            return;
        }
        std::vector<void*> sockets(ls.sockets.begin(), ls.sockets.end());
        for (void* p : sockets) {
            auto* ws = static_cast<Ws<SSL>*>(p);
            // A close frame waits behind unsent bytes, which a client that
            // never reads never lets out: at the deadline close those hard.
            if (late && ws->getBufferedAmount() > 0) {
                ws->close();
            } else {
                ws->end(1001, "server going away");
            }
        }
        us_timer_close(t);
    }

    template <bool SSL>
    static void start_drain(LoopState& ls, unsigned seconds) {
        if (ls.listener) {
            us_listen_socket_close(SSL, ls.listener);
            ls.listener = nullptr;
        }
        close_timers<SSL>(ls);
        ls.drain_deadline = Clock::now() + std::chrono::seconds(seconds);
        us_timer_t* t = us_create_timer(static_cast<us_loop_t*>(static_cast<void*>(ls.loop)), 0,
                                        sizeof(LoopState*));
        *static_cast<LoopState**>(us_timer_ext(t)) = &ls;
        us_timer_set(t, drain_tick<SSL>, 1, kDrainTickMs);
    }

    template <bool SSL>
    void run_loop(LoopState& ls, int listen_port) {
        auto engine = make_engine();
        if (!engine) {
            report(ls, nullptr, -1, "engine: " + engine.error().message);
            return;
        }
        LoopContext ctx(std::move(engine).value(), options.config, limits.get(), &ls.metrics, &log);
        std::unique_ptr<App<SSL>> app;
        if (SSL) {
            uWS::SocketContextOptions o;
            o.cert_file_name = options.tls_cert.c_str();
            o.key_file_name = options.tls_key.c_str();
            o.ssl_ciphers = tls::kTlsCiphers;
            app = std::make_unique<App<SSL>>(o);
        } else {
            app = std::make_unique<App<SSL>>();
        }
        if (app->constructorFailed()) {
            report(ls, nullptr, -1, "TLS setup failed for " + options.tls_cert);
            return;
        }
        wire<SSL>(*app, ls, ctx);

        us_listen_socket_t* listener = nullptr;
        const auto on_listen = [&](us_listen_socket_t* s) { listener = s; };
        if (options.bind.empty()) {
            app->listen(listen_port, on_listen);
        } else {
            app->listen(options.bind, listen_port, on_listen);
        }
        if (!listener) {
            report(ls, nullptr, -1, "cannot listen on port " + std::to_string(listen_port));
            return;
        }
        ls.listener = listener;
        ls.loop = uWS::Loop::get();
        if (options.hello_timeout_ms) {
            const int period = int(std::clamp<uint32_t>(options.hello_timeout_ms / 4, 20, 1000));
            ls.hello_timer = us_create_timer(static_cast<us_loop_t*>(static_cast<void*>(ls.loop)),
                                             0, sizeof(LoopState*));
            *static_cast<LoopState**>(us_timer_ext(ls.hello_timer)) = &ls;
            us_timer_set(ls.hello_timer, hello_sweep<SSL>, period, period);
        }
        ++listening;
        report(ls, app.get(),
               us_socket_local_port(SSL, static_cast<us_socket_t*>(static_cast<void*>(listener))),
               "");
        app->run();
        {
            std::lock_guard lock(mu);
            ls.loop = nullptr;
            ls.app = nullptr;
        }
        --listening;
        // The App is destroyed here, on its own thread, while the thread's
        // uWS::Loop still exists.
    }

    void run(LoopState& ls, int listen_port) {
        if (tls) {
            run_loop<true>(ls, listen_port);
        } else {
            run_loop<false>(ls, listen_port);
        }
    }

    void report(LoopState& ls, void* app, int bound, const std::string& err) {
        std::lock_guard lock(mu);
        ls.app = app;
        if (!app) {
            ls.loop = nullptr;
        }
        if (!err.empty() && error.empty()) {
            error = err;
        }
        if (bound > 0 && port < 0) {
            port = bound;
        }
        ++ready;
        cv.notify_all();
        // A stop or drain that came while this loop was starting.
        if (app && stopped) {
            post_stop(ls);
        } else if (app && draining) {
            post_drain(ls, drain_seconds);
        }
    }

    unsigned drain_seconds = 0;

    // Both called with `mu` held and ls.loop non-null.
    void post_stop(LoopState& ls) {
        LoopState* p = &ls;
        const bool ssl = tls;
        ls.loop->defer([p, ssl] {
            if (!p->app) {
                return;
            }
            if (ssl) {
                close_timers<true>(*p);
                static_cast<App<true>*>(p->app)->close();
            } else {
                close_timers<false>(*p);
                static_cast<App<false>*>(p->app)->close();
            }
            p->listener = nullptr;
        });
    }

    void post_drain(LoopState& ls, unsigned seconds) {
        LoopState* p = &ls;
        const bool ssl = tls;
        ls.loop->defer([p, ssl, seconds] {
            if (!p->app) {
                return;
            }
            if (ssl) {
                start_drain<true>(*p, seconds);
            } else {
                start_drain<false>(*p, seconds);
            }
        });
    }

    std::string metrics_text() const {
        std::vector<const Metrics*> all;
        for (const auto& ls : loops) {
            all.push_back(&ls->metrics);
        }
        ServerInfo info;
        info.server_version = kServerVersion;
        info.protocol = eph::kProtoVersion;
        info.tls = tls;
        info.draining = draining;
        return server::metrics_text(all, info);
    }
};

WsServer::WsServer(WsOptions options, EngineFactory make_engine) : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
    impl_->log = Log(impl_->options.log_level);
    impl_->make_engine = std::move(make_engine);
    LimitsConfig lc = impl_->options.limits;
    lc.burst_cells = impl_->options.config.max_cells;
    impl_->limits = std::make_unique<Limits>(lc, impl_->options.tokens);
    impl_->tls = !impl_->options.tls_cert.empty();
}

WsServer::~WsServer() {
    stop();
    join();
}

Result<void> WsServer::start() {
    Impl& im = *impl_;
    if (im.options.tls_cert.empty() != im.options.tls_key.empty()) {
        return make_error(ErrorCode::ArgumentError,
                          "a TLS certificate needs its key, and a key its certificate");
    }
    if (im.options.limits.require_token && im.options.tokens.empty()) {
        return make_error(ErrorCode::ArgumentError, "a token is required but none is accepted");
    }
    if (im.tls) {
        if (auto r = tls::check_pair(im.options.tls_cert, im.options.tls_key); !r) {
            return r.error();
        }
    }
    const size_t n = std::max<unsigned>(1, im.options.threads);
    for (size_t i = 0; i < n; ++i) {
        im.loops.push_back(std::make_unique<Impl::LoopState>());
        im.loops.back()->index = i;
        im.loops.back()->impl = &im;
    }
    int port = im.options.port;
    if (port == 0) {
        port = free_port(im.options.bind);
        if (port <= 0) {
            return make_error(ErrorCode::IoError, "no free port");
        }
    } else if (port_in_use(im.options.bind, port)) {
        return make_error(ErrorCode::IoError,
                          "another server is listening on port " + std::to_string(port));
    }
    // The first loop listens before the others share its port through
    // SO_REUSEPORT, so a port that cannot be bound fails once, early.
    im.threads.emplace_back([&im, port] { im.run(*im.loops[0], port); });
    {
        std::unique_lock lock(im.mu);
        im.cv.wait(lock, [&] { return im.ready >= 1; });
        if (!im.error.empty()) {
            return make_error(ErrorCode::IoError, im.error);
        }
    }
    for (size_t i = 1; i < n; ++i) {
        im.threads.emplace_back([&im, i, port] { im.run(*im.loops[i], port); });
    }
    std::unique_lock lock(im.mu);
    im.cv.wait(lock, [&] { return im.ready >= n; });
    if (!im.error.empty()) {
        lock.unlock();
        stop();
        return make_error(ErrorCode::IoError, im.error);
    }
    return {};
}

int WsServer::port() const {
    std::lock_guard lock(impl_->mu);
    return impl_->port;
}

bool WsServer::tls() const {
    return impl_->tls;
}

void WsServer::stop() {
    std::lock_guard lock(impl_->mu);
    impl_->stopped = true;
    for (const auto& ls : impl_->loops) {
        if (ls->loop && ls->app) {
            impl_->post_stop(*ls);
        }
    }
}

void WsServer::drain(unsigned seconds) {
    std::lock_guard lock(impl_->mu);
    if (impl_->draining.exchange(true)) {
        return;
    }
    impl_->drain_seconds = seconds;
    for (const auto& ls : impl_->loops) {
        if (ls->loop && ls->app) {
            impl_->post_drain(*ls, seconds);
        }
    }
}

bool WsServer::draining() const {
    return impl_->draining;
}

void WsServer::join() {
    for (std::thread& t : impl_->threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    impl_->threads.clear();
}

Result<void> WsServer::reload_tls() {
    Impl& im = *impl_;
    if (!im.tls) {
        return make_error(ErrorCode::ArgumentError, "no certificate to reload (plain ws://)");
    }
    if (auto r = tls::check_pair(im.options.tls_cert, im.options.tls_key); !r) {
        return r.error();
    }
    std::lock_guard lock(im.mu);
    for (const auto& ls : im.loops) {
        if (!ls->loop || !ls->app) {
            continue;
        }
        Impl::LoopState* p = ls.get();
        const std::string cert = im.options.tls_cert, key = im.options.tls_key;
        ls->loop->defer([p, cert, key] {
            if (!p->app) {
                return;
            }
            auto* app = static_cast<Impl::App<true>*>(p->app);
            if (auto r = tls::load_into(app->getNativeHandle(), cert, key); !r) {
                std::fprintf(stderr, "prometheiad: loop %zu: TLS reload failed: %s\n", p->index,
                             r.error().message.c_str());
            }
        });
    }
    return {};
}

std::string WsServer::metrics() const {
    return impl_->metrics_text();
}

} // namespace prometheia::server

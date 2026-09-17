// SPDX-License-Identifier: GPL-2.0-or-later
#include "ws_server.hpp"

#include <App.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace prometheia::server {
namespace {

// Bytes a connection may have buffered unsent before its replies wait for
// the drain callback. Applied here rather than as uWS's maxBackpressure,
// past which uWS drops sends outright, ERROR replies included.
constexpr unsigned kBackpressureBytes = 4u << 20;
constexpr unsigned short kIdleTimeoutSeconds = 30;

struct Conn {
    std::unique_ptr<Session> session;
    bool close_when_sent = false;
};

using Ws = uWS::WebSocket<false, true, Conn>;

void flush(Ws* ws) {
    Conn* c = ws->getUserData();
    std::vector<uint8_t> msg;
    // BACKPRESSURE means the frame was taken and buffered; with no uWS
    // limit a send is never DROPPED.
    while (ws->getBufferedAmount() < kBackpressureBytes && c->session->next(msg)) {
        ws->send(std::string_view(reinterpret_cast<const char*>(msg.data()), msg.size()),
                 uWS::OpCode::BINARY);
    }
    if (c->close_when_sent && !c->session->pending()) {
        ws->end(1008, "protocol error");
    }
}

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

} // namespace

struct WsServer::Impl {
    WsOptions options;
    WireMap map;
    EngineFactory make_engine;

    struct Loop {
        uWS::Loop* loop = nullptr;
        uWS::App* app = nullptr;
    };
    std::vector<std::thread> threads;
    std::mutex mu;
    std::condition_variable cv;
    std::vector<Loop> loops;
    size_t ready = 0;
    int port = -1;
    std::string error;
    bool stopping = false;

    void run(size_t index, int listen_port) {
        auto engine = make_engine();
        if (!engine) {
            report(index, nullptr, nullptr, -1, "engine: " + engine.error().message);
            return;
        }
        LoopContext ctx(std::move(engine).value(), map, options.config);
        uWS::App app;
        uWS::App::WebSocketBehavior<Conn> behavior;
        behavior.compression = uWS::DISABLED;
        behavior.maxPayloadLength = eph::kMaxPayload + 65536;
        behavior.idleTimeout = kIdleTimeoutSeconds;
        behavior.maxBackpressure = 0;
        behavior.sendPingsAutomatically = true;
        const bool verbose = options.verbose;
        behavior.open = [&ctx, verbose](Ws* ws) {
            ws->getUserData()->session = std::make_unique<Session>(ctx);
            if (verbose) {
                std::fprintf(stderr, "prometheiad: open %.*s\n",
                             int(ws->getRemoteAddressAsText().size()),
                             ws->getRemoteAddressAsText().data());
            }
        };
        behavior.message = [](Ws* ws, std::string_view message, uWS::OpCode op) {
            Conn* c = ws->getUserData();
            if (c->close_when_sent) {
                return;
            }
            if (!c->session->on_message(message, op == uWS::OpCode::BINARY)) {
                c->close_when_sent = true;
            }
            flush(ws);
        };
        behavior.drain = [](Ws* ws) { flush(ws); };
        app.ws<Conn>("/*", std::move(behavior));

        us_listen_socket_t* listener = nullptr;
        const auto on_listen = [&](us_listen_socket_t* s) { listener = s; };
        if (options.bind.empty()) {
            app.listen(listen_port, on_listen);
        } else {
            app.listen(options.bind, listen_port, on_listen);
        }
        if (!listener) {
            report(index, nullptr, nullptr, -1,
                   "cannot listen on port " + std::to_string(listen_port));
            return;
        }
        report(index, uWS::Loop::get(), &app, us_socket_local_port(0, (us_socket_t*)listener), "");
        app.run();
        std::lock_guard lock(mu);
        loops[index] = Loop{};
    }

    void report(size_t index, uWS::Loop* loop, uWS::App* app, int bound, const std::string& err) {
        std::lock_guard lock(mu);
        loops[index] = Loop{loop, app};
        if (!err.empty() && error.empty()) {
            error = err;
        }
        if (bound > 0 && port < 0) {
            port = bound;
        }
        ++ready;
        cv.notify_all();
        if (stopping && app) {
            loop->defer([app] { app->close(); });
        }
    }
};

WsServer::WsServer(WsOptions options, WireMap map, EngineFactory make_engine)
    : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
    impl_->map = std::move(map);
    impl_->make_engine = std::move(make_engine);
}

WsServer::~WsServer() {
    stop();
    join();
}

Result<void> WsServer::start() {
    Impl& im = *impl_;
    const size_t n = std::max<unsigned>(1, im.options.threads);
    im.loops.assign(n, {});
    // The first loop listens before the others share its port through
    // SO_REUSEPORT, so a port that is taken fails once, early.
    const int port = im.options.port == 0 ? free_port(im.options.bind) : im.options.port;
    if (port <= 0) {
        return make_error(ErrorCode::IoError, "no free port");
    }
    im.threads.emplace_back([&im, port] { im.run(0, port); });
    {
        std::unique_lock lock(im.mu);
        im.cv.wait(lock, [&] { return im.ready >= 1; });
        if (!im.error.empty()) {
            return make_error(ErrorCode::IoError, im.error);
        }
    }
    for (size_t i = 1; i < n; ++i) {
        im.threads.emplace_back([&im, i] { im.run(i, im.port); });
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

void WsServer::stop() {
    std::lock_guard lock(impl_->mu);
    impl_->stopping = true;
    for (const Impl::Loop& l : impl_->loops) {
        if (l.loop && l.app) {
            uWS::App* app = l.app;
            l.loop->defer([app] { app->close(); });
        }
    }
}

void WsServer::join() {
    for (std::thread& t : impl_->threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    impl_->threads.clear();
}

} // namespace prometheia::server

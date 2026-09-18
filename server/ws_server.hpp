// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheiad's WebSocket head: one uWebSockets event loop per thread, each
// with its own Engine and LoopContext, all listening on one port
// (SO_REUSEPORT lets the kernel balance the accepts). Each binary message
// goes to the connection's Session; its replies are sent while the socket
// has less than 4 MiB buffered, and the rest follow from the drain callback.
//
// Around the protocol (docs/SERVER.md, "Operations"):
//   - connection caps at the upgrade (HTTP 503 with the reason);
//   - a HELLO deadline;
//   - GET /healthz, /readyz and /metrics (Prometheus text) on the same port;
//   - drain(): stop accepting, let answers in flight finish, then close;
//   - wss:// when a certificate and key are given, reloadable in place.
#ifndef PROMETHEIA_SERVER_WS_SERVER_HPP
#define PROMETHEIA_SERVER_WS_SERVER_HPP

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>

#include "limits.hpp"
#include "prometheia/engine.hpp"
#include "prometheia/error.hpp"
#include "session.hpp"

namespace prometheia::server {

struct WsOptions {
    std::string bind; // empty: every interface
    int port = 47190; // 0: a free port, see WsServer::port()
    unsigned threads = 1;
    // Connections, HELLOs, requests and errors on stderr, one line each
    // (log.hpp). Quiet by default for embedders; prometheiad asks for info.
    LogLevel log_level = LogLevel::Quiet;
    ServerConfig config;
    // Caps, budget and token requirement; burst_cells is taken from
    // config.max_cells.
    LimitsConfig limits;
    std::unordered_set<std::string> tokens;
    uint32_t hello_timeout_ms = 10000; // close without HELLO after this; 0 = never
    std::string tls_cert;              // PEM chain; with tls_key, serve wss://
    std::string tls_key;
};

class WsServer {
public:
    // Opens one Engine per thread; called on that thread.
    using EngineFactory = std::function<Result<Engine>()>;

    WsServer(WsOptions options, EngineFactory make_engine);
    ~WsServer(); // stops and joins

    // Starts the loops; returns once every loop listens, or the first error.
    Result<void> start();
    // The port listened on (the chosen one when options.port was 0).
    int port() const;
    bool tls() const;

    // Closes the listeners and every connection at once; the loops return.
    void stop();
    // Stops accepting; connections with answers queued or bytes unsent get
    // up to `seconds` to finish, then every connection is closed (1001) and
    // the loops return. /readyz answers 503 from the start of the drain.
    void drain(unsigned seconds);
    bool draining() const;
    // Blocks until every loop has returned.
    void join();

    // Re-reads the certificate and key into every loop (checked first; a bad
    // pair leaves the current one serving).
    Result<void> reload_tls();

    // /metrics's text, summed over the loops.
    std::string metrics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace prometheia::server

#endif // PROMETHEIA_SERVER_WS_SERVER_HPP

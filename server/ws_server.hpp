// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheiad's WebSocket head: one uWebSockets event loop per thread, each
// with its own Engine and LoopContext, all listening on one port
// (SO_REUSEPORT lets the kernel balance the accepts). Each binary message
// goes to the connection's Session; its replies are sent while the socket
// has less than 4 MiB buffered, and the rest follow from the drain callback.
#ifndef PROMETHEIA_SERVER_WS_SERVER_HPP
#define PROMETHEIA_SERVER_WS_SERVER_HPP

#include <functional>
#include <memory>
#include <string>

#include "prometheia/engine.hpp"
#include "prometheia/error.hpp"
#include "session.hpp"
#include "wire_map.hpp"

namespace prometheia::server {

struct WsOptions {
    std::string bind; // empty: every interface
    int port = 47190; // 0: an ephemeral port, see WsServer::port()
    unsigned threads = 1;
    bool verbose = false; // one line per connection on stderr
    ServerConfig config;
};

class WsServer {
public:
    // Opens one Engine per thread; called on that thread.
    using EngineFactory = std::function<Result<Engine>()>;

    WsServer(WsOptions options, WireMap map, EngineFactory make_engine);
    ~WsServer(); // stops and joins

    // Starts the loops; returns once every loop listens, or the first error.
    Result<void> start();
    // The port listened on (the ephemeral one when options.port was 0).
    int port() const;
    // Closes the listeners and every connection; the loops then return.
    void stop();
    // Blocks until every loop has returned.
    void join();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace prometheia::server

#endif // PROMETHEIA_SERVER_WS_SERVER_HPP

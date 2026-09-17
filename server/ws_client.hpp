// SPDX-License-Identifier: GPL-2.0-or-later
//
// A minimal blocking WebSocket client (RFC 6455) for talking to prometheiad:
// the reference client (prometheia-wire-client) and the socket round-trip
// test. Binary messages only; answers protocol pings, reassembles
// fragmented messages, masks what it sends. No TLS.
#ifndef PROMETHEIA_SERVER_WS_CLIENT_HPP
#define PROMETHEIA_SERVER_WS_CLIENT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "prometheia/error.hpp"

namespace prometheia::server {

class WsClient {
public:
    WsClient() = default;
    ~WsClient();
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // TCP connect and the HTTP upgrade to a WebSocket at `path`.
    Result<void> connect(const std::string& host, int port, const std::string& path = "/");
    // Sends one binary message.
    Result<void> send(const std::vector<uint8_t>& message);
    // Receives the next binary message. timeout_ms < 0 waits forever.
    // NotFound when the server closed the connection.
    Result<std::vector<uint8_t>> receive(int timeout_ms = 10000);
    void close();

private:
    Result<void> write_all(const uint8_t* data, size_t len);
    Result<void> read_exact(uint8_t* data, size_t len, int timeout_ms);
    Result<void> send_frame(uint8_t opcode, const uint8_t* data, size_t len);

    int fd_ = -1;
    std::vector<uint8_t> buffered_; // bytes read past the HTTP response
};

} // namespace prometheia::server

#endif // PROMETHEIA_SERVER_WS_CLIENT_HPP

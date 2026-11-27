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

struct WsTlsOptions {
    bool enabled = false;
    std::string ca_file; // PEM trust anchors; empty: the system store
    std::string sni;     // name to verify; empty: the host
    bool verify = true;  // check the chain and the name
};

class WsClient {
public:
    WsClient() = default;
    ~WsClient();
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // TCP connect, TLS when asked (needs a PROMETHEIA_TLS build), and the
    // HTTP upgrade to a WebSocket at `path`.
    Result<void> connect(const std::string& host, int port, const std::string& path = "/",
                         const WsTlsOptions& tls = {});
    // One plain HTTP GET on a fresh connection (TLS as given); returns the
    // whole response, status line included. For /healthz and friends.
    static Result<std::string> http_get(const std::string& host, int port, const std::string& path,
                                        const WsTlsOptions& tls = {});
    // Sends one binary message.
    Result<void> send(const std::vector<uint8_t>& message);
    // Receives the next binary message. timeout_ms < 0 waits forever.
    // NotFound when the server closed the connection.
    Result<std::vector<uint8_t>> receive(int timeout_ms = 10000);
    void close();

private:
    Result<void> open_transport(const std::string& host, int port, const WsTlsOptions& tls);
    Result<void> write_all(const uint8_t* data, size_t len);
    // Some bytes into data (at most len), waiting up to timeout_ms.
    Result<size_t> read_some(uint8_t* data, size_t len, int timeout_ms);
    Result<void> read_exact(uint8_t* data, size_t len, int timeout_ms);
    Result<void> send_frame(uint8_t opcode, const uint8_t* data, size_t len);

    int fd_ = -1;
    void* ssl_ = nullptr;           // SSL* when TLS
    void* ssl_ctx_ = nullptr;       // SSL_CTX*
    std::vector<uint8_t> buffered_; // bytes read past the HTTP response
};

} // namespace prometheia::server

#endif // PROMETHEIA_SERVER_WS_CLIENT_HPP

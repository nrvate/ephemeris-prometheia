// SPDX-License-Identifier: GPL-2.0-or-later
#include "ws_client.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <random>

#ifdef PROMETHEIA_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

namespace prometheia::server {
namespace {

Error io_error(const std::string& what) {
    return make_error(ErrorCode::IoError, what + ": " + std::strerror(errno));
}

std::string base64(const uint8_t* p, size_t n) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) |
                           (i + 2 < n ? uint32_t(p[i + 2]) : 0);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += i + 1 < n ? kAlphabet[(v >> 6) & 63] : '=';
        out += i + 2 < n ? kAlphabet[v & 63] : '=';
    }
    return out;
}

} // namespace

WsClient::~WsClient() {
    close();
}

void WsClient::close() {
#ifdef PROMETHEIA_TLS
    if (ssl_) {
        SSL_free(static_cast<SSL*>(ssl_));
        ssl_ = nullptr;
    }
    if (ssl_ctx_) {
        SSL_CTX_free(static_cast<SSL_CTX*>(ssl_ctx_));
        ssl_ctx_ = nullptr;
    }
#endif
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    buffered_.clear();
}

Result<void> WsClient::open_transport(const std::string& host, int port, const WsTlsOptions& tls) {
    close();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (const int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res)) {
        return make_error(ErrorCode::IoError, host + ": " + gai_strerror(rc));
    }
    for (addrinfo* ai = res; ai && fd_ < 0; ai = ai->ai_next) {
        fd_ = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd_ >= 0 && ::connect(fd_, ai->ai_addr, ai->ai_addrlen) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    freeaddrinfo(res);
    if (fd_ < 0) {
        return io_error("connect " + host + ":" + std::to_string(port));
    }
    const int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (!tls.enabled) {
        return {};
    }
#ifdef PROMETHEIA_TLS
    const auto tls_error = [this](const std::string& what) {
        char text[256] = "";
        if (const unsigned long e = ERR_get_error()) {
            ERR_error_string_n(e, text, sizeof text);
        }
        ERR_clear_error();
        close();
        return make_error(ErrorCode::IoError, what + ": " + text);
    };
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    ssl_ctx_ = ctx;
    if (!ctx) {
        return tls_error("TLS context");
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (tls.verify) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        const int loaded = tls.ca_file.empty()
                               ? SSL_CTX_set_default_verify_paths(ctx)
                               : SSL_CTX_load_verify_locations(ctx, tls.ca_file.c_str(), nullptr);
        if (loaded != 1) {
            return tls_error("trust anchors");
        }
    }
    SSL* ssl = SSL_new(ctx);
    ssl_ = ssl;
    const std::string name = tls.sni.empty() ? host : tls.sni;
    SSL_set_tlsext_host_name(ssl, name.c_str());
    if (tls.verify) {
        SSL_set1_host(ssl, name.c_str());
    }
    SSL_set_fd(ssl, fd_);
    if (SSL_connect(ssl) != 1) {
        return tls_error("TLS handshake with " + host);
    }
    return {};
#else
    close();
    return make_error(ErrorCode::ArgumentError, "this build has no TLS (OpenSSL was not found)");
#endif
}

Result<void> WsClient::connect(const std::string& host, int port, const std::string& path,
                               const WsTlsOptions& tls) {
    if (auto r = open_transport(host, port, tls); !r) {
        return r;
    }

    uint8_t nonce[16];
    std::random_device rd;
    for (uint8_t& b : nonce) {
        b = uint8_t(rd());
    }
    const std::string request =
        "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: " +
        base64(nonce, sizeof nonce) + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if (auto w = write_all(reinterpret_cast<const uint8_t*>(request.data()), request.size()); !w) {
        return w;
    }
    std::string response;
    uint8_t buf[1024];
    while (response.find("\r\n\r\n") == std::string::npos) {
        auto n = read_some(buf, sizeof buf, 10000);
        if (!n) {
            return make_error(ErrorCode::IoError,
                              "no WebSocket upgrade response: " + n.error().message);
        }
        response.append(reinterpret_cast<const char*>(buf), n.value());
        if (response.size() > 16384) {
            return make_error(ErrorCode::FormatError, "oversized upgrade response");
        }
    }
    if (response.rfind("HTTP/1.1 101", 0) != 0) {
        return make_error(ErrorCode::FormatError,
                          "upgrade refused: " + response.substr(0, response.find("\r\n")));
    }
    const size_t body = response.find("\r\n\r\n") + 4;
    buffered_.assign(response.begin() + long(body), response.end());
    return {};
}

Result<void> WsClient::write_all(const uint8_t* data, size_t len) {
    while (len > 0) {
#ifdef PROMETHEIA_TLS
        if (ssl_) {
            const int n =
                SSL_write(static_cast<SSL*>(ssl_), data, int(std::min<size_t>(len, 1 << 30)));
            if (n <= 0) {
                return make_error(ErrorCode::IoError, "TLS write failed");
            }
            data += n;
            len -= size_t(n);
            continue;
        }
#endif
        const ssize_t n = ::send(fd_, data, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return io_error("send");
        }
        data += n;
        len -= size_t(n);
    }
    return {};
}

Result<size_t> WsClient::read_some(uint8_t* data, size_t len, int timeout_ms) {
    for (;;) {
#ifdef PROMETHEIA_TLS
        const bool tls_buffered = ssl_ && SSL_pending(static_cast<SSL*>(ssl_)) > 0;
#else
        const bool tls_buffered = false;
#endif
        if (!tls_buffered) {
            pollfd pfd{fd_, POLLIN, 0};
            const int ready = ::poll(&pfd, 1, timeout_ms);
            if (ready == 0) {
                return make_error(ErrorCode::IoError, "timed out waiting for the server");
            }
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return io_error("poll");
            }
        }
#ifdef PROMETHEIA_TLS
        if (ssl_) {
            SSL* ssl = static_cast<SSL*>(ssl_);
            const int n = SSL_read(ssl, data, int(std::min<size_t>(len, 1 << 30)));
            if (n > 0) {
                return size_t(n);
            }
            const int e = SSL_get_error(ssl, n);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            if (e == SSL_ERROR_ZERO_RETURN || e == SSL_ERROR_SYSCALL) {
                ERR_clear_error();
                return make_error(ErrorCode::NotFound, "the server closed the connection");
            }
            ERR_clear_error();
            return make_error(ErrorCode::IoError, "TLS read failed");
        }
#endif
        const ssize_t n = ::recv(fd_, data, len, 0);
        if (n == 0) {
            return make_error(ErrorCode::NotFound, "the server closed the connection");
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return io_error("recv");
        }
        return size_t(n);
    }
}

Result<void> WsClient::read_exact(uint8_t* data, size_t len, int timeout_ms) {
    const size_t from_buffer = std::min(len, buffered_.size());
    std::memcpy(data, buffered_.data(), from_buffer);
    buffered_.erase(buffered_.begin(), buffered_.begin() + long(from_buffer));
    size_t got = from_buffer;
    while (got < len) {
        auto n = read_some(data + got, len - got, timeout_ms);
        if (!n) {
            return n.error();
        }
        got += n.value();
    }
    return {};
}

Result<std::string> WsClient::http_get(const std::string& host, int port, const std::string& path,
                                       const WsTlsOptions& tls) {
    WsClient c;
    if (auto r = c.open_transport(host, port, tls); !r) {
        return r.error();
    }
    const std::string request =
        "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    if (auto r = c.write_all(reinterpret_cast<const uint8_t*>(request.data()), request.size());
        !r) {
        return r.error();
    }
    std::string response;
    uint8_t buf[4096];
    for (;;) {
        auto n = c.read_some(buf, sizeof buf, 5000);
        if (!n) {
            if (n.error().code == ErrorCode::NotFound && !response.empty()) {
                break;
            }
            return n.error();
        }
        response.append(reinterpret_cast<const char*>(buf), n.value());
        // uWS answers with Content-Length and may keep the socket open.
        const size_t head = response.find("\r\n\r\n");
        if (head != std::string::npos) {
            const size_t cl = response.find("Content-Length: ");
            if (cl != std::string::npos && cl < head) {
                const size_t body = std::strtoul(response.c_str() + cl + 16, nullptr, 10);
                if (response.size() >= head + 4 + body) {
                    break;
                }
            }
        }
    }
    return response;
}

Result<void> WsClient::send_frame(uint8_t opcode, const uint8_t* data, size_t len) {
    if (fd_ < 0) {
        return make_error(ErrorCode::ArgumentError, "not connected");
    }
    std::vector<uint8_t> frame;
    frame.reserve(len + 14);
    frame.push_back(uint8_t(0x80 | opcode));
    if (len < 126) {
        frame.push_back(uint8_t(0x80 | len));
    } else if (len <= 0xFFFF) {
        frame.push_back(0x80 | 126);
        frame.push_back(uint8_t(len >> 8));
        frame.push_back(uint8_t(len));
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(uint8_t(uint64_t(len) >> (8 * i)));
        }
    }
    static thread_local std::mt19937 rng{std::random_device{}()};
    const uint32_t mask = rng();
    for (int i = 0; i < 4; ++i) {
        frame.push_back(uint8_t(mask >> (8 * (3 - i))));
    }
    const size_t start = frame.size();
    frame.resize(start + len);
    for (size_t i = 0; i < len; ++i) {
        frame[start + i] = data[i] ^ frame[start - 4 + (i & 3)];
    }
    return write_all(frame.data(), frame.size());
}

Result<void> WsClient::send(const std::vector<uint8_t>& message) {
    return send_frame(0x2, message.data(), message.size());
}

Result<std::vector<uint8_t>> WsClient::receive(int timeout_ms) {
    if (fd_ < 0) {
        return make_error(ErrorCode::ArgumentError, "not connected");
    }
    std::vector<uint8_t> message;
    for (;;) {
        uint8_t head[2];
        if (auto r = read_exact(head, 2, timeout_ms); !r) {
            return r.error();
        }
        const bool fin = head[0] & 0x80;
        const uint8_t opcode = head[0] & 0x0F;
        uint64_t len = head[1] & 0x7F;
        if (head[1] & 0x80) {
            return make_error(ErrorCode::FormatError, "masked frame from the server");
        }
        if (len >= 126) {
            uint8_t ext[8];
            const size_t n = len == 126 ? 2 : 8;
            if (auto r = read_exact(ext, n, timeout_ms); !r) {
                return r.error();
            }
            len = 0;
            for (size_t i = 0; i < n; ++i) {
                len = len << 8 | ext[i];
            }
        }
        if (len > (64u << 20)) {
            return make_error(ErrorCode::FormatError, "frame over 64 MiB");
        }
        std::vector<uint8_t> payload(len);
        if (auto r = read_exact(payload.data(), payload.size(), timeout_ms); !r) {
            return r.error();
        }
        switch (opcode) {
        case 0x8: // close
            close();
            return make_error(ErrorCode::NotFound, "the server closed the connection");
        case 0x9: // ping
            if (auto r = send_frame(0xA, payload.data(), payload.size()); !r) {
                return r.error();
            }
            continue;
        case 0xA: // pong
            continue;
        case 0x1:
            return make_error(ErrorCode::FormatError, "text frame from the server");
        case 0x0:
        case 0x2:
            message.insert(message.end(), payload.begin(), payload.end());
            if (fin) {
                return message;
            }
            continue;
        default:
            return make_error(ErrorCode::FormatError, "unknown frame opcode");
        }
    }
}

} // namespace prometheia::server

// SPDX-License-Identifier: GPL-2.0-or-later
#include "ws_client.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <random>

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
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    buffered_.clear();
}

Result<void> WsClient::connect(const std::string& host, int port, const std::string& path) {
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
    char buf[1024];
    while (response.find("\r\n\r\n") == std::string::npos) {
        pollfd pfd{fd_, POLLIN, 0};
        if (::poll(&pfd, 1, 10000) <= 0) {
            return make_error(ErrorCode::IoError, "no WebSocket upgrade response");
        }
        const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
        if (n <= 0) {
            return make_error(ErrorCode::IoError, "connection closed during the upgrade");
        }
        response.append(buf, size_t(n));
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

Result<void> WsClient::read_exact(uint8_t* data, size_t len, int timeout_ms) {
    const size_t from_buffer = std::min(len, buffered_.size());
    std::memcpy(data, buffered_.data(), from_buffer);
    buffered_.erase(buffered_.begin(), buffered_.begin() + long(from_buffer));
    size_t got = from_buffer;
    while (got < len) {
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
        const ssize_t n = ::recv(fd_, data + got, len - got, 0);
        if (n == 0) {
            return make_error(ErrorCode::NotFound, "the server closed the connection");
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return io_error("recv");
        }
        got += size_t(n);
    }
    return {};
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

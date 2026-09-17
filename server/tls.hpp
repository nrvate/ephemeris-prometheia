// SPDX-License-Identifier: GPL-2.0-or-later
//
// TLS certificate handling for prometheiad (built when OpenSSL is found;
// PROMETHEIA_TLS). uSockets floors the protocol at TLS 1.2 and leaves the
// TLS 1.3 suites at OpenSSL's defaults; kTlsCiphers restricts TLS 1.2 to
// forward-secret AEAD suites.
#ifndef PROMETHEIA_SERVER_TLS_HPP
#define PROMETHEIA_SERVER_TLS_HPP

#include <string>

#include "prometheia/error.hpp"

namespace prometheia::server::tls {

inline constexpr const char* kTlsCiphers =
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
    "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305";

// Whether this build can serve TLS.
bool available();

// Loads the pair into a scratch context and checks what a client would
// refuse or the server would fail on: unreadable or non-PEM files, a key
// that does not match, a certificate expired or not yet valid. Returns the
// leaf's notAfter as text.
Result<std::string> check_pair(const std::string& cert, const std::string& key);

// Loads the pair into a live SSL_CTX (the next handshake uses it;
// established sessions are left alone). Call on the context's own loop.
Result<void> load_into(void* ssl_ctx, const std::string& cert, const std::string& key);

} // namespace prometheia::server::tls

#endif // PROMETHEIA_SERVER_TLS_HPP

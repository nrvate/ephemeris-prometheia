// SPDX-License-Identifier: GPL-2.0-or-later
#include "tls.hpp"

#ifdef PROMETHEIA_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#endif

namespace prometheia::server::tls {

#ifdef PROMETHEIA_TLS

namespace {

std::string openssl_error() {
    char text[256] = "";
    if (const unsigned long e = ERR_get_error()) {
        ERR_error_string_n(e, text, sizeof text);
    }
    ERR_clear_error();
    return text;
}

Result<void> use_pair(SSL_CTX* ctx, const std::string& cert, const std::string& key) {
    if (SSL_CTX_use_certificate_chain_file(ctx, cert.c_str()) != 1) {
        return make_error(ErrorCode::IoError,
                          "cannot read certificate " + cert + ": " + openssl_error());
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key.c_str(), SSL_FILETYPE_PEM) != 1) {
        // OpenSSL checks the pair while loading the key.
        if (ERR_GET_REASON(ERR_peek_last_error()) == X509_R_KEY_VALUES_MISMATCH) {
            ERR_clear_error();
            return make_error(ErrorCode::FormatError,
                              "private key " + key + " does not match certificate " + cert);
        }
        return make_error(ErrorCode::IoError,
                          "cannot read private key " + key + ": " + openssl_error());
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        ERR_clear_error();
        return make_error(ErrorCode::FormatError,
                          "private key " + key + " does not match certificate " + cert);
    }
    return {};
}

} // namespace

bool available() {
    return true;
}

Result<std::string> check_pair(const std::string& cert, const std::string& key) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        return make_error(ErrorCode::IoError, "cannot create a TLS context: " + openssl_error());
    }
    Result<std::string> out = std::string();
    if (auto r = use_pair(ctx, cert, key); !r) {
        out = r.error();
    } else {
        X509* leaf = SSL_CTX_get0_certificate(ctx);
        const ASN1_TIME* not_after = X509_get0_notAfter(leaf);
        char expiry[64] = "?";
        if (BIO* bio = BIO_new(BIO_s_mem())) {
            if (ASN1_TIME_print(bio, not_after) == 1) {
                const int n = BIO_read(bio, expiry, sizeof expiry - 1);
                expiry[n > 0 ? n : 0] = '\0';
            }
            BIO_free(bio);
        }
        if (X509_cmp_current_time(not_after) <= 0) {
            out = make_error(ErrorCode::ArgumentError,
                             "certificate " + cert + " expired on " + expiry);
        } else if (X509_cmp_current_time(X509_get0_notBefore(leaf)) > 0) {
            out = make_error(ErrorCode::ArgumentError, "certificate " + cert + " is not valid yet");
        } else {
            out = std::string(expiry);
        }
    }
    SSL_CTX_free(ctx);
    return out;
}

Result<void> load_into(void* ssl_ctx, const std::string& cert, const std::string& key) {
    return use_pair(static_cast<SSL_CTX*>(ssl_ctx), cert, key);
}

#else

bool available() {
    return false;
}

Result<std::string> check_pair(const std::string&, const std::string&) {
    return make_error(ErrorCode::ArgumentError, "this build has no TLS (OpenSSL was not found)");
}

Result<void> load_into(void*, const std::string&, const std::string&) {
    return make_error(ErrorCode::ArgumentError, "this build has no TLS (OpenSSL was not found)");
}

#endif

} // namespace prometheia::server::tls

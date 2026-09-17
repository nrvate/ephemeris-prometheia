// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheiad over real sockets: an in-process WsServer (two event loops on
// the synthetic linear kernel) and the reference WebSocket client. The
// protocol details are tested socket-free in test_server.cpp; this checks
// the transport: upgrade, framing both ways, chunk streaming, several
// connections, and closing after a protocol error.
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "dataset.hpp"
#include "synthetic_spk.hpp"
#include "ws_client.hpp"
#include "ws_server.hpp"

#include <doctest/doctest.h>

using namespace prometheia::server;
using synth::TempFile;

namespace {

WsOptions test_options() {
    WsOptions o;
    o.port = 0;
    o.bind = "127.0.0.1";
    o.threads = 2;
    o.config.engine = "Prometheia 0.1.0, synthetic kernel";
    o.config.dataset_id = "synthetic/test#00000000";
    return o;
}

struct Running {
    TempFile tf{"prometheiad"};
    std::unique_ptr<WsServer> server;

    explicit Running(WsOptions o = test_options()) {
        synth::write_linear_spk(tf.path);
        const std::string path = tf.path.string();
        server = std::make_unique<WsServer>(o, [path] { return Engine::open(path); });
        const auto r = server->start();
        REQUIRE_MESSAGE(r.ok(), r.error().message);
        REQUIRE(server->port() > 0);
    }
};

std::vector<uint8_t> message(uint16_t type, uint32_t request_id, const uint8_t* payload,
                             size_t len) {
    std::vector<uint8_t> out;
    eph::WriteEnvelope(&out, type, request_id, len);
    out.insert(out.end(), payload, payload + len);
    return out;
}

uint16_t type_of(const std::vector<uint8_t>& m) {
    REQUIRE(m.size() >= eph::kEnvelopeSize);
    return uint16_t(m[4] | (uint16_t(m[5]) << 8));
}

uint32_t id_of(const std::vector<uint8_t>& m) {
    REQUIRE(m.size() >= eph::kEnvelopeSize);
    return uint32_t(m[8]) | (uint32_t(m[9]) << 8) | (uint32_t(m[10]) << 16) |
           (uint32_t(m[11]) << 24);
}

std::vector<uint8_t> hello_message() {
    eph::Hello h;
    h.clientName = "test";
    std::vector<uint8_t> payload;
    eph::EncodeHello(&payload, h);
    return message(eph::kMsgHello, 0, payload.data(), payload.size());
}

void connect_and_hello(WsClient& c, int port) {
    const auto r = c.connect("127.0.0.1", port);
    REQUIRE_MESSAGE(r.ok(), r.error().message);
    REQUIRE(c.send(hello_message()).ok());
    const auto w = c.receive();
    REQUIRE_MESSAGE(w.ok(), w.error().message);
    REQUIRE(type_of(w.value()) == eph::kMsgWelcome);
}

} // namespace

TEST_CASE("prometheiad_round_trip") {
    Running run;
    Engine check = Engine::open(run.tf.path.string()).value();
    WsClient c;
    connect_and_hello(c, run.server->port());

    eph::Request req; // one default profile: geocentric apparent
    req.profiles.emplace_back();
    // TT grid, geocentric apparent ecliptic of date, f64
    req.start.jd1 = 2451545.0;
    req.stepNs = 3600LL * 1000000000LL;
    req.nTime = 1200;
    req.chunkRows = 500;
    eph::Object sun, jup, unmapped;
    sun.naif = 10;
    jup.naif = 5;
    unmapped.naif = 1;
    req.objs = {sun, jup, unmapped};
    std::vector<uint8_t> payload;
    eph::EncodeRequest(&payload, req);
    const uint32_t request_id = 9;
    REQUIRE(c.send(message(eph::kMsgRequest, request_id, payload.data(), payload.size())).ok());

    std::vector<double> cols(3 * 1200 * 6, -1.0);
    std::vector<eph::Meta> meta;
    uint32_t rows = 0, chunks = 0;
    while (rows < 1200) {
        const auto m = c.receive();
        REQUIRE_MESSAGE(m.ok(), m.error().message);
        REQUIRE(type_of(m.value()) == eph::kMsgData);
        CHECK(id_of(m.value()) == request_id);
        eph::DataChunk d;
        std::string why;
        REQUIRE_MESSAGE(eph::ParseData(m.value().data() + eph::kEnvelopeSize,
                                       m.value().size() - eph::kEnvelopeSize, &d, &why) == eph::kOk,
                        why);
        CHECK(d.chunkIndex == chunks++);
        CHECK(d.iTime == rows);
        CHECK(d.totalRows == 1200);
        CHECK(d.nObj == 3);
        if (d.flags & eph::kChunkMeta) {
            REQUIRE(d.meta.size() == 3);
            meta = d.meta;
        }
        for (uint32_t o = 0; o < 3; ++o) {
            for (uint32_t r = 0; r < d.nRows; ++r) {
                for (int k = 0; k < 6; ++k) {
                    cols[(size_t(o) * 1200 + d.iTime + r) * 6 + k] =
                        d.values[(size_t(o) * d.nRows + r) * 6 + k];
                }
            }
        }
        rows += d.nRows;
    }
    CHECK(chunks == 3);
    REQUIRE(meta.size() == 3);
    CHECK(meta[0].rowsOk == 1200);
    CHECK(meta[0].name == "Sun");
    CHECK(meta[0].errCode == eph::kOErrNone);
    CHECK(meta[1].name == "Jupiter");
    CHECK(meta[2].rowsOk == 0);
    CHECK(meta[2].errCode == eph::kOErrUnknownBody);

    CalcOptions o;
    o.sigma = false;
    for (uint32_t r = 0; r < 1200; r += 97) {
        const double jd = 2451545.0 + r * 3600.0 / 86400.0;
        const Position p = check.calc(5, jd, o).value().pos;
        CHECK(cols[(1200 + r) * 6] == p.lon_deg);
        CHECK(cols[(1200 + r) * 6 + 5] == p.dist_speed);
        CHECK(std::isnan(cols[(2 * 1200 + r) * 6]));
    }

    // PING is answered on the same connection (requestId 0, 3.2).
    REQUIRE(c.send(message(eph::kMsgPing, 0, nullptr, 0)).ok());
    const auto pong = c.receive();
    REQUIRE(pong.ok());
    CHECK(type_of(pong.value()) == eph::kMsgPong);
    CHECK(id_of(pong.value()) == 0);
}

TEST_CASE("prometheiad_connections") {
    Running run;
    const int port = run.server->port();

    SUBCASE("several clients at once") {
        constexpr int kClients = 6;
        std::vector<std::thread> threads;
        std::vector<int> ok(kClients, 0);
        for (int i = 0; i < kClients; ++i) {
            threads.emplace_back([&, i] {
                WsClient c;
                if (!c.connect("127.0.0.1", port).ok() || !c.send(hello_message()).ok()) {
                    return;
                }
                auto w = c.receive();
                eph::Request req; // one default profile: geocentric apparent
                req.profiles.emplace_back();

                req.start.jd1 = 2451545.0 + i;
                req.stepNs = 60LL * 1000000000LL;
                req.nTime = 50;
                eph::Object sun;
                sun.naif = 10;
                req.objs = {sun};
                if (!w.ok()) {
                    return;
                }
                std::vector<uint8_t> payload;
                eph::EncodeRequest(&payload, req);
                if (!c.send(message(eph::kMsgRequest, uint32_t(100 + i), payload.data(),
                                    payload.size()))
                         .ok()) {
                    return;
                }
                auto d = c.receive();
                ok[i] = d.ok() && type_of(d.value()) == eph::kMsgData &&
                        id_of(d.value()) == uint32_t(100 + i);
            });
        }
        for (auto& t : threads) {
            t.join();
        }
        for (int i = 0; i < kClients; ++i) {
            CHECK(ok[i] == 1);
        }
    }
    SUBCASE("REQUEST before HELLO: ERROR, then the server closes") {
        WsClient c;
        REQUIRE(c.connect("127.0.0.1", port).ok());
        eph::Request req; // one default profile: geocentric apparent
        req.profiles.emplace_back();

        req.start.jd1 = 2451545.0;
        eph::Object sun;
        sun.naif = 10;
        req.objs = {sun};
        std::vector<uint8_t> payload;
        eph::EncodeRequest(&payload, req);
        REQUIRE(c.send(message(eph::kMsgRequest, 3, payload.data(), payload.size())).ok());
        const auto e = c.receive();
        REQUIRE(e.ok());
        CHECK(type_of(e.value()) == eph::kMsgError);
        const auto closed = c.receive(5000);
        REQUIRE(!closed.ok());
        CHECK(closed.error().code == prometheia::ErrorCode::NotFound);
    }
    SUBCASE("stop closes the connections") {
        WsClient c;
        connect_and_hello(c, port);
        run.server->stop();
        run.server->join();
        const auto closed = c.receive(5000);
        REQUIRE(!closed.ok());
        CHECK(closed.error().code == prometheia::ErrorCode::NotFound);
    }
}

TEST_CASE("prometheiad_operations") {
    SUBCASE("connection caps refuse at the upgrade with 503") {
        WsOptions o = test_options();
        o.limits.max_conns_per_addr = 2;
        Running run(o);
        WsClient a, b, c;
        REQUIRE(a.connect("127.0.0.1", run.server->port()).ok());
        REQUIRE(b.connect("127.0.0.1", run.server->port()).ok());
        const auto refused = c.connect("127.0.0.1", run.server->port());
        REQUIRE(!refused.ok());
        CHECK(refused.error().message == "upgrade refused: HTTP/1.1 503 Service Unavailable");
        a.close();
        // The place comes back once the server has seen the close.
        bool admitted = false;
        for (int i = 0; i < 50 && !admitted; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            admitted = c.connect("127.0.0.1", run.server->port()).ok();
        }
        CHECK(admitted);
        CHECK(run.server->metrics().find("prometheiad_refused_connections_total 1\n") !=
              std::string::npos);
    }
    SUBCASE("no HELLO in time: closed") {
        WsOptions o = test_options();
        o.hello_timeout_ms = 100;
        Running run(o);
        WsClient c;
        REQUIRE(c.connect("127.0.0.1", run.server->port()).ok());
        const auto t0 = std::chrono::steady_clock::now();
        const auto closed = c.receive(3000);
        REQUIRE(!closed.ok());
        CHECK(closed.error().code == prometheia::ErrorCode::NotFound);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
        // A connection that said HELLO stays.
        WsClient d;
        connect_and_hello(d, run.server->port());
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        REQUIRE(d.send(message(eph::kMsgPing, 0, nullptr, 0)).ok());
        CHECK(d.receive(1000).ok());
    }
    SUBCASE("healthz, readyz, metrics") {
        Running run;
        const int port = run.server->port();
        const auto health = WsClient::http_get("127.0.0.1", port, "/healthz");
        REQUIRE_MESSAGE(health.ok(), health.error().message);
        CHECK(health.value().rfind("HTTP/1.1 200 OK", 0) == 0);
        CHECK(health.value().find("\r\n\r\nok\n") != std::string::npos);
        const auto ready = WsClient::http_get("127.0.0.1", port, "/readyz");
        REQUIRE(ready.ok());
        CHECK(ready.value().find("\r\n\r\nready\n") != std::string::npos);

        WsClient c;
        connect_and_hello(c, port);
        const auto metrics = WsClient::http_get("127.0.0.1", port, "/metrics");
        REQUIRE(metrics.ok());
        CHECK(metrics.value().find("prometheiad_hellos_total 1\n") != std::string::npos);
        CHECK(metrics.value().find("prometheiad_connections_open 1\n") != std::string::npos);
        CHECK(metrics.value().find("prometheiad_draining 0\n") != std::string::npos);
    }
    SUBCASE("drain: answers in flight finish, then 1001, then the loops return") {
        Running run;
        const int port = run.server->port();
        WsClient c;
        connect_and_hello(c, port);
        eph::Request req; // one default profile: geocentric apparent
        req.profiles.emplace_back();

        req.start.jd1 = 2451545.0;
        req.stepNs = 60LL * 1000000000LL;
        req.nTime = 3000;
        req.chunkRows = 100;
        eph::Object sun;
        sun.naif = 10;
        req.objs = {sun};
        std::vector<uint8_t> payload;
        eph::EncodeRequest(&payload, req);
        REQUIRE(c.send(message(eph::kMsgRequest, 5, payload.data(), payload.size())).ok());
        // Let the request arrive, then drain before reading anything.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        run.server->drain(5);
        CHECK(run.server->draining());
        CHECK(run.server->metrics().find("prometheiad_draining 1\n") != std::string::npos);
        uint32_t chunks = 0;
        for (;;) {
            const auto m = c.receive(5000);
            if (!m.ok()) {
                CHECK(m.error().code == prometheia::ErrorCode::NotFound);
                break;
            }
            CHECK(type_of(m.value()) == eph::kMsgData);
            ++chunks;
        }
        CHECK(chunks == 30);
        run.server->join();
        WsClient late;
        CHECK(!late.connect("127.0.0.1", port).ok());
    }
    SUBCASE("a port another server listens on is refused") {
        Running run;
        WsOptions o = test_options();
        o.port = run.server->port();
        WsServer second(o, [path = run.tf.path.string()] { return Engine::open(path); });
        const auto r = second.start();
        REQUIRE(!r.ok());
        CHECK(r.error().message ==
              "another server is listening on port " + std::to_string(run.server->port()));
    }
}

#ifdef PROMETHEIA_TLS
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

namespace {

// A self-signed P-256 certificate for "localhost", valid for a day.
void write_self_signed(const std::string& cert_path, const std::string& key_path) {
    EVP_PKEY* key = EVP_EC_gen("P-256");
    REQUIRE(key != nullptr);
    X509* x = X509_new();
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), -60);
    X509_gmtime_adj(X509_getm_notAfter(x), 86400);
    X509_set_pubkey(x, key);
    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(x, name);
    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, x, x, nullptr, nullptr, 0);
    X509_EXTENSION* san = X509V3_EXT_conf_nid(nullptr, &v3, NID_subject_alt_name, "DNS:localhost");
    X509_add_ext(x, san, -1);
    X509_EXTENSION_free(san);
    REQUIRE(X509_sign(x, key, EVP_sha256()) > 0);
    FILE* f = std::fopen(cert_path.c_str(), "wb");
    PEM_write_X509(f, x);
    std::fclose(f);
    f = std::fopen(key_path.c_str(), "wb");
    PEM_write_PrivateKey(f, key, nullptr, nullptr, 0, nullptr, nullptr);
    std::fclose(f);
    X509_free(x);
    EVP_PKEY_free(key);
}

} // namespace

TEST_CASE("prometheiad_tls") {
    TempFile cert("tls-cert"), key("tls-key"), other_cert("tls-other-cert"),
        other_key("tls-other-key");
    write_self_signed(cert.path.string(), key.path.string());
    write_self_signed(other_cert.path.string(), other_key.path.string());

    WsOptions o = test_options();
    o.tls_cert = cert.path.string();
    o.tls_key = key.path.string();
    Running run(o);
    CHECK(run.server->tls());
    const int port = run.server->port();

    WsTlsOptions tls;
    tls.enabled = true;
    tls.ca_file = cert.path.string();
    tls.sni = "localhost";

    WsClient c;
    const auto r = c.connect("127.0.0.1", port, "/", tls);
    REQUIRE_MESSAGE(r.ok(), r.error().message);
    REQUIRE(c.send(hello_message()).ok());
    const auto w = c.receive();
    REQUIRE(w.ok());
    CHECK(type_of(w.value()) == eph::kMsgWelcome);

    const auto health = WsClient::http_get("127.0.0.1", port, "/healthz", tls);
    REQUIRE_MESSAGE(health.ok(), health.error().message);
    CHECK(health.value().find("\r\n\r\nok\n") != std::string::npos);

    // Verification fails against another anchor, and plain ws:// is not served.
    WsTlsOptions wrong = tls;
    wrong.ca_file = other_cert.path.string();
    WsClient d;
    CHECK(!d.connect("127.0.0.1", port, "/", wrong).ok());
    WsClient plain;
    CHECK(!plain.connect("127.0.0.1", port).ok());

    CHECK(run.server->reload_tls().ok());
    CHECK(run.server->metrics().find("tls=\"1\"") != std::string::npos);

    // A key that does not match its certificate fails before any loop starts.
    WsOptions bad = test_options();
    bad.tls_cert = cert.path.string();
    bad.tls_key = other_key.path.string();
    WsServer refused(bad, [p = run.tf.path.string()] { return Engine::open(p); });
    const auto e = refused.start();
    REQUIRE(!e.ok());
    CHECK(e.error().message.find("does not match certificate") != std::string::npos);
}
#endif

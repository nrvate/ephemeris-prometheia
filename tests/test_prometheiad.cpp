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
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "synthetic_spk.hpp"
#include "ws_client.hpp"
#include "ws_server.hpp"

#include <doctest/doctest.h>

using namespace prometheia::server;
using synth::TempFile;

namespace {

constexpr const char* kTestMap = "# invented numbers, test only\n"
                                 "body 900 10 Test Sun\n"
                                 "body 905 5\n"
                                 "flag 5 equatorial\n";

WsOptions test_options() {
    WsOptions o;
    o.port = 0;
    o.bind = "127.0.0.1";
    o.threads = 2;
    return o;
}

struct Running {
    TempFile tf{"prometheiad"};
    std::unique_ptr<WsServer> server;

    explicit Running(WsOptions o = test_options()) {
        synth::write_linear_spk(tf.path);
        const std::string path = tf.path.string();
        server = std::make_unique<WsServer>(o, WireMap::parse(kTestMap).value(),
                                            [path] { return Engine::open(path); });
        const auto r = server->start();
        REQUIRE_MESSAGE(r.ok(), r.error().message);
        REQUIRE(server->port() > 0);
    }
};

std::vector<uint8_t> hello_message() {
    uint8_t buf[eph::kHelloMaxSize];
    uint32_t len = 0;
    eph::buildHello(buf, eph::kCapFloat32, 1, "test", &len);
    return eph::makeMessage(eph::kMsgHello, 1, buf, len);
}

std::vector<uint8_t> request_message(const eph::Request& req, uint32_t id) {
    std::vector<uint8_t> payload;
    eph::buildRequest(&payload, req);
    return eph::makeMessage(eph::kMsgRequest, id, payload.data(), payload.size());
}

uint16_t type_of(const std::vector<uint8_t>& m) {
    REQUIRE(m.size() >= eph::kEnvelopeSize);
    return eph::getU16(m.data() + 4);
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

    eph::Request req;
    req.jdStart = 2451545.0;
    req.stepSeconds = 3600;
    req.nTime = 1200;
    req.chunkRows = 500;
    req.iflag = 1u << 5;
    eph::ObjSpec sun, jup, unmapped;
    sun.id = 900;
    jup.id = 905;
    unmapped.id = 1;
    req.objs = {sun, jup, unmapped};
    REQUIRE(c.send(request_message(req, 9)).ok());

    std::vector<double> cols(3 * 1200 * 6, -1.0);
    uint32_t rows = 0, chunks = 0;
    while (rows < 1200) {
        const auto m = c.receive();
        REQUIRE_MESSAGE(m.ok(), m.error().message);
        REQUIRE(type_of(m.value()) == eph::kMsgData);
        CHECK(eph::getU32(m.value().data() + 8) == 9);
        eph::Reader rd(m.value().data() + eph::kEnvelopeSize,
                       m.value().size() - eph::kEnvelopeSize);
        CHECK(rd.u32() == chunks++);
        const uint32_t i_time = rd.u32(), n = rd.u32();
        CHECK(i_time == rows);
        rd.u8();
        CHECK(rd.u32() == 3);
        std::vector<int32_t> ret;
        for (int o = 0; o < 3; ++o) {
            ret.push_back(rd.i32());
            rd.skip(eph::kDataMetaSize - 4);
        }
        CHECK(ret[0] == int32_t(1u << 5));
        CHECK(ret[2] == -1);
        for (int o = 0; o < 3; ++o) {
            for (uint32_t k = 0; k < n * 6; ++k) {
                cols[(size_t(o) * 1200 + i_time) * 6 + k] = rd.f64();
            }
        }
        CHECK(rd.left() == 0);
        rows += n;
    }
    CHECK(chunks == 3);

    CalcOptions o;
    o.sigma = false;
    o.coords = Coords::Equatorial;
    for (uint32_t r = 0; r < 1200; r += 97) {
        const double jd = 2451545.0 + r * 3600.0 / 86400.0;
        const Position p = check.calc_ut(5, jd, o).value().pos;
        CHECK(cols[(1200 + r) * 6] == p.lon_deg);
        CHECK(cols[(1200 + r) * 6 + 5] == p.dist_speed);
        CHECK(std::isnan(cols[(2 * 1200 + r) * 6]));
    }

    // PING is answered on the same connection.
    REQUIRE(c.send(eph::buildPing(77)).ok());
    const auto pong = c.receive();
    REQUIRE(pong.ok());
    CHECK(type_of(pong.value()) == eph::kMsgPong);
    CHECK(eph::getU32(pong.value().data() + 8) == 77);
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
                eph::Request req;
                req.jdStart = 2451545.0 + i;
                req.stepSeconds = 60;
                req.nTime = 50;
                eph::ObjSpec sun;
                sun.id = 900;
                req.objs = {sun};
                if (!w.ok() || !c.send(request_message(req, 100 + i)).ok()) {
                    return;
                }
                auto d = c.receive();
                ok[i] = d.ok() && eph::getU16(d.value().data() + 4) == eph::kMsgData &&
                        eph::getU32(d.value().data() + 8) == uint32_t(100 + i);
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
        eph::Request req;
        req.jdStart = 2451545.0;
        req.nTime = 1;
        eph::ObjSpec sun;
        sun.id = 900;
        req.objs = {sun};
        REQUIRE(c.send(request_message(req, 3)).ok());
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
        REQUIRE(d.send(eph::buildPing(1)).ok());
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
        eph::Request req;
        req.jdStart = 2451545.0;
        req.stepSeconds = 60;
        req.nTime = 3000;
        req.chunkRows = 100;
        eph::ObjSpec sun;
        sun.id = 900;
        req.objs = {sun};
        REQUIRE(c.send(request_message(req, 5)).ok());
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
        WsServer second(o, WireMap(), [path = run.tf.path.string()] { return Engine::open(path); });
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
    WsServer refused(bad, WireMap(), [p = run.tf.path.string()] { return Engine::open(p); });
    const auto e = refused.start();
    REQUIRE(!e.ok());
    CHECK(e.error().message.find("does not match certificate") != std::string::npos);
}
#endif

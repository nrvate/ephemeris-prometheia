// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheiad over real sockets: an in-process WsServer (two event loops on
// the synthetic linear kernel) and the reference WebSocket client. The
// protocol details are tested socket-free in test_server.cpp; this checks
// the transport: upgrade, framing both ways, chunk streaming, several
// connections, and closing after a protocol error.
#include <cmath>
#include <cstring>
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

struct Running {
    TempFile tf{"prometheiad"};
    std::unique_ptr<WsServer> server;

    Running() {
        synth::write_linear_spk(tf.path);
        WsOptions o;
        o.port = 0;
        o.bind = "127.0.0.1";
        o.threads = 2;
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

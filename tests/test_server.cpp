// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheiad's protocol core: the wire map and the Session state machine,
// driven without sockets. The wire-map numbers here are invented for the
// test; they are not Astrolog's (docs/SERVER.md).
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "session.hpp"
#include "synthetic_spk.hpp"
#include "wire_map.hpp"

#include <doctest/doctest.h>

using namespace prometheia::server;
using synth::TempFile;

namespace {

constexpr const char* kTestMap = R"(# invented numbers, test only
body 900 10 Test Sun
body 905 5
asteroids 70000
flag 2 speed
flag 5 equatorial
flag 6 j2000
flag 9 xyz
flag 10 radians
flag 11 heliocentric
flag 12 topocentric
flag 13 barycentric
flag 14 sidereal
flag 20 ignore
sidereal 7 lahiri
)";

struct Reply {
    eph::Envelope env{};
    std::vector<uint8_t> payload;
};

std::vector<Reply> drain(Session& s) {
    std::vector<Reply> out;
    std::vector<uint8_t> msg;
    while (s.next(msg)) {
        Reply r;
        REQUIRE(msg.size() >= eph::kEnvelopeSize);
        r.env.version = msg[2];
        r.env.flags = msg[3];
        r.env.type = eph::getU16(msg.data() + 4);
        r.env.requestId = eph::getU32(msg.data() + 8);
        r.env.payloadLen = eph::getU32(msg.data() + 12);
        r.payload.assign(msg.begin() + eph::kEnvelopeSize, msg.end());
        CHECK(r.payload.size() == r.env.payloadLen);
        out.push_back(std::move(r));
    }
    return out;
}

std::string as_view(const std::vector<uint8_t>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
}

std::string hello(uint32_t proto, uint8_t envelope_version = eph::kProtoVersion) {
    uint8_t buf[eph::kHelloMaxSize];
    uint32_t len = 0;
    eph::buildHello(buf, eph::kCapFloat32, 1, "test", &len, nullptr, uint8_t(proto));
    return as_view(eph::makeMessage(eph::kMsgHello, 1, buf, len, 0, envelope_version));
}

std::string request(const eph::Request& req, uint32_t id) {
    std::vector<uint8_t> payload;
    eph::buildRequest(&payload, req);
    return as_view(eph::makeMessage(eph::kMsgRequest, id, payload.data(), payload.size()));
}

eph::ObjSpec obj(uint32_t id) {
    eph::ObjSpec o;
    o.id = id;
    return o;
}

eph::ErrorMsg error_of(const Reply& r) {
    eph::ErrorMsg e{};
    REQUIRE(r.env.type == eph::kMsgError);
    REQUIRE(eph::parseError(r.payload.data(), r.payload.size(), &e));
    return e;
}

// A decoded DATA answer: all chunks joined back into object-major columns.
struct Data {
    std::vector<int32_t> ret;
    std::vector<std::string> serr, name;
    std::vector<double> cols;
    uint32_t chunks = 0;
};

Data join(const std::vector<Reply>& replies, uint32_t n_obj, uint32_t n_time) {
    Data d;
    d.cols.assign(size_t(n_obj) * n_time * 6, -1.0);
    for (const Reply& r : replies) {
        if (r.env.type != eph::kMsgData) {
            continue;
        }
        eph::Reader rd(r.payload.data(), r.payload.size());
        CHECK(rd.u32() == d.chunks);
        const uint32_t i_time = rd.u32(), rows = rd.u32();
        const uint8_t precision = rd.u8();
        CHECK(rd.u32() == n_obj);
        if (d.chunks == 0) {
            for (uint32_t o = 0; o < n_obj; ++o) {
                eph::DataMetaWire m{};
                d.ret.push_back(rd.i32());
                rd.i32();
                rd.raw(m.serr, sizeof m.serr);
                rd.raw(m.name, sizeof m.name);
                d.serr.emplace_back(m.serr, strnlen(m.serr, sizeof m.serr));
                d.name.emplace_back(m.name, strnlen(m.name, sizeof m.name));
            }
        } else {
            rd.skip(n_obj * eph::kDataMetaSize);
        }
        for (uint32_t o = 0; o < n_obj; ++o) {
            for (uint32_t k = 0; k < rows * 6; ++k) {
                d.cols[(size_t(o) * n_time + i_time) * 6 + k] =
                    precision == eph::kPrecF32 ? double(rd.f32()) : rd.f64();
            }
        }
        CHECK(rd.ok());
        CHECK(rd.left() == 0);
        ++d.chunks;
    }
    return d;
}

struct Fixture {
    TempFile tf{"server"};
    WireMap map = WireMap::parse(kTestMap).value();
    ServerConfig config;
    std::unique_ptr<LoopContext> ctx;
    std::unique_ptr<Session> session;

    Fixture() {
        ctx = std::make_unique<LoopContext>(synth::open_synthetic(tf), map, config);
        session = std::make_unique<Session>(*ctx);
    }
    void welcome() {
        CHECK(session->on_message(hello(3), true));
        const auto r = drain(*session);
        REQUIRE(r.size() == 1);
        REQUIRE(r[0].env.type == eph::kMsgWelcome);
    }
};

eph::Request base_request(double jd, uint32_t n_time) {
    eph::Request req;
    req.jdStart = jd;
    req.stepSeconds = 3600 * 6;
    req.nTime = n_time;
    return req;
}

} // namespace

TEST_CASE("server_wire_map") {
    auto map = WireMap::parse(kTestMap);
    REQUIRE(map.ok());
    const WireMap& m = map.value();
    CHECK(!m.empty());
    CHECK(m.body(900)->naif_id == 10);
    CHECK(m.body(900)->name == "Test Sun");
    CHECK(m.body(905)->name.empty());
    CHECK(!m.body(901));
    CHECK(m.body(70001)->naif_id == 20000001);
    CHECK(!m.body(70000));
    CHECK(m.flag(5) == FlagMeaning::Equatorial);
    CHECK(m.flag(0) == FlagMeaning::None);
    CHECK(m.flag(40) == FlagMeaning::None);
    CHECK(m.sidereal(7) == SiderealMode::Lahiri);
    CHECK(!m.sidereal(8));
    CHECK(flag_meaning_name(FlagMeaning::NoNutation) == "no-nutation");

    CHECK(WireMap::parse("# nothing\n\n").value().empty());
    const auto bad = [](const char* text) {
        auto r = WireMap::parse(text);
        return r.ok() ? std::string() : r.error().message;
    };
    CHECK(bad("flag 3 sideways\n") == "wire map line 1: unknown flag meaning 'sideways'");
    CHECK(bad("\nflag 32 speed\n") == "wire map line 2: expected: flag <bit 0-31> <meaning>");
    CHECK(bad("body 1 10\nbody 1 11\n") == "wire map line 2: body 1 is mapped twice");
    CHECK(bad("body x 10\n") == "wire map line 1: expected: body <wire-id> <naif-id> [name]");
    CHECK(bad("flag 1 speed\nflag 1 xyz\n") == "wire map line 2: flag bit 1 is mapped twice");
    CHECK(bad("sidereal 1 tropical\n") == "wire map line 1: unknown zodiac 'tropical'");
    CHECK(bad("asteroids 1\nasteroids 2\n") == "wire map line 2: asteroids is given twice");
    CHECK(bad("planet 1 2\n") == "wire map line 1: unknown entry 'planet'");
    CHECK(!WireMap::load("/nonexistent/wire.map").ok());
}

TEST_CASE("server_handshake") {
    Fixture f;
    Session& s = *f.session;

    SUBCASE("REQUEST before HELLO closes") {
        eph::Request req = base_request(2451545.0, 1);
        req.objs = {obj(900)};
        CHECK(!s.on_message(request(req, 5), true));
        const auto r = drain(s);
        REQUIRE(r.size() == 1);
        CHECK(error_of(r[0]).code == eph::kErrBad);
        CHECK(error_of(r[0]).requestId == 5);
    }
    SUBCASE("version 3 WELCOME") {
        CHECK(s.on_message(hello(3), true));
        const auto r = drain(s);
        REQUIRE(r.size() == 1);
        CHECK(r[0].env.type == eph::kMsgWelcome);
        CHECK(r[0].env.version == 3);
        eph::Welcome w;
        REQUIRE(eph::parseWelcome(r[0].payload.data(), r[0].payload.size(), &w));
        CHECK(w.protoVersion == 3);
        CHECK(w.caps == eph::kCapFloat32);
        CHECK(w.swissephVersion == 0);
        CHECK(w.maxObjs == eph::kMaxObjs);
        CHECK(w.maxCells == eph::kMaxCellsDefault);
        CHECK(w.serverVersion == kServerVersion);
        CHECK(s.version() == 3);
    }
    SUBCASE("a version 2 client is answered in version 2") {
        CHECK(s.on_message(hello(2, 2), true));
        const auto r = drain(s);
        REQUIRE(r.size() == 1);
        CHECK(r[0].env.version == 2);
        CHECK(s.version() == 2);
        // Later HELLOs keep the first one's version.
        CHECK(s.on_message(hello(3), true));
        CHECK(drain(s)[0].env.version == 2);
    }
    SUBCASE("a newer client is answered in version 3") {
        uint8_t buf[eph::kHelloMaxSize];
        uint32_t len = 0;
        eph::buildHello(buf, 0, 1, "future", &len, nullptr, 3);
        eph::putU32(buf, 9);
        CHECK(s.on_message(as_view(eph::makeMessage(eph::kMsgHello, 1, buf, len)), true));
        CHECK(drain(s)[0].env.version == 3);
    }
    SUBCASE("too old: HELLO below the minimum") {
        CHECK(!s.on_message(hello(1, 2), true));
        const auto r = drain(s);
        REQUIRE(r.size() == 1);
        CHECK(error_of(r[0]).code == eph::kErrVersion);
    }
    SUBCASE("too old: envelope below the minimum, refused in its own version") {
        auto msg = eph::makeMessage(eph::kMsgHello, 1, nullptr, 0, 0, 1);
        CHECK(!s.on_message(as_view(msg), true));
        const auto r = drain(s);
        REQUIRE(r.size() == 1);
        CHECK(r[0].env.version == 1);
        CHECK(error_of(r[0]).code == eph::kErrVersion);
    }
    SUBCASE("malformed frames") {
        CHECK(s.on_message("text", false));
        CHECK(s.on_message("short", true));
        auto msg = eph::makeMessage(eph::kMsgPing, 3, nullptr, 0);
        msg[0] ^= 0xFF;
        CHECK(s.on_message(as_view(msg), true));
        msg = eph::makeMessage(eph::kMsgPing, 3, nullptr, 0);
        eph::putU32(msg.data() + 12, 4);
        CHECK(s.on_message(as_view(msg), true));
        msg = eph::makeMessage(eph::kMsgPing, 3, nullptr, 0, eph::kEnvFlagZstd);
        CHECK(s.on_message(as_view(msg), true));
        msg = eph::makeMessage(eph::kMsgPing, 3, nullptr, 0);
        msg[3] = 0x80;
        CHECK(s.on_message(as_view(msg), true));
        const auto r = drain(s);
        REQUIRE(r.size() == 6);
        for (const Reply& e : r) {
            CHECK(error_of(e).code == eph::kErrBad);
        }
    }
    SUBCASE("ping, pong, unknown type") {
        f.welcome();
        CHECK(s.on_message(as_view(eph::buildPing(42)), true));
        CHECK(s.on_message(as_view(eph::buildPong(43)), true));
        CHECK(s.on_message(as_view(eph::makeMessage(99, 44, nullptr, 0)), true));
        const auto r = drain(s);
        REQUIRE(r.size() == 2);
        CHECK(r[0].env.type == eph::kMsgPong);
        CHECK(r[0].env.requestId == 42);
        CHECK(error_of(r[1]).code == eph::kErrUnknown);
        CHECK(error_of(r[1]).requestId == 44);
    }
}

TEST_CASE("server_request") {
    Fixture f;
    Session& s = *f.session;
    Engine check = synth::open_synthetic(f.tf);
    f.welcome();

    const double jd = 2451545.25;
    const uint32_t n_time = 7;
    eph::Request req = base_request(jd, n_time);
    eph::ObjSpec star;
    star.kind = eph::kObjStar;
    std::strcpy(star.name, "Sirius");
    eph::ObjSpec node = obj(905);
    node.kind = eph::kObjNodAps;
    node.point = eph::kPntNorthNode;
    req.objs = {obj(900), obj(905), obj(901), star, node};
    req.iflag = (1u << 2) | (1u << 20);
    req.chunkRows = 3;

    CHECK(s.on_message(request(req, 77), true));
    CHECK(s.queued_answers() == 1);
    const auto replies = drain(s);
    REQUIRE(replies.size() == 3); // rows 3 + 3 + 1
    for (const Reply& r : replies) {
        CHECK(r.env.type == eph::kMsgData);
        CHECK(r.env.requestId == 77);
        CHECK(r.env.flags == 0);
    }
    const Data d = join(replies, 5, n_time);
    CHECK(d.chunks == 3);

    // Mapped bodies: the engine's answer, bit for bit, at each row's UT.
    const auto opts = [] {
        CalcOptions o;
        o.sigma = false;
        return o;
    }();
    for (uint32_t o = 0; o < 2; ++o) {
        CHECK(d.ret[o] == int32_t(req.iflag));
        CHECK(d.serr[o].empty());
        for (uint32_t r = 0; r < n_time; ++r) {
            const auto res = check.calc_ut(o == 0 ? 10 : 5, jd + r * 0.25, opts);
            REQUIRE(res.ok());
            const Position& p = res.value().pos;
            const double* row = &d.cols[(size_t(o) * n_time + r) * 6];
            CHECK(row[0] == p.lon_deg);
            CHECK(row[1] == p.lat_deg);
            CHECK(row[2] == p.dist_au);
            CHECK(row[3] == p.lon_speed);
            CHECK(row[4] == p.lat_speed);
            CHECK(row[5] == p.dist_speed);
        }
    }
    CHECK(d.name[0] == "Test Sun");
    CHECK(d.name[1] == "SPK-ID 5");

    // Unsupported objects fail alone: NaN rows, retFlag -1, the reason.
    CHECK(d.serr[2] == "body 901 has no wire-map entry");
    CHECK(d.serr[3] == "fixed stars are not supported");
    CHECK(d.serr[4] == "nodes and apsides are not supported");
    for (uint32_t o = 2; o < 5; ++o) {
        CHECK(d.ret[o] == -1);
        CHECK(d.name[o].empty());
        for (size_t k = 0; k < n_time * 6; ++k) {
            CHECK(std::isnan(d.cols[size_t(o) * n_time * 6 + k]));
        }
    }

    SUBCASE("the same question again is a cache hit, whatever the delivery") {
        req.chunkRows = 500;
        req.precision = eph::kPrecF32;
        CHECK(s.on_message(request(req, 78), true));
        const auto again = drain(s);
        REQUIRE(again.size() == 1);
        CHECK(f.ctx->cache().hits() == 1);
        CHECK(f.ctx->cache().entries() == 1);
        const Data d32 = join(again, 5, n_time);
        for (size_t k = 0; k < d.cols.size(); ++k) {
            if (std::isnan(d.cols[k])) {
                CHECK(std::isnan(d32.cols[k]));
            } else {
                CHECK(d32.cols[k] == double(float(d.cols[k])));
            }
        }
    }
    SUBCASE("a row the engine cannot answer is NaN; the object keeps the rest") {
        eph::Request edge = base_request(jd + 60.0 * 365.25 - 1.0, 4);
        edge.stepSeconds = 86400;
        edge.objs = {obj(900)};
        CHECK(s.on_message(request(edge, 79), true));
        const Data e = join(drain(s), 1, 4);
        CHECK(e.ret[0] >= 0);
        CHECK(!e.serr[0].empty());
        CHECK(!std::isnan(e.cols[0]));
        CHECK(std::isnan(e.cols[3 * 6]));
    }
}

TEST_CASE("server_flags") {
    Fixture f;
    Session& s = *f.session;
    Engine check = synth::open_synthetic(f.tf);
    f.welcome();
    const double jd = 2451600.5;

    const auto one = [&](uint64_t iflag, uint32_t id = 905) {
        eph::Request req = base_request(jd, 1);
        req.objs = {obj(id)};
        req.iflag = iflag;
        req.topoLon = 10.0;
        req.topoLat = 50.0;
        req.topoElv = 300.0;
        req.sidMode = 7;
        static uint32_t rid = 100;
        CHECK(s.on_message(request(req, ++rid), true));
        return join(drain(s), 1, 1);
    };
    const auto engine = [&](CalcOptions o, bool tt = false) {
        o.sigma = false;
        auto r = tt ? check.calc(5, jd, o) : check.calc_ut(5, jd, o);
        REQUIRE(r.ok());
        return r.value().pos;
    };

    SUBCASE("equatorial, J2000") {
        CalcOptions o;
        o.coords = Coords::Equatorial;
        o.frame = Frame::J2000;
        const Position p = engine(o);
        const Data d = one((1u << 5) | (1u << 6));
        CHECK(d.cols[0] == p.lon_deg);
        CHECK(d.cols[1] == p.lat_deg);
    }
    SUBCASE("rectangular, heliocentric") {
        CalcOptions o;
        o.center = Center::Heliocentric;
        const Position p = engine(o);
        const Data d = one((1u << 9) | (1u << 11));
        for (int i = 0; i < 3; ++i) {
            CHECK(d.cols[i] == p.xyz_au[i]);
            CHECK(d.cols[3 + i] == p.vel_au_day[i]);
        }
    }
    SUBCASE("radians") {
        const Position p = engine(CalcOptions{});
        const Data d = one(1u << 10);
        CHECK(d.cols[0] == p.lon_deg * (3.14159265358979323846 / 180.0));
        CHECK(d.cols[2] == p.dist_au);
    }
    SUBCASE("topocentric uses the request's site") {
        CalcOptions o;
        o.center = Center::Topocentric;
        o.site.lon_rad = 10.0 * (3.14159265358979323846 / 180.0);
        o.site.lat_rad = 50.0 * (3.14159265358979323846 / 180.0);
        o.site.height_m = 300.0;
        CHECK(one(1u << 12).cols[0] == engine(o).lon_deg);
    }
    SUBCASE("sidereal through the map's zodiac") {
        CalcOptions o;
        o.sidereal = SiderealMode::Lahiri;
        CHECK(one(1u << 14).cols[0] == engine(o).lon_deg);
    }
    SUBCASE("an unmapped bit fails the objects") {
        const Data d = one(1u << 3);
        CHECK(std::isnan(d.cols[0]));
        CHECK(d.ret[0] == -1);
        CHECK(d.serr[0] == "iflag bit 3 has no wire-map entry");
    }
    SUBCASE("TT rows") {
        CHECK(one(eph::kIflagTimeTT).cols[0] == engine(CalcOptions{}, true).lon_deg);
    }
    SUBCASE("unsupported combinations fail the objects") {
        CHECK(one((1u << 11) | (1u << 13)).serr[0] ==
              "more than one observer (heliocentric, barycentric, topocentric)");
        CHECK(one(eph::kIflagCenter).serr[0] == "planet-centred positions are not supported");
    }
}

TEST_CASE("server_limits") {
    Fixture f;
    Session& s = *f.session;
    f.welcome();

    eph::Request big = base_request(2451545.0, 20000);
    big.objs = {obj(900), obj(905), obj(900), obj(905), obj(900), obj(905)};
    CHECK(s.on_message(request(big, 1), true));
    auto r = drain(s);
    REQUIRE(r.size() == 1);
    CHECK(error_of(r[0]).code == eph::kErrLimits);
    CHECK(error_of(r[0]).text == "REQUEST asks 120000 cells; WELCOME's bound is 100000");

    eph::Request over = base_request(2451545.0, 20001);
    over.objs = {obj(900)};
    CHECK(s.on_message(request(over, 2), true));
    CHECK(error_of(drain(s)[0]).code == eph::kErrLimits);

    // Answers not yet taken: the fifth is refused before it is computed.
    for (uint32_t i = 0; i < 5; ++i) {
        eph::Request req = base_request(2451545.0 + i, 1);
        req.objs = {obj(900)};
        CHECK(s.on_message(request(req, 10 + i), true));
    }
    CHECK(s.queued_answers() == 4);
    r = drain(s);
    REQUIRE(r.size() == 5);
    CHECK(error_of(r[0]).requestId == 14); // control replies go first
    for (int i = 1; i < 5; ++i) {
        CHECK(r[i].env.type == eph::kMsgData);
        CHECK(r[i].env.requestId == uint32_t(9 + i));
    }

    auto payload = std::vector<uint8_t>(3, 0);
    CHECK(s.on_message(as_view(eph::makeMessage(eph::kMsgRequest, 30, payload.data(), 3)), true));
    CHECK(error_of(drain(s)[0]).code == eph::kErrBad);
}

TEST_CASE("server_cache_budget") {
    ResultCache cache(1000);
    auto answer = [](size_t n) {
        auto a = std::make_shared<Answer>();
        a->cols.assign(n, 0.0);
        return a;
    };
    cache.put("a", answer(50)); // 400 + 2
    cache.put("b", answer(50));
    CHECK(cache.entries() == 2);
    CHECK(cache.get("a") != nullptr); // a is now the most recent
    cache.put("c", answer(50));       // evicts b
    CHECK(cache.get("b") == nullptr);
    CHECK(cache.get("a") != nullptr);
    CHECK(cache.get("c") != nullptr);
    CHECK(cache.used_bytes() == 804);
    cache.put("huge", answer(200)); // larger than the budget: not kept
    CHECK(cache.entries() == 2);
    CHECK(cache.hits() == 3);
    CHECK(cache.misses() == 1);
}

TEST_CASE("server_ephproto_matches_astrolog") {
    // Astrolog owns the protocol; third_party/ephproto/ephproto.h is a pinned
    // copy. With an Astrolog checkout named by $PROMETHEIA_ASTROLOG, the copy
    // must equal its ephsrv/ephproto.h byte for byte. SKIPs otherwise.
    const char* astrolog = std::getenv("PROMETHEIA_ASTROLOG");
    if (!astrolog || !*astrolog) {
        std::printf("  SKIP: PROMETHEIA_ASTROLOG not set\n");
        return;
    }
    const auto slurp = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        REQUIRE(in.good());
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const std::string theirs = slurp(std::string(astrolog) + "/ephsrv/ephproto.h");
    const std::string ours =
        slurp(std::string(PROMETHEIA_SOURCE_DIR) + "/third_party/ephproto/ephproto.h");
    CHECK_MESSAGE(ours == theirs,
                  "Astrolog's ephproto.h changed: review it and re-pin (third_party/README.md)");
}

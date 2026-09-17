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

#include "objects.hpp"
#include "prometheia/stars.hpp"
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
    node.method = eph::kNodOscu;
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
    // The star object ("Sirius"): the engine's catalog place.
    CHECK(d.ret[3] == int32_t(req.iflag));
    CHECK(d.name[3] == "Sirius");
    for (uint32_t r = 0; r < n_time; ++r) {
        const auto res = check.calc_star_ut(stars::find("Sirius").value(), jd + r * 0.25, opts);
        REQUIRE(res.ok());
        CHECK(d.cols[(3 * n_time + r) * 6] == res.value().pos.lon_deg);
    }
    // The node object (osculating ascending node of wire body 905).
    CHECK(d.ret[4] == int32_t(req.iflag));
    CHECK(d.name[4] == "SPK-ID 5 asc. node");
    for (uint32_t r = 0; r < n_time; ++r) {
        const auto res = check.calc_orbit_point_ut(5, OrbitPoint::AscendingNode,
                                                   OrbitElements::Osculating, jd + r * 0.25, opts);
        REQUIRE(res.ok());
        CHECK(d.cols[(4 * n_time + r) * 6] == res.value().pos.lon_deg);
        CHECK(d.cols[(4 * n_time + r) * 6 + 3] == res.value().pos.lon_speed);
    }
    for (uint32_t o = 2; o < 3; ++o) {
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
    SUBCASE("planet-centred through the request's center") {
        CalcOptions o;
        o.center = Center::Body;
        o.center_body = 10;
        eph::Request req = base_request(jd, 1);
        req.objs = {obj(905)};
        req.center = 900; // the Sun, by its wire id
        CHECK(s.on_message(request(req, 300), true));
        const Data d = join(drain(s), 1, 1);
        CHECK(d.ret[0] >= 0);
        CHECK(d.cols[0] == engine(o).lon_deg);
        // Center 0 with the flag bit is wire body 0, which this map lacks;
        // a center together with an observer flag is a conflict.
        req.center = 900;
        req.iflag = 1u << 11;
        CHECK(s.on_message(request(req, 301), true));
        CHECK(join(drain(s), 1, 1).serr[0] == "more than one observer (helio, bary, topo, center)");
    }
    SUBCASE("TT rows") {
        CHECK(one(eph::kIflagTimeTT).cols[0] == engine(CalcOptions{}, true).lon_deg);
    }
    SUBCASE("unsupported combinations fail the objects") {
        CHECK(one((1u << 11) | (1u << 13)).serr[0] ==
              "more than one observer (helio, bary, topo, center)");
        CHECK(one(eph::kIflagCenter).serr[0] == "center 0 has no wire-map entry");
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
    // Astrolog owns the protocol; third_party/ephproto/v4/ephproto.h is the
    // pinned copy of the locked version 4 (the version 3 header stays beside
    // it only until the migration's delete step). With an Astrolog checkout
    // named by $PROMETHEIA_ASTROLOG, the copy must equal its
    // ephsrv/ephproto.h byte for byte. SKIPs otherwise.
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
        slurp(std::string(PROMETHEIA_SOURCE_DIR) + "/third_party/ephproto/v4/ephproto.h");
    CHECK_MESSAGE(ours == theirs,
                  "Astrolog's ephproto.h changed: review it and re-pin (third_party/README.md)");
}

TEST_CASE("server_limits_caps_and_budget") {
    LimitsConfig lc;
    lc.max_conns = 3;
    lc.max_conns_per_addr = 2;
    lc.cells_per_sec = 100;
    lc.burst_cells = 1000;
    Limits limits(lc, {"secret"});

    CHECK(limits.admit("a") == nullptr);
    CHECK(limits.admit("a") == nullptr);
    CHECK(std::string(limits.admit("a")) == "too many connections from this address");
    CHECK(limits.admit("b") == nullptr);
    CHECK(std::string(limits.admit("c")) == "the server is at its connection limit");
    CHECK(limits.connections() == 3);
    limits.release("a");
    CHECK(limits.admit("c") == nullptr);

    CHECK(limits.token_known("secret"));
    CHECK(!limits.token_known(""));
    CHECK(!limits.token_known("guess"));

    const auto t0 = Limits::Clock::now();
    CHECK(limits.charge("k", 1000, t0) == 0.0); // a full bucket covers one full request
    CHECK(limits.charge("k", 50, t0) == doctest::Approx(0.5));
    CHECK(limits.charge("k", 50, t0 + std::chrono::milliseconds(500)) == 0.0);
    CHECK(limits.charge("other", 1000, t0) == 0.0); // budgets are separate

    LimitsConfig none;
    none.cells_per_sec = 0;
    CHECK(Limits(none).charge("k", 1u << 30) == 0.0);

    TempFile tokens("tokens");
    {
        std::ofstream out(tokens.path);
        out << "# accepted\n\n  alpha  \nbeta\r\n";
    }
    auto loaded = Limits::load_tokens(tokens.path.string());
    REQUIRE(loaded.ok());
    CHECK(loaded.value().size() == 2);
    CHECK(loaded.value().count("alpha") == 1);
    CHECK(loaded.value().count("beta") == 1);
    {
        std::ofstream out(tokens.path);
        out << std::string(129, 'x') << "\n";
    }
    CHECK(!Limits::load_tokens(tokens.path.string()).ok());
    CHECK(!Limits::load_tokens("/nonexistent/tokens").ok());
}

TEST_CASE("server_tokens_and_rate_limit") {
    TempFile tf("server-limits");
    WireMap map = WireMap::parse(kTestMap).value();
    LimitsConfig lc;
    lc.require_token = true;
    lc.cells_per_sec = 10;
    lc.burst_cells = 20;
    Limits limits(lc, {"secret"});
    ServerConfig config;
    config.max_cells = 20;
    LoopContext ctx(synth::open_synthetic(tf), map, config, &limits);

    const auto hello_with = [](const char* token) {
        uint8_t buf[eph::kHelloMaxSize];
        uint32_t len = 0;
        eph::buildHello(buf, 0, 1, "test", &len, token);
        return as_view(eph::makeMessage(eph::kMsgHello, 1, buf, len));
    };

    SUBCASE("no token, unknown token: ERROR 7 and close") {
        Session a(ctx, "10.0.0.1");
        CHECK(!a.on_message(hello_with(nullptr), true));
        CHECK(error_of(drain(a)[0]).text == "this server requires a token");
        Session b(ctx, "10.0.0.1");
        CHECK(!b.on_message(hello_with("guess"), true));
        auto e = error_of(drain(b)[0]);
        CHECK(e.code == eph::kErrToken);
        CHECK(e.text == "unknown token");
        CHECK(ctx.metrics().errors[eph::kErrToken] == 2);
    }
    SUBCASE("a known token has its own budget; over it, ERROR 6") {
        Session s(ctx, "10.0.0.1");
        CHECK(s.on_message(hello_with("secret"), true));
        CHECK(drain(s)[0].env.type == eph::kMsgWelcome);
        eph::Request req = base_request(2451545.0, 20);
        req.objs = {obj(900)};
        CHECK(s.on_message(request(req, 1), true));
        CHECK(drain(s).back().env.type == eph::kMsgData);
        req.jdStart += 1.0;
        CHECK(s.on_message(request(req, 2), true));
        const auto r = drain(s);
        REQUIRE(r.size() == 1);
        const auto e = error_of(r[0]);
        CHECK(e.code == eph::kErrRateLimited);
        CHECK(e.text.rfind("rate limited: 10 cells a second; ask again in ", 0) == 0);
        CHECK(ctx.metrics().hellos == 1);
        CHECK(ctx.metrics().requests == 1);
        CHECK(ctx.metrics().cache_misses == 1);
        CHECK(ctx.metrics().cells_computed == 20);
    }
}

TEST_CASE("server_metrics_text") {
    Metrics a, b;
    ++a.hellos;
    b.hellos += 2;
    a.error(eph::kErrLimits);
    b.error(99);
    a.computed(40, 3.0);
    ServerInfo info;
    info.server_version = kServerVersion;
    info.protocol = 3;
    const std::string text = metrics_text({&a, &b}, info);
    CHECK(text.find("prometheiad_hellos_total 3\n") != std::string::npos);
    CHECK(text.find("prometheiad_errors_total{code=\"2\"} 1\n") != std::string::npos);
    CHECK(text.find("prometheiad_errors_total{code=\"0\"} 1\n") != std::string::npos);
    CHECK(text.find("prometheiad_compute_seconds_bucket{le=\"0.001\"} 0\n") != std::string::npos);
    CHECK(text.find("prometheiad_compute_seconds_bucket{le=\"0.005\"} 1\n") != std::string::npos);
    CHECK(text.find("prometheiad_compute_seconds_count 1\n") != std::string::npos);
    CHECK(text.find("prometheiad_cells_computed_total 40\n") != std::string::npos);
    CHECK(text.find(
              "prometheiad_build_info{server=\"prometheiad/0.1.0\",protocol=\"3\",tls=\"0\"} 1") !=
          std::string::npos);
}

TEST_CASE("server_objects_resolve_once_and_sample_the_same_body") {
    TempFile tf{"objects"};
    Engine engine = synth::open_synthetic(tf);
    const WireMap map = WireMap::parse(kTestMap).value();

    // The wire numbering is resolved away, and what comes back carries the
    // name the answer's metadata reports.
    auto sun = resolve_object(obj(900), map);
    REQUIRE(sun.ok());
    CHECK(sun.value().kind == ResolvedObject::Kind::Body);
    CHECK(sun.value().naif_id == 10);
    CHECK(sun.value().name == "Test Sun");

    eph::ObjSpec star_spec;
    star_spec.kind = eph::kObjStar;
    std::strcpy(star_spec.name, "Sirius");
    auto star = resolve_object(star_spec, map);
    REQUIRE(star.ok());
    CHECK(star.value().kind == ResolvedObject::Kind::Star);
    CHECK(star.value().name == "Sirius");

    eph::ObjSpec node_spec = obj(905);
    node_spec.kind = eph::kObjNodAps;
    node_spec.point = eph::kPntNorthNode;
    node_spec.method = eph::kNodOscu;
    auto node = resolve_object(node_spec, map);
    REQUIRE(node.ok());
    CHECK(node.value().kind == ResolvedObject::Kind::OrbitPoint);
    CHECK(node.value().naif_id == 5);
    CHECK(node.value().name == "SPK-ID 5 asc. node");

    // A refusal names no instant, because it is the same for every row.
    auto missing = resolve_object(obj(123), map);
    REQUIRE(!missing.ok());
    CHECK(missing.error().message == "body 123 has no wire-map entry");

    // The sampler is not a second path to the body: it is the row path, in
    // rectangular form, so the two agree to the bit.
    const ResolvedObject jupiter = resolve_object(obj(905), map).value();
    CalcOptions opts;
    const segments::Sampler sampler = sampler_for(engine, jupiter, opts);
    for (int i = 0; i < 5; ++i) {
        const double jd = synth::kJ2000 + i * 3.0;
        double p[3] = {0, 0, 0}, v[3] = {0, 0, 0};
        REQUIRE(sampler(jd, p, v).ok());
        auto row = calc_at(engine, jupiter, jd, true, opts);
        REQUIRE(row.ok());
        for (int k = 0; k < 3; ++k) {
            CHECK(p[k] == row.value().pos.xyz_au[k]);
            CHECK(v[k] == row.value().pos.vel_au_day[k]);
        }
    }

    // And it is what the fitter wants: a lattice cell's worth of it holds to
    // the residual the fit declares.
    segments::FitOptions fit_options;
    fit_options.target_err_arcsec = 0.01;
    auto report = segments::fit(sampler, synth::kJ2000, synth::kJ2000 + 32.0, fit_options);
    REQUIRE_MESSAGE(report.ok(), report.error().message);
    CHECK(report.value().met_target);
    double worst = 0.0;
    for (int i = 0; i <= 64; ++i) {
        const double jd = synth::kJ2000 + i * 0.5;
        const segments::Segment* seg = nullptr;
        for (const segments::Segment& s : report.value().segments) {
            if (jd <= s.mid_jd_tt + s.half_span_days + 1e-9) {
                seg = &s;
                break;
            }
        }
        REQUIRE(seg != nullptr);
        double got[3];
        seg->position(jd, got);
        auto want = calc_at(engine, jupiter, jd, true, opts);
        REQUIRE(want.ok());
        const double* w = want.value().pos.xyz_au;
        const double dot = got[0] * w[0] + got[1] * w[1] + got[2] * w[2];
        const double cx[3] = {got[1] * w[2] - got[2] * w[1], got[2] * w[0] - got[0] * w[2],
                              got[0] * w[1] - got[1] * w[0]};
        const double cross = std::sqrt(cx[0] * cx[0] + cx[1] * cx[1] + cx[2] * cx[2]);
        worst = std::max(worst, std::atan2(cross, dot) * 180.0 / 3.14159265358979323846 * 3600.0);
    }
    CHECK(worst <= fit_options.target_err_arcsec);
    // The declared residual bounds an outsider's measurement, to the same
    // slack test_segments allows: the check set is finite, and an instant
    // between two of its points can sit a hair above what they saw.
    CHECK(worst <= report.value().worst_err_arcsec * 1.05 + 1e-9);
}

// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheiad's protocol core: the Session state machine (protocol version
// 4), driven without sockets, against the synthetic linear kernel and the
// committed sample catalog. The conformance fixtures pin the codec itself
// (tests/test_ephproto4.cpp); this file pins the server: the handshake, the
// profiles, the answers' metadata and values, and the limits.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "dataset.hpp"
#include "objects.hpp"
#include "prometheia/hypotheticals.hpp"
#include "prometheia/stars.hpp"
#include "session.hpp"
#include "synthetic_spk.hpp"

#include <doctest/doctest.h>

using namespace prometheia::server;
using synth::TempFile;

namespace {

struct Reply {
    eph::Envelope env{};
    std::vector<uint8_t> payload;
};

// Drives the session the way the WebSocket head does: send what is ready,
// then work the computes a slice at a time until nothing is left.
std::vector<Reply> drain(Session& s) {
    std::vector<Reply> out;
    std::vector<uint8_t> msg;
    for (;;) {
        while (s.next(msg)) {
            Reply r;
            REQUIRE(msg.size() >= eph::kEnvelopeSize);
            r.env.version = msg[2];
            r.env.type = uint16_t(msg[4] | (uint16_t(msg[5]) << 8));
            r.env.requestId = uint32_t(msg[8]) | (uint32_t(msg[9]) << 8) |
                              (uint32_t(msg[10]) << 16) | (uint32_t(msg[11]) << 24);
            r.env.payloadLen = uint32_t(msg[12]) | (uint32_t(msg[13]) << 8) |
                               (uint32_t(msg[14]) << 16) | (uint32_t(msg[15]) << 24);
            r.payload.assign(msg.begin() + eph::kEnvelopeSize, msg.end());
            CHECK(r.payload.size() == r.env.payloadLen);
            out.push_back(std::move(r));
        }
        if (!s.has_work()) {
            return out;
        }
        s.work();
    }
}

std::string as_view(const std::vector<uint8_t>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
}

std::vector<uint8_t> message(uint16_t type, uint32_t request_id, const uint8_t* payload, size_t len,
                             uint8_t version = eph::kProtoVersion) {
    std::vector<uint8_t> out;
    eph::WriteEnvelope(&out, type, request_id, len, version);
    out.insert(out.end(), payload, payload + len);
    return out;
}

std::string hello(uint32_t proto_max = eph::kProtoVersion, uint32_t proto_min = eph::kProtoMin,
                  const char* token = nullptr, uint8_t envelope_version = eph::kProtoVersion) {
    eph::Hello h;
    h.protoMax = proto_max;
    h.protoMin = proto_min;
    h.clientName = "test";
    if (token) {
        h.token = token;
    }
    std::vector<uint8_t> payload;
    eph::EncodeHello(&payload, h);
    return as_view(message(eph::kMsgHello, 0, payload.data(), payload.size(), envelope_version));
}

struct Fixture {
    TempFile tf{"server"};
    ServerConfig config;
    std::unique_ptr<LoopContext> ctx;
    std::unique_ptr<Session> session;

    // `element_file`: an optional element file of named hypothetical bodies,
    // added before the config is taken, as prometheiad does.
    explicit Fixture(const std::string& element_file = {}, const Log* log = nullptr) {
        Engine engine = synth::open_synthetic(tf);
        auto catalog =
            engine.add_catalog(std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/sample-100.epm");
        REQUIRE_MESSAGE(catalog.ok(), catalog.error().message);
        if (!element_file.empty()) {
            auto added = engine.add_hypotheticals(element_file);
            REQUIRE_MESSAGE(added.ok(), added.error().message);
        }
        config.hypotheticals = engine.hypothetical_tokens();
        config.engine = "Prometheia 0.1.0, synthetic kernel";
        config.dataset_id = "synthetic/test#00000000";
        config.ephemeris_name = "synthetic.bsp";
        config.catalog_names = {"sample-100.epm"};
        ctx = std::make_unique<LoopContext>(std::move(engine), config, nullptr, nullptr, log);
        session = std::make_unique<Session>(*ctx, "10.0.0.9", "0.7");
    }
    eph::Welcome welcome() {
        CHECK(session->on_message(hello(), true));
        const auto r = drain(*session);
        REQUIRE(r.size() == 1);
        REQUIRE(r[0].env.type == eph::kMsgWelcome);
        eph::Welcome w;
        std::string why;
        REQUIRE_MESSAGE(
            eph::ParseWelcome(r[0].payload.data(), r[0].payload.size(), &w, &why) == eph::kOk, why);
        return w;
    }
};

// A request with one profile (returned for tweaking) and body objects.
eph::Request base_request(double jd, uint32_t n_time, double step_days = 0.25) {
    eph::Request req;
    req.start.jd1 = jd;
    req.nTime = n_time;
    // 3.5: stepNs is 0 with one row, nonzero otherwise.
    req.stepNs = n_time > 1 ? int64_t(step_days * 86400.0 * 1e9) : 0;
    req.profiles.emplace_back(); // one default profile: geocentric apparent
    return req;
}

eph::Object body_obj(int naif) {
    eph::Object o;
    o.kind = eph::kObjBody;
    o.naif = naif;
    return o;
}

std::string request(const eph::Request& req, uint32_t id) {
    std::vector<uint8_t> payload;
    eph::EncodeRequest(&payload, req);
    return as_view(message(eph::kMsgRequest, id, payload.data(), payload.size()));
}

eph::Error error_of(const Reply& r) {
    eph::Error e;
    std::string why;
    REQUIRE(r.env.type == eph::kMsgError);
    REQUIRE_MESSAGE(eph::ParseError(r.payload.data(), r.payload.size(), &e, &why) == eph::kOk, why);
    return e;
}

// A decoded DATA answer: all chunks joined back into object-major columns.
struct Data {
    std::vector<eph::Meta> meta;
    std::vector<double> cols;
    uint32_t chunks = 0, columns_present = 0;
    int n_cols = 6;

    const eph::Meta* find(const char* name) const {
        for (const eph::Meta& m : meta) {
            if (m.name == name) {
                return &m;
            }
        }
        return nullptr;
    }
};

Data join(const std::vector<Reply>& replies) {
    Data d;
    uint32_t total = 0;
    for (const Reply& r : replies) {
        if (r.env.type != eph::kMsgData) {
            continue;
        }
        eph::DataChunk c;
        std::string why;
        REQUIRE_MESSAGE(eph::ParseData(r.payload.data(), r.payload.size(), &c, &why) == eph::kOk,
                        why);
        REQUIRE(c.chunkIndex == d.chunks);
        REQUIRE(c.iTime == total);
        if (c.flags & eph::kChunkMeta) {
            REQUIRE(d.meta.empty());
            d.meta = c.meta;
            d.columns_present = c.columnsPresent;
            d.n_cols = c.Cols();
            if (d.cols.empty()) {
                d.cols.assign(size_t(c.nObj) * c.totalRows * d.n_cols,
                              std::numeric_limits<double>::quiet_NaN());
            }
        }
        for (uint32_t o = 0; o < c.nObj; ++o) {
            for (uint32_t r2 = 0; r2 < c.nRows; ++r2) {
                for (int k = 0; k < d.n_cols; ++k) {
                    d.cols[(size_t(o) * c.totalRows + c.iTime + r2) * d.n_cols + k] =
                        c.values[(size_t(o) * c.nRows + r2) * d.n_cols + k];
                }
            }
        }
        total += c.nRows;
        ++d.chunks;
        if (d.chunks > 1) {
            REQUIRE(total <= d.cols.size() / d.n_cols);
        }
    }
    return d;
}

} // namespace

TEST_CASE("server_handshake") {
    Fixture f;
    Session& s = *f.session;

    SUBCASE("REQUEST before HELLO closes") {
        eph::Request req = base_request(2451545.0, 1);
        req.objs = {body_obj(10)};
        CHECK(!s.on_message(request(req, 5), true));
        const auto r = drain(s);
        REQUIRE(r.size() == 1);
        CHECK(error_of(r[0]).code == eph::kErrMalformed);
    }
    SUBCASE("WELCOME carries the session, the bounds and the capabilities") {
        const eph::Welcome w = f.welcome();
        CHECK(w.protoSession == 4);
        CHECK(w.caps == (eph::kCapF32 | eph::kCapInstantLists | eph::kCapLookup | eph::kCapDeepSky |
                         eph::kCapCancel | eph::kCapPriority | eph::kCapSegments));
        CHECK(w.serverName == kServerVersion);
        CHECK(w.engine == f.config.engine);
        CHECK(w.datasetId == f.config.dataset_id);
        CHECK(w.maxObjs == f.config.max_objs);
        CHECK(w.maxCells == f.config.max_cells);
        CHECK(s.version() == 4);
        eph::Capabilities c;
        std::string why;
        REQUIRE_MESSAGE(eph::ParseCapabilities(w.caps_, &c, &why) == eph::kOk, why);
        CHECK(c.Kind(eph::kObjBody));
        CHECK(c.Kind(eph::kObjOrbitPoint));
        CHECK(c.Kind(eph::kObjStar));
        CHECK(c.Kind(eph::kObjElements));
        // Kind 3 exactly when there are named bodies to serve.
        CHECK(c.Kind(eph::kObjHypothetical) == !c.hypotheticals.empty());
        CHECK(c.Observer(eph::kObsGeo));
        CHECK(c.Observer(eph::kObsBary));
        CHECK(c.Zodiac("fagan-bradley"));
        CHECK(c.Zodiac("user"));
        CHECK(!c.Zodiac("raman"));
        CHECK(c.TimeScale(eph::kTimeUT1));
        CHECK(c.TimeScale(eph::kTimeTDB));
        CHECK(c.deltaTModel == kDeltaTModelName);
        // The Sun's centre cannot be deflected; every other observer can
        // (3.5a), including the barycentre.
        CHECK(c.CorrectionMask(eph::kObsGeo, eph::kCorrMask));
        CHECK(c.CorrectionMask(eph::kObsTopo, eph::kCorrMask));
        CHECK(c.CorrectionMask(eph::kObsBary, eph::kCorrMask));
        CHECK(!c.CorrectionMask(eph::kObsHelio, eph::kCorrDeflection));
        CHECK(c.CorrectionMask(eph::kObsHelio, eph::kCorrLightTime | eph::kCorrAberration));
        // A.3 0x0004 lists EXACT masks, and every one honoured must be listed:
        // a client never sends an unlisted pair, so an unlisted mask 0 would
        // mean no geometric answers from this server at all.
        for (uint8_t m = 0; m <= eph::kCorrMask; ++m) {
            CHECK(c.CorrectionMask(eph::kObsGeo, m));
            CHECK(c.CorrectionMask(eph::kObsBary, m));
            CHECK(c.CorrectionMask(eph::kObsHelio, m) == ((m & eph::kCorrDeflection) == 0));
        }
        CHECK(c.lookupMax > 0);
    }
    SUBCASE("a newer client meets the server at 4") {
        CHECK(s.on_message(hello(9), true));
        const auto r = drain(s);
        REQUIRE(r.size() == 1);
        CHECK(r[0].env.version == 4);
        eph::Welcome w;
        std::string why;
        REQUIRE(eph::ParseWelcome(r[0].payload.data(), r[0].payload.size(), &w, &why) == eph::kOk);
        CHECK(w.protoSession == 4);
    }
    SUBCASE("a repeated HELLO keeps the first session") {
        f.welcome();
        CHECK(s.on_message(hello(9), true));
        CHECK(drain(s)[0].env.version == 4);
    }
    SUBCASE("an older envelope is refused in its own layout") {
        // A version-3 HELLO: the ERROR comes back as the legacy layout, in
        // version 3 (3.3.5).
        const auto msg = hello(3, 3, nullptr, 3);
        CHECK(!s.on_message(msg, true));
        const auto r = drain(s);
        REQUIRE(r.size() == 1);
        CHECK(r[0].env.version == 3);
        CHECK(r[0].env.type == eph::kMsgError);
        eph::LegacyError le;
        std::string why;
        REQUIRE(eph::ParseLegacyError(r[0].payload.data(), r[0].payload.size(), &le, &why) ==
                eph::kOk);
        CHECK(le.code == eph::kErrVersion);
    }
    SUBCASE("malformed frames") {
        CHECK(!s.on_message("text", false));
        CHECK(!s.on_message("short", true));
        auto msg = message(eph::kMsgPing, 0, nullptr, 0);
        msg[0] ^= 0xFF;
        CHECK(!s.on_message(as_view(msg), true));
        msg = message(eph::kMsgPing, 0, nullptr, 0);
        msg[12] = 4; // payloadLen does not match the frame
        CHECK(!s.on_message(as_view(msg), true));
        msg = message(eph::kMsgPing, 0, nullptr, 0);
        msg[6] = 1; // reserved nonzero
        CHECK(!s.on_message(as_view(msg), true));
        const auto r = drain(s);
        REQUIRE(r.size() == 5);
        for (const Reply& e : r) {
            CHECK(error_of(e).code == eph::kErrMalformed);
        }
    }
    SUBCASE("ping, pong, unknown type, wrong direction") {
        f.welcome();
        CHECK(s.on_message(as_view(message(eph::kMsgPing, 0, nullptr, 0)), true));
        CHECK(s.on_message(as_view(message(eph::kMsgPong, 0, nullptr, 0)), true));
        CHECK(s.on_message(as_view(message(99, 44, nullptr, 0)), true));
        CHECK(s.on_message(as_view(message(eph::kMsgWelcome, 0, nullptr, 0)), true));
        const auto r = drain(s);
        REQUIRE(r.size() == 3);
        CHECK(r[0].env.type == eph::kMsgPong);
        CHECK(error_of(r[1]).code == eph::kErrUnknownType);
        CHECK(error_of(r[2]).code == eph::kErrMalformed);
    }
}

TEST_CASE("server_request_answers") {
    Fixture f;
    Session& s = *f.session;
    Engine check = synth::open_synthetic(f.tf);
    REQUIRE(
        check.add_catalog(std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/sample-100.epm").ok());
    f.welcome();

    const double jd = 2451545.25;
    const uint32_t n_time = 7;
    eph::Request req = base_request(jd, n_time);
    eph::Object star;
    star.kind = eph::kObjStar;
    star.name = "Sirius";
    eph::Object node = body_obj(5);
    node.kind = eph::kObjOrbitPoint;
    node.point = eph::kPtAscNode;
    node.method = eph::kMethOsculating;
    eph::Object ceres;
    ceres.kind = eph::kObjDesignation;
    ceres.name = "Ceres";
    req.objs = {body_obj(10), body_obj(5), body_obj(1), star, node, ceres};
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
    const Data d = join(replies);
    CHECK(d.chunks == 3);
    REQUIRE(d.meta.size() == 6);

    // Mapped bodies: the engine's answer, bit for bit, at each row's TT.
    const auto opts = [] {
        CalcOptions o;
        o.sigma = false;
        return o;
    }();
    for (uint32_t o = 0; o < 2; ++o) {
        CHECK(d.meta[o].rowsOk == int32_t(n_time));
        CHECK(d.meta[o].errCode == eph::kOErrNone);
        for (uint32_t r = 0; r < n_time; ++r) {
            const auto res = check.calc(o == 0 ? 10 : 5, jd + r * 0.25, opts);
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
    CHECK(d.meta[0].name == "Sun");
    CHECK(d.meta[1].name == "Jupiter");
    CHECK(d.meta[0].resolvedNaif == 10);

    // A body the kernel does not carry fails alone (3.5).
    CHECK(d.meta[2].rowsOk == 0);
    CHECK(d.meta[2].errCode == eph::kOErrUnknownBody);
    CHECK(!d.meta[2].errText.empty());
    for (uint32_t r = 0; r < n_time; ++r) {
        for (int k = 0; k < 6; ++k) {
            CHECK(std::isnan(d.cols[(size_t(2) * n_time + r) * 6 + k]));
        }
    }

    // The star object: the engine's catalog place, with no light time
    // structurally (corrApplied 2|4).
    CHECK(d.meta[3].rowsOk == int32_t(n_time));
    CHECK(d.meta[3].name == "Sirius");
    CHECK(d.meta[3].corrApplied == (eph::kCorrDeflection | eph::kCorrAberration));
    CHECK(d.meta[3].resolvedNaif == eph::kNaifNone);
    for (uint32_t r = 0; r < n_time; ++r) {
        const auto res = check.calc_star(stars::find("Sirius").value(), jd + r * 0.25, opts);
        REQUIRE(res.ok());
        CHECK(d.cols[(3 * n_time + r) * 6] == res.value().pos.lon_deg);
    }

    // The osculating ascending node of Jupiter.
    CHECK(d.meta[4].rowsOk == int32_t(n_time));
    CHECK(d.meta[4].name == "Jupiter asc. node");
    CHECK(d.meta[4].corrApplied == eph::kCorrMask);
    for (uint32_t r = 0; r < n_time; ++r) {
        const auto res = check.calc_orbit_point(5, OrbitPoint::AscendingNode,
                                                OrbitElements::Osculating, jd + r * 0.25, opts);
        REQUIRE(res.ok());
        CHECK(d.cols[(4 * n_time + r) * 6] == res.value().pos.lon_deg);
        CHECK(d.cols[(4 * n_time + r) * 6 + 3] == res.value().pos.lon_speed);
    }

    // The designation resolves through the catalog, exactly as a LOOKUP.
    CHECK(d.meta[5].rowsOk == int32_t(n_time));
    CHECK(d.meta[5].name == "Ceres");
    CHECK(d.meta[5].resolvedNaif == 20000001);
    for (uint32_t r = 0; r < n_time; ++r) {
        const auto res = check.calc(20000001, jd + r * 0.25, opts);
        REQUIRE(res.ok());
        CHECK(d.cols[(5 * n_time + r) * 6] == res.value().pos.lon_deg);
    }

    SUBCASE("the same question again is a cache hit, whatever the delivery") {
        req.chunkRows = 500;
        req.precision = eph::kPrecF32;
        CHECK(s.on_message(request(req, 78), true));
        const auto again = drain(s);
        REQUIRE(again.size() == 1);
        CHECK(f.ctx->cache().hits() == 1);
        CHECK(f.ctx->cache().entries() == 1);
        const Data d32 = join(again);
        REQUIRE(d32.meta.size() == 6);
        CHECK(d32.meta[2].errCode == eph::kOErrUnknownBody); // meta survives the hit
        for (size_t k = 0; k < d.cols.size(); ++k) {
            if (std::isnan(d.cols[k])) {
                CHECK(std::isnan(d32.cols[k]));
            } else {
                CHECK(d32.cols[k] == double(float(d.cols[k])));
            }
        }
    }
    SUBCASE("a row the engine cannot answer is NaN; the object keeps the rest") {
        eph::Request edge = base_request(jd + 60.0 * 365.25 - 1.0, 4, 1.0);
        edge.objs = {body_obj(10)};
        CHECK(s.on_message(request(edge, 79), true));
        const Data e = join(drain(s));
        REQUIRE(e.meta.size() == 1);
        CHECK(e.meta[0].rowsOk > 0);
        CHECK((e.meta[0].flags & eph::kMetaPartial) != 0);
        CHECK(e.meta[0].firstFailedRow == uint32_t(e.meta[0].rowsOk));
        CHECK(e.meta[0].errCode == eph::kOErrCoverage);
        // 3.8: the reason names no instant.
        CHECK(e.meta[0].errText.find("JD") == std::string::npos);
        CHECK(!std::isnan(e.cols[0]));
        CHECK(std::isnan(e.cols[size_t(e.meta[0].firstFailedRow) * 6]));
    }
}

TEST_CASE("server_profiles") {
    Fixture f;
    Session& s = *f.session;
    Engine check = synth::open_synthetic(f.tf);
    f.welcome();
    const double jd = 2451600.5;

    const auto one = [&](const eph::Profile& pf, uint32_t columns = 0) {
        eph::Request req = base_request(jd, 1);
        req.profiles[0] = pf;
        req.profiles[0].columns = columns;
        req.objs = {body_obj(5)};
        static uint32_t rid = 100;
        CHECK(s.on_message(request(req, ++rid), true));
        return join(drain(s));
    };
    const auto engine = [&](CalcOptions o) {
        o.sigma = false;
        auto r = check.calc(5, jd, o);
        REQUIRE(r.ok());
        return r.value();
    };

    SUBCASE("equatorial, J2000") {
        CalcOptions o;
        o.coords = Coords::Equatorial;
        o.frame = Frame::J2000;
        const CalcResult want = engine(o);
        eph::Profile pf;
        pf.plane = eph::kPlaneEquator;
        pf.frame = eph::kFrameJ2000;
        const Data d = one(pf);
        CHECK(d.cols[0] == want.pos.lon_deg);
        CHECK(d.cols[1] == want.pos.lat_deg);
    }
    SUBCASE("rectangular, heliocentric") {
        CalcOptions o;
        o.center = Center::Heliocentric;
        const CalcResult want = engine(o);
        eph::Profile pf;
        pf.observer = eph::kObsHelio;
        pf.corrections = eph::kCorrLightTime | eph::kCorrAberration; // all the Sun's centre honours
        pf.form = eph::kFormRectangular;
        const Data d = one(pf);
        for (int i = 0; i < 3; ++i) {
            CHECK(d.cols[i] == want.pos.xyz_au[i]);
            CHECK(d.cols[3 + i] == want.pos.vel_au_day[i]);
        }
        // Heliocentric: no deflection, structurally.
        REQUIRE(d.meta.size() == 1);
        CHECK(d.meta[0].corrApplied == (eph::kCorrLightTime | eph::kCorrAberration));
    }
    SUBCASE("the Sun's own light is not deflected; barycentric light is") {
        eph::Profile pf;
        const Data sun = [&] {
            eph::Request req = base_request(jd, 1);
            req.objs = {body_obj(10)};
            CHECK(s.on_message(request(req, 201), true));
            return join(drain(s));
        }();
        REQUIRE(sun.meta.size() == 1);
        CHECK(sun.meta[0].corrApplied == (eph::kCorrLightTime | eph::kCorrAberration));
        pf.observer = eph::kObsBary;
        const Data bary = one(pf);
        REQUIRE(bary.meta.size() == 1);
        CHECK(bary.meta[0].corrApplied == eph::kCorrMask);
    }
    SUBCASE("speeds off: zero rate columns and the noSpeeds flag") {
        eph::Profile pf;
        pf.speeds = 0;
        const Data d = one(pf);
        REQUIRE(d.meta.size() == 1);
        CHECK((d.meta[0].flags & eph::kMetaNoSpeeds) != 0);
        for (int k = 3; k < 6; ++k) {
            CHECK(d.cols[k] == 0.0);
        }
    }
    SUBCASE("topocentric uses the profile's site") {
        CalcOptions o;
        o.center = Center::Topocentric;
        o.site.lon_rad = 10.0 * (std::acos(-1.0) / 180.0);
        o.site.lat_rad = 50.0 * (std::acos(-1.0) / 180.0);
        o.site.height_m = 300.0;
        const Position want = engine(o).pos;
        eph::Profile pf;
        pf.observer = eph::kObsTopo;
        pf.siteLonEastDeg = 10.0;
        pf.siteLatDeg = 50.0;
        pf.siteHeightM = 300.0;
        CHECK(one(pf).cols[0] == want.lon_deg);
    }
    SUBCASE("a zodiac shifts the longitude; the ayanamsa column reports it") {
        CalcOptions o;
        o.sidereal = SiderealMode::Lahiri;
        const CalcResult want = engine(o);
        eph::Profile pf;
        pf.zodiac = "lahiri";
        const Data d = one(pf, eph::kColAyanamsa);
        CHECK(d.columns_present == eph::kColAyanamsa);
        CHECK(d.n_cols == 7);
        CHECK(d.cols[0] == want.pos.lon_deg);
        CHECK(d.cols[6] == want.ayanamsa_deg.value());
        // Tropical asks nothing and reports zero.
        const Data trop = one(eph::Profile{}, eph::kColAyanamsa);
        CHECK(trop.cols[6] == 0.0);
    }
    SUBCASE("a user zodiac anchors at the profile's epoch") {
        CalcOptions o;
        o.sidereal = SiderealMode::User;
        o.sidereal_epoch_jtdb = 2435553.5;
        o.sidereal_ayanamsa_deg = 23.0;
        const Position want = engine(o).pos;
        eph::Profile pf;
        pf.zodiac = "user";
        pf.anchorEpoch.jd1 = 2435553.5;
        pf.anchorAyanamsaDeg = 23.0;
        CHECK(one(pf).cols[0] == want.lon_deg);
    }
    SUBCASE("a zodiac this engine does not serve is refused") {
        eph::Profile pf;
        pf.zodiac = "raman";
        eph::Request req = base_request(jd, 1);
        req.profiles[0] = pf;
        req.objs = {body_obj(5)};
        CHECK(s.on_message(request(req, 210), true));
        const auto r = drain(s);
        REQUIRE(r.size() == 1);
        CHECK(error_of(r[0]).code == eph::kErrUnsupported);
    }
    SUBCASE("the fixed sidereal planes are served, as rows and on the ecliptic") {
        // A.8 planes 1 and 2 answer what the engine answers; the ayanamsa
        // column reports the anchor's A0 there.
        for (const auto [plane, engine_plane] :
             {std::pair{eph::kSidPlaneAnchor, SiderealPlane::EclipticOfAnchor},
              std::pair{eph::kSidPlaneInvariable, SiderealPlane::Invariable}}) {
            CalcOptions o;
            o.sidereal = SiderealMode::Lahiri;
            o.sidereal_plane = engine_plane;
            const CalcResult want = engine(o);
            eph::Profile pf;
            pf.zodiac = "lahiri";
            pf.siderealPlane = plane;
            const Data d = one(pf, eph::kColAyanamsa);
            REQUIRE(d.cols.size() >= 7);
            CHECK(d.cols[0] == want.pos.lon_deg);
            CHECK(d.cols[1] == want.pos.lat_deg);
            CHECK(d.cols[6] == *want.ayanamsa_deg);
        }
        // A zodiac on the equator is malformed (the codec's rule, whatever
        // the sidereal plane).
        eph::Profile eq;
        eq.zodiac = "lahiri";
        eq.siderealPlane = eph::kSidPlaneInvariable;
        eq.plane = eph::kPlaneEquator;
        eph::Request req = base_request(jd, 1);
        req.profiles[0] = eq;
        req.objs = {body_obj(5)};
        CHECK(s.on_message(request(req, 212), true));
        CHECK(error_of(drain(s)[0]).code == eph::kErrMalformed);
        // Segments carry the ayanamsa as a longitude shift, which a fixed
        // plane is not: refused (ERROR 11), rows are the way to ask.
        eph::Request seg = base_request(jd, 4);
        seg.representation = 1;
        seg.segTargetErrArcsec = 0.01f;
        seg.profiles[0].zodiac = "lahiri";
        seg.profiles[0].siderealPlane = eph::kSidPlaneAnchor;
        seg.profiles[0].form = eph::kFormRectangular;
        seg.objs = {body_obj(5)};
        CHECK(s.on_message(request(seg, 213), true));
        CHECK(error_of(drain(s)[0]).code == eph::kErrUnsupported);
    }
    SUBCASE("an observer equal to the object is a per-object error") {
        eph::Profile pf;
        pf.observer = eph::kObsBody;
        pf.observerBody = 5;
        const Data d = one(pf);
        REQUIRE(d.meta.size() == 1);
        CHECK(d.meta[0].rowsOk == 0);
        CHECK(d.meta[0].errCode == eph::kOErrUnsupported);
    }
}

TEST_CASE("server_time_and_columns") {
    Fixture f;
    Session& s = *f.session;
    Engine check = synth::open_synthetic(f.tf);
    f.welcome();
    const double jd = 2451600.5;

    SUBCASE("UT1 rows with the server's delta T") {
        eph::Request req = base_request(jd, 2);
        req.timeScale = eph::kTimeUT1;
        req.objs = {body_obj(5)};
        CHECK(s.on_message(request(req, 300), true));
        const Data d = join(drain(s));
        CalcOptions o;
        o.sigma = false;
        const auto want = check.calc_ut(5, jd, o);
        REQUIRE(want.ok());
        CHECK(d.cols[0] == want.value().pos.lon_deg);
    }
    SUBCASE("UT1 rows with the client's one delta T value") {
        eph::Request req = base_request(jd, 2);
        req.timeScale = eph::kTimeUT1;
        req.deltaTSec = 70.0;
        req.objs = {body_obj(5)};
        CHECK(s.on_message(request(req, 301), true));
        const Data d = join(drain(s));
        struct Constant : time::DeltaTModel {
            double delta_t_seconds(double) const override { return 70.0; }
        } constant;
        check.set_delta_t_model(&constant);
        CalcOptions o;
        o.sigma = false;
        const auto want = check.calc_ut(5, jd, o);
        check.set_delta_t_model(nullptr);
        REQUIRE(want.ok());
        CHECK(d.cols[0] == want.value().pos.lon_deg);
    }
    SUBCASE("TDB rows") {
        eph::Request req = base_request(jd, 1);
        req.timeScale = eph::kTimeTDB;
        req.objs = {body_obj(5)};
        CHECK(s.on_message(request(req, 302), true));
        const Data d = join(drain(s));
        CalcOptions o;
        o.sigma = false;
        const auto want = check.calc(5, time::tt_from_tdb(jd), o);
        REQUIRE(want.ok());
        CHECK(d.cols[0] == want.value().pos.lon_deg);
    }
    SUBCASE("a delta T table, and the delta T column") {
        eph::Request req = base_request(jd, 2);
        req.timeScale = eph::kTimeUT1;
        std::vector<std::pair<eph::Time, double>> table;
        eph::Time a, b;
        a.jd1 = jd - 1.0;
        b.jd1 = jd + 1.0;
        table.push_back({a, 60.0});
        table.push_back({b, 80.0});
        eph::Tlv e;
        eph::EncodeDeltaTTable(table, &e);
        req.ext.push_back(std::move(e));
        req.objs = {body_obj(5)};
        // The columns come with the question, so a second profile-less ask:
        req.profiles[0].columns = eph::kColDeltaT | eph::kColLightTime;
        CHECK(s.on_message(request(req, 303), true));
        const Data d = join(drain(s));
        CHECK(d.columns_present == (eph::kColDeltaT | eph::kColLightTime));
        CHECK(d.n_cols == 8);
        // In A.10 bit order the light-time column (bit 2) comes before the
        // delta T column (bit 3). Halfway between the table's instants at
        // row 0 (jd): 70 s; a quarter further at row 1: 72.5 s.
        CHECK(d.cols[7] == doctest::Approx(70.0).epsilon(1e-9));
        CHECK(d.cols[8 + 7] == doctest::Approx(72.5).epsilon(1e-9)); // row 1: jd+0.25
        // The light-time column is the tau the engine applied, with the
        // table's delta T converting the UT1 row, so the check engine gets
        // the same table.
        struct DeltaTable : time::DeltaTModel {
            // The table above by hand: 60 s at 2451599.5, 80 s at 2451601.5.
            double delta_t_seconds(double jd) const override {
                return 60.0 + 10.0 * (jd - 2451599.5);
            }
        } delta_table;
        check.set_delta_t_model(&delta_table);
        CalcOptions o;
        o.sigma = false;
        const auto want = check.calc_ut(5, jd, o);
        check.set_delta_t_model(nullptr);
        REQUIRE(want.ok());
        CHECK(d.cols[6] == want.value().provenance.light_time_days);
    }
    SUBCASE("an instant list, and a backward grid") {
        eph::Request list;
        list.timeScale = eph::kTimeTT;
        list.timeMode = eph::kTimeList;
        list.profiles.emplace_back();
        list.instants = {{2451600.5, 0.0}, {2451601.5, 0.0}, {2451600.0, 0.0}};
        list.objs = {body_obj(5)};
        CHECK(s.on_message(request(list, 304), true));
        const Data d = join(drain(s));
        REQUIRE(d.meta.size() == 1);
        CHECK(d.meta[0].rowsOk == 3);
        const auto p0 = check.calc(5, 2451600.5, CalcOptions{}).value().pos;
        const auto p2 = check.calc(5, 2451600.0, CalcOptions{}).value().pos;
        CHECK(d.cols[0] == p0.lon_deg);
        CHECK(d.cols[2 * 6] == p2.lon_deg);

        eph::Request back = base_request(2451601.0, 3, -1.0);
        back.objs = {body_obj(5)};
        CHECK(s.on_message(request(back, 305), true));
        const Data b = join(drain(s));
        CHECK(b.meta[0].rowsOk == 3);
        CHECK(b.cols[0] == check.calc(5, 2451601.0, CalcOptions{}).value().pos.lon_deg);
        CHECK(b.cols[6] == check.calc(5, 2451600.0, CalcOptions{}).value().pos.lon_deg);
    }
}

TEST_CASE("server_limits_and_errors") {
    Fixture f;
    Session& s = *f.session;
    f.welcome();

    eph::Request big = base_request(2451545.0, 20000);
    big.objs = {body_obj(10), body_obj(5), body_obj(10), body_obj(5), body_obj(10), body_obj(5)};
    CHECK(s.on_message(request(big, 1), true));
    auto r = drain(s);
    REQUIRE(r.size() == 1);
    CHECK(error_of(r[0]).code == eph::kErrLimits);

    eph::Request over = base_request(2451545.0, 20001);
    over.objs = {body_obj(10)};
    CHECK(s.on_message(request(over, 2), true));
    CHECK(error_of(drain(s)[0]).code == eph::kErrLimits);

    // Answers not yet taken: the fifth is refused before it is computed.
    for (uint32_t i = 0; i < 5; ++i) {
        eph::Request req = base_request(2451545.0 + i, 1);
        req.objs = {body_obj(10)};
        CHECK(s.on_message(request(req, 10 + i), true));
    }
    CHECK(s.queued_answers() == 4);
    r = drain(s);
    REQUIRE(r.size() == 5);
    CHECK(r[0].env.requestId == 14); // control replies go first
    CHECK(error_of(r[0]).code == eph::kErrBusy);
    CHECK((error_of(r[0]).flags & eph::kErrFlagRetryable) != 0);
    CHECK(error_of(r[0]).retryAfterMs > 0);
    for (int i = 1; i < 5; ++i) {
        CHECK(r[i].env.type == eph::kMsgData);
        CHECK(r[i].env.requestId == uint32_t(9 + i));
    }

    std::vector<uint8_t> junk(3, 0);
    CHECK(s.on_message(as_view(message(eph::kMsgRequest, 30, junk.data(), 3)), true));
    CHECK(error_of(drain(s)[0]).code == eph::kErrMalformed);

    SUBCASE("a segments request is refused while segments are dark") {
        eph::Request seg = base_request(2451545.0, 4);
        seg.representation = 1;
        seg.segTargetErrArcsec = 0.01f;
        seg.objs = {body_obj(5)};
        CHECK(s.on_message(request(seg, 40), true));
        CHECK(error_of(drain(s)[0]).code == eph::kErrUnsupported);
    }
    SUBCASE("pins are honoured") {
        eph::Request req = base_request(2451545.0, 1);
        req.objs = {body_obj(10)};
        eph::Tlv pin;
        pin.tag = eph::kReqTagDatasetPin;
        pin.value = std::string("\x0dsynthetic/x#0", 1 + 13);
        req.ext.push_back(pin);
        CHECK(s.on_message(request(req, 50), true));
        CHECK(error_of(drain(s)[0]).code == eph::kErrSource);
    }
}

TEST_CASE("server_lookup") {
    Fixture f;
    Session& s = *f.session;
    f.welcome();
    const auto lookup = [&](const eph::Lookup& l, uint32_t id) {
        std::vector<uint8_t> payload;
        eph::EncodeLookup(&payload, l);
        CHECK(s.on_message(as_view(message(eph::kMsgLookup, id, payload.data(), payload.size())),
                           true));
        const auto r = drain(s);
        REQUIRE(r.size() == 1);
        if (r[0].env.type == eph::kMsgError) {
            return eph::LookupResult{};
        }
        eph::LookupResult lr;
        std::string why;
        REQUIRE_MESSAGE(
            eph::ParseLookupResult(r[0].payload.data(), r[0].payload.size(), &lr, &why) == eph::kOk,
            why);
        return lr;
    };

    SUBCASE("a catalog body by name and by designation") {
        eph::Lookup l;
        l.queries = {"Ceres", "1"};
        const eph::LookupResult lr = lookup(l, 1);
        REQUIRE(lr.queries.size() == 2);
        REQUIRE(lr.queries[0].size() == 1);
        const eph::Match& m = lr.queries[0][0];
        CHECK(m.obj.kind == eph::kObjBody);
        CHECK(m.obj.naif == 20000001);
        CHECK(m.canonicalName == "Ceres");
        CHECK(m.quality == 0); // the canonical name, exactly
        REQUIRE(lr.queries[1].size() == 1);
        CHECK(lr.queries[1][0].quality == 1); // a designation, exactly
        CHECK(lr.queries[1][0].designation == "1");
        CHECK(lr.queries[1][0].canonicalName == "Ceres");
    }
    SUBCASE("stars when asked for, with prefixes; the budget truncates") {
        eph::Lookup l;
        l.maxMatches = 3;
        l.flags = 1 | 4; // prefix, include stars
        l.queries = {"ald", "Aldebaran"};
        const eph::LookupResult lr = lookup(l, 2);
        REQUIRE(lr.queries.size() == 2);
        CHECK((lr.flags & 1) != 0); // the budget ran out inside the first query
        CHECK(lr.queries[0].size() == 3);
        for (const eph::Match& m : lr.queries[0]) {
            CHECK(m.quality == 2);
        }
        CHECK(lr.queries[1].empty());
        // Exact, the star is there and first.
        eph::Lookup exact;
        exact.flags = 4;
        exact.queries = {"Aldebaran"};
        const eph::LookupResult ex = lookup(exact, 3);
        REQUIRE(ex.queries[0].size() >= 1);
        CHECK(ex.queries[0][0].obj.kind == eph::kObjStar);
        CHECK(ex.queries[0][0].quality == 0);
    }
    SUBCASE("without the stars flag, a star name finds nothing") {
        eph::Lookup l;
        l.queries = {"Sirius"};
        const eph::LookupResult lr = lookup(l, 4);
        REQUIRE(lr.queries.size() == 1);
        CHECK(lr.queries[0].empty());
    }
}

TEST_CASE("server_dataset_id") {
    TempFile a{"dataset-a"}, b{"dataset-b"};
    {
        std::ofstream out(a.path);
        out << "one";
    }
    {
        std::ofstream out(b.path);
        out << "two";
    }
    const Dataset d1 = make_dataset("engine", a.path.string(), {}, "");
    const Dataset d2 = make_dataset("engine", a.path.string(), {}, "");
    const Dataset d3 = make_dataset("engine", b.path.string(), {}, "");
    const Dataset d4 = make_dataset("engine2", a.path.string(), {}, "");
    CHECK(d1.id == d2.id);
    CHECK(d1.id != d3.id); // the contents changed
    CHECK(d1.id != d4.id); // the engine changed
    CHECK(d1.id.find("engine/") == 0);
    CHECK(d1.id.find('#') != std::string::npos);
    CHECK(d1.id.size() - d1.id.find('#') == 9); // '#' + 8 hex
    // Same bytes under another name: the same dataset. The readable prefix
    // names the file; the digest after '#' is the identity.
    TempFile c{"dataset-c"};
    {
        std::ofstream out(c.path);
        out << "one";
    }
    const std::string c_id = make_dataset("engine", c.path.string(), {}, "").id;
    CHECK(c_id.substr(c_id.find('#')) == d1.id.substr(d1.id.find('#')));
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
    LimitsConfig lc;
    lc.require_token = true;
    lc.cells_per_sec = 10;
    lc.burst_cells = 20;
    Limits limits(lc, {"secret"});
    ServerConfig config;
    config.max_cells = 20;
    config.engine = "test";
    config.dataset_id = "test#00000000";
    LoopContext ctx(synth::open_synthetic(tf), config, &limits);

    const auto hello_with = [](const char* token) {
        return hello(eph::kProtoVersion, eph::kProtoMin, token);
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
        CHECK((e.flags & eph::kErrFlagClosing) != 0);
        CHECK(ctx.metrics().errors[eph::kErrToken] == 2);
    }
    SUBCASE("a known token has its own budget; over it, ERROR 6") {
        Session s2(ctx, "10.0.0.1");
        CHECK(s2.on_message(hello_with("secret"), true));
        CHECK(drain(s2)[0].env.type == eph::kMsgWelcome);
        eph::Request req = base_request(2451545.0, 20);
        req.objs = {body_obj(10)};
        CHECK(s2.on_message(request(req, 1), true));
        CHECK(drain(s2).back().env.type == eph::kMsgData);
        req.start.jd1 += 1.0;
        CHECK(s2.on_message(request(req, 2), true));
        const auto r = drain(s2);
        REQUIRE(r.size() == 1);
        const auto e = error_of(r[0]);
        CHECK(e.code == eph::kErrRateLimited);
        CHECK((e.flags & eph::kErrFlagRetryable) != 0);
        CHECK(e.retryAfterMs >= 1000);
        CHECK(e.text.rfind("rate limited: 10 cells a second; ask again in ", 0) == 0);
        CHECK(ctx.metrics().hellos == 1);
        CHECK(ctx.metrics().requests == 1);
        CHECK(ctx.metrics().cache_misses == 1);
        CHECK(ctx.metrics().cells_computed == 20);
    }
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

TEST_CASE("server_metrics_text") {
    Metrics a, b;
    ++a.hellos;
    b.hellos += 2;
    a.error(eph::kErrLimits);
    b.error(99);
    a.computed(40, 3.0);
    ServerInfo info;
    info.server_version = kServerVersion;
    info.protocol = 4;
    const std::string text = metrics_text({&a, &b}, info);
    CHECK(text.find("prometheiad_hellos_total 3\n") != std::string::npos);
    CHECK(text.find("prometheiad_errors_total{code=\"2\"} 1\n") != std::string::npos);
    CHECK(text.find("prometheiad_errors_total{code=\"0\"} 1\n") != std::string::npos);
    CHECK(text.find("prometheiad_compute_seconds_bucket{le=\"0.001\"} 0\n") != std::string::npos);
    CHECK(text.find("prometheiad_compute_seconds_bucket{le=\"0.005\"} 1\n") != std::string::npos);
    CHECK(text.find("prometheiad_compute_seconds_count 1\n") != std::string::npos);
    CHECK(text.find("prometheiad_cells_computed_total 40\n") != std::string::npos);
    CHECK(text.find(
              "prometheiad_build_info{server=\"prometheiad/0.3.0\",protocol=\"4\",tls=\"0\"} 1") !=
          std::string::npos);
}

// ---- CANCEL, priority, segments (3.4) ----------------------------------------

// A decoded SEGDATA answer: every object's segment list and the metadata.
struct Segments {
    std::vector<eph::Meta> meta;
    std::vector<std::vector<eph::Segment>> segs;
    std::vector<eph::AyanSeries> ayan;
    uint32_t chunks = 0, n_obj = 0;
};

Segments join_segs(const std::vector<Reply>& replies) {
    Segments d;
    for (const Reply& r : replies) {
        REQUIRE(r.env.type == eph::kMsgSegData);
        eph::SegDataChunk c;
        std::string why;
        REQUIRE_MESSAGE(eph::ParseSegData(r.payload.data(), r.payload.size(), &c, &why) == eph::kOk,
                        why);
        REQUIRE(c.chunkIndex == d.chunks);
        if (c.flags & eph::kChunkMeta) {
            REQUIRE(d.meta.empty());
            d.meta = c.meta;
            d.ayan = c.ayan;
            d.n_obj = c.nObj;
            REQUIRE(c.nObj == c.meta.size());
            d.segs.assign(c.nObj, {});
        }
        REQUIRE(c.nObj == d.n_obj);
        REQUIRE(c.iObj + c.nObjChunk <= c.nObj);
        for (uint32_t i = 0; i < c.nObjChunk; ++i) {
            REQUIRE(c.segs[i].size() == size_t(uint32_t(c.meta[c.iObj + i].rowsOk)));
            d.segs[c.iObj + i] = c.segs[i];
        }
        ++d.chunks;
        if (d.chunks > 1) {
            // nothing
        }
    }
    return d;
}

// The largest angle between a fit and the engine, in arcsec, over a span.
double worst_angle(const eph::Segment* seg, double jd, const double want[3]) {
    double pos[3], vel[3];
    seg->Eval(eph::Time{jd, 0.0}, pos, vel);
    const double cross[3] = {pos[1] * want[2] - pos[2] * want[1],
                             pos[2] * want[0] - pos[0] * want[2],
                             pos[0] * want[1] - pos[1] * want[0]};
    const double n = std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
    const double dot = pos[0] * want[0] + pos[1] * want[1] + pos[2] * want[2];
    return std::atan2(n, dot) * (180.0 / 3.14159265358979323846) * 3600.0;
}

const eph::Segment* covering(const std::vector<eph::Segment>& segs, double jd) {
    for (const eph::Segment& g : segs) {
        if (jd >= g.mid.jd1 - g.halfSpanDays - 1e-9 && jd <= g.mid.jd1 + g.halfSpanDays + 1e-9) {
            return &g;
        }
    }
    return nullptr;
}

TEST_CASE("server_cancel") {
    Fixture f;
    Session& s = *f.session;
    f.welcome();

    SUBCASE("an unknown id gets nothing; others keep working") {
        eph::Request req = base_request(2451545.0, 2);
        req.objs = {body_obj(10)};
        CHECK(s.on_message(request(req, 1), true));
        CHECK(s.on_message(as_view(message(eph::kMsgCancel, 42, nullptr, 0)), true));
        const auto replies = drain(s);
        REQUIRE(replies.size() == 1);
        CHECK(replies[0].env.type == eph::kMsgData);
        CHECK(replies[0].env.requestId == 1);
        CHECK(f.ctx->metrics().cancels == 0);
    }

    SUBCASE("a cancelled request stops work and caches nothing") {
        eph::Request req = base_request(2451545.0, 20000, 1.0 / 24.0); // 20000 hourly rows
        req.objs = {body_obj(10), body_obj(399), body_obj(5)};
        CHECK(s.on_message(request(req, 7), true));
        CHECK(s.has_work());
        s.work(); // one slice: a fraction of the rows, never the whole answer
        CHECK(s.has_work());
        CHECK(f.ctx->cache().entries() == 0); // partial answers are never cached
        CHECK(s.on_message(as_view(message(eph::kMsgCancel, 7, nullptr, 0)), true));
        CHECK(!s.has_work());
        CHECK(s.queued_answers() == 0);
        const auto replies = drain(s);
        REQUIRE(replies.size() == 1);
        REQUIRE(replies[0].env.type == eph::kMsgError);
        const eph::Error e = error_of(replies[0]);
        CHECK(e.code == eph::kErrCancelled);
        CHECK(e.flags == 0); // not closing, not retryable
        CHECK(e.retryAfterMs == 0);
        CHECK(f.ctx->cache().entries() == 0);
        CHECK(f.ctx->metrics().cancels == 1);
        CHECK(f.ctx->metrics().cache_misses == 0); // the compute never finished

        // The same question asked again is answered whole: the cancellation
        // left no half answer behind.
        CHECK(s.on_message(request(req, 8), true));
        const Data d = join(drain(s));
        REQUIRE(d.meta.size() == 3);
        CHECK(d.meta[0].rowsOk == 20000);
        CHECK(f.ctx->cache().entries() == 1);
        CHECK(f.ctx->metrics().cancels == 1);
    }

    SUBCASE("a request answered completely gets nothing") {
        eph::Request req = base_request(2451545.0, 3);
        req.objs = {body_obj(10)};
        CHECK(s.on_message(request(req, 3), true));
        CHECK(drain(s).size() == 1); // one chunk, the last
        CHECK(s.on_message(as_view(message(eph::kMsgCancel, 3, nullptr, 0)), true));
        CHECK(drain(s).empty());
        CHECK(f.ctx->metrics().cancels == 0);
    }
}

TEST_CASE("server_priority_interactive_before_prefetch") {
    Fixture f;
    Session& s = *f.session;
    f.welcome();

    eph::Request prefetch = base_request(2451545.0, 3000, 1.0 / 24.0);
    prefetch.objs = {body_obj(10), body_obj(399), body_obj(5)};
    prefetch.priority = 1;
    CHECK(s.on_message(request(prefetch, 21), true));
    eph::Request interactive = base_request(2451550.0, 4);
    interactive.objs = {body_obj(10)};
    interactive.priority = 0;
    CHECK(s.on_message(request(interactive, 22), true));

    const auto replies = drain(s);
    // The interactive answer is worked and sent before the prefetch (3.5),
    // even though the prefetch arrived first.
    size_t first_data = replies.size();
    for (size_t i = 0; i < replies.size(); ++i) {
        if (replies[i].env.type == eph::kMsgData) {
            first_data = i;
            break;
        }
    }
    REQUIRE(first_data < replies.size());
    CHECK(replies[first_data].env.requestId == 22);
    // Both answers arrive complete, each in order.
    uint32_t rows21 = 0, rows22 = 0;
    for (const Reply& r : replies) {
        if (r.env.type != eph::kMsgData) {
            continue;
        }
        eph::DataChunk c;
        std::string why;
        REQUIRE(eph::ParseData(r.payload.data(), r.payload.size(), &c, &why) == eph::kOk);
        if (r.env.requestId == 21) {
            CHECK(c.iTime == rows21);
            rows21 += c.nRows;
        } else {
            CHECK(r.env.requestId == 22);
            CHECK(c.iTime == rows22);
            rows22 += c.nRows;
        }
    }
    CHECK(rows21 == 3000);
    CHECK(rows22 == 4);
}

TEST_CASE("server_segments") {
    Fixture f;
    Session& s = *f.session;
    Engine check = synth::open_synthetic(f.tf);
    REQUIRE(
        check.add_catalog(std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/sample-100.epm").ok());
    f.welcome();

    const auto seg_request = [&](double jd, uint32_t n_time, double step_days) {
        eph::Request req = base_request(jd, n_time, step_days);
        req.representation = 1;
        req.segTargetErrArcsec = 0.1f;
        req.profiles[0].observer = eph::kObsHelio;
        req.profiles[0].corrections = eph::kCorrLightTime | eph::kCorrAberration;
        req.profiles[0].form = eph::kFormRectangular;
        return req;
    };

    SUBCASE("segments answer, evaluate against the engine, and share cells") {
        eph::Request req = seg_request(2451545.5, 10, 4.0); // 36 days: 3 cells
        req.objs = {body_obj(5), body_obj(399)};
        CHECK(s.on_message(request(req, 5), true));
        const Segments d = join_segs(drain(s));
        CHECK(d.n_obj == 2);
        REQUIRE(d.segs[0].size() > 0);
        REQUIRE(d.segs[1].size() > 0);

        CHECK(d.meta[0].name == "Jupiter");
        CHECK(d.meta[0].rowsOk == int32_t(d.segs[0].size()));
        CHECK(d.meta[0].errCode == eph::kOErrNone);
        CHECK(d.meta[0].errText.empty());
        CHECK(d.meta[0].resolvedNaif == 5);
        CHECK(d.meta[0].firstFailedRow == eph::kRowNone);
        // A heliocentric observer cannot be deflected; light time and
        // aberration are live (the same table DATA answers use).
        CHECK(d.meta[0].corrApplied == (eph::kCorrLightTime | eph::kCorrAberration));
        CHECK(d.meta[0].sourceIdx != eph::kSourceNone);
        CHECK(d.ayan.empty()); // tropical profiles carry no series

        // Whole cells, contiguous, within the degree cap.
        const double from = Lattice::cell_start(Lattice::cell_of(2451545.5));
        const double to = Lattice::cell_end(Lattice::cell_of(2451545.5 + 36.0));
        double prev_end = from;
        float worst_declared = 0.0f;
        for (const eph::Segment& g : d.segs[0]) {
            CHECK(std::fabs((g.mid.jd1 - g.halfSpanDays) - prev_end) < 1e-9);
            prev_end = g.mid.jd1 + g.halfSpanDays;
            CHECK(g.degree <= uint8_t(kSegMaxDegree));
            worst_declared = std::max(worst_declared, g.errArcsec);
        }
        CHECK(prev_end == doctest::Approx(to));
        CHECK(worst_declared <= 0.1f); // the target, met and measured

        // The fit is tropical and answers the engine within what it declares.
        CalcOptions o;
        o.center = Center::Heliocentric;
        o.sigma = false;
        double worst = 0.0, worst_rate = 0.0;
        for (int i = 0; i <= 400; ++i) {
            const double jd = from + i * (to - from) / 400.0;
            const eph::Segment* seg = covering(d.segs[0], jd);
            REQUIRE(seg != nullptr);
            const Position p = check.calc(5, jd, o).value().pos;
            worst = std::max(worst, worst_angle(seg, jd, p.xyz_au));
            double pos[3], vel[3];
            seg->Eval(eph::Time{jd, 0.0}, pos, vel);
            const double dv[3] = {vel[0] - p.vel_au_day[0], vel[1] - p.vel_au_day[1],
                                  vel[2] - p.vel_au_day[2]};
            const double dist = std::sqrt(p.xyz_au[0] * p.xyz_au[0] + p.xyz_au[1] * p.xyz_au[1] +
                                          p.xyz_au[2] * p.xyz_au[2]);
            worst_rate =
                std::max(worst_rate, std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]) /
                                         dist * (180.0 / 3.14159265358979323846) * 3600.0);
        }
        CHECK(worst <= 0.1);
        CHECK(worst_rate <= 0.5); // the rate residual is declared, not gated

        // A second identical request serves the same cells: the lattice, not
        // the ask, is what was fitted.
        const uint64_t hits = f.ctx->seg_cache().hits();
        CHECK(s.on_message(request(req, 6), true));
        const Segments again = join_segs(drain(s));
        CHECK(again.segs[0].size() == d.segs[0].size());
        CHECK(f.ctx->seg_cache().hits() > hits);
        CHECK(f.ctx->seg_cache().entries() >= 3);
    }

    SUBCASE("a sidereal profile carries its ayanamsa series") {
        eph::Request req = seg_request(2451545.5, 10, 4.0);
        req.profiles[0].zodiac = "lahiri";
        req.objs = {body_obj(5)};
        CHECK(s.on_message(request(req, 5), true));
        const Segments d = join_segs(drain(s));
        CHECK(d.meta[0].errCode == eph::kOErrNone);
        REQUIRE(d.ayan.size() == 1);
        CHECK(d.ayan[0].profile == 0);
        REQUIRE(!d.ayan[0].segs.empty());

        // The ayanamsa series answers the engine's own ayanamsa within its
        // declared error.
        CalcOptions sid; // the ayanamsa is observer-independent; the Sun is
        // asked geocentrically so the engine always has it.
        sid.center = Center::Geocentric;
        sid.sidereal = SiderealMode::Lahiri;
        sid.sigma = false;
        double worst = 0.0, worst_declared = 0.0;
        for (const eph::AyanSeg& g : d.ayan[0].segs) {
            worst_declared = std::max(worst_declared, double(g.errArcsec));
        }
        const double ayan_from = Lattice::cell_start(Lattice::cell_of(2451545.5));
        const double ayan_to = Lattice::cell_end(Lattice::cell_of(2451545.5 + 36.0));
        for (int i = 0; i <= 400; ++i) {
            const double jd = ayan_from + i * (ayan_to - ayan_from) / 400.0;
            const eph::AyanSeg* seg = nullptr;
            for (const eph::AyanSeg& g : d.ayan[0].segs) {
                if (jd >= g.mid.jd1 - g.halfSpanDays - 1e-9 &&
                    jd <= g.mid.jd1 + g.halfSpanDays + 1e-9) {
                    seg = &g;
                    break;
                }
            }
            REQUIRE(seg != nullptr);
            const auto r = check.calc(10, jd, sid);
            REQUIRE(r.ok());
            REQUIRE(r.value().ayanamsa_deg.has_value());
            worst = std::max(
                worst,
                std::fabs(seg->Eval(eph::Time{jd, 0.0}) - r.value().ayanamsa_deg.value()) * 3600.0);
        }
        CHECK(worst_declared <= 0.1f);
        CHECK(worst <= 0.1);
    }

    SUBCASE("objects this server does not fit are per-object errors") {
        eph::Request req = seg_request(2451545.5, 5, 4.0);
        eph::Object star;
        star.kind = eph::kObjStar;
        star.name = "Sirius";
        eph::Object luno;
        luno.kind = eph::kObjOrbitPoint;
        luno.naif = 301;
        luno.method = eph::kMethOsculating;
        req.objs = {star, body_obj(5), luno};
        CHECK(s.on_message(request(req, 5), true));
        const Segments d = join_segs(drain(s));
        CHECK(d.n_obj == 3);
        CHECK(d.meta[0].errCode == eph::kOErrUnsupported);
        CHECK(d.meta[0].rowsOk == 0);
        CHECK(d.segs[0].empty());
        CHECK(d.meta[1].errCode == eph::kOErrNone);
        CHECK(!d.segs[1].empty());
        CHECK(d.meta[2].errCode == eph::kOErrUnsupported);
        CHECK(d.meta[2].rowsOk == 0);
        CHECK(d.segs[2].empty());
    }

    SUBCASE("refusals that cost no work") {
        // Over the advertised span bound.
        eph::Request too_long = seg_request(2451545.0, 60, 40.0); // 2360 days
        too_long.objs = {body_obj(5)};
        CHECK(s.on_message(request(too_long, 30), true));
        auto r = drain(s);
        REQUIRE(r.size() == 1);
        const eph::Error e1 = error_of(r[0]);
        CHECK(e1.code == eph::kErrLimits);
        CHECK(e1.text.find("segMaxSpanDays") != std::string::npos);
        CHECK(f.ctx->seg_cache().misses() == 0);

        // Finer than the advertised floor: refused, not served coarser.
        eph::Request too_fine = seg_request(2451545.0, 5, 1.0);
        too_fine.segTargetErrArcsec = 0.0005f;
        too_fine.objs = {body_obj(5)};
        CHECK(s.on_message(request(too_fine, 31), true));
        r = drain(s);
        REQUIRE(r.size() == 1);
        CHECK(error_of(r[0]).code == eph::kErrLimits);

        // The codec's own refusals: a spherical profile, and a list.
        eph::Request spherical = base_request(2451545.0, 5, 1.0);
        spherical.representation = 1;
        spherical.segTargetErrArcsec = 0.1f;
        spherical.objs = {body_obj(5)};
        CHECK(s.on_message(request(spherical, 32), true));
        r = drain(s);
        REQUIRE(r.size() == 1);
        CHECK(error_of(r[0]).code == eph::kErrUnsupported);

        eph::Request listreq = seg_request(2451545.0, 3, 1.0);
        listreq.timeMode = eph::kTimeList;
        for (int i = 0; i < 3; ++i) {
            eph::Time t;
            t.jd1 = 2451545.0 + i;
            listreq.instants.push_back(t);
        }
        listreq.objs = {body_obj(5)};
        CHECK(s.on_message(request(listreq, 33), true));
        r = drain(s);
        REQUIRE(r.size() == 1);
        CHECK(error_of(r[0]).code == eph::kErrUnsupported);

        // One instant still serves the cell covering it.
        eph::Request one = seg_request(2451545.0, 1, 1.0);
        one.objs = {body_obj(5)};
        CHECK(s.on_message(request(one, 34), true));
        const Segments d = join_segs(drain(s));
        CHECK(d.meta[0].errCode == eph::kOErrNone);
        CHECK(d.segs[0].size() >= 1);
    }
}

// ---- hypothetical bodies (kinds 3 and 4) ------------------------------------

namespace {

// An invented body; every number is made up for the test.
constexpr const char* kInventedBody =
    R"({"token":"testbody","name":"Test Body","set":"Invented for tests","citation":"tests/test_server.cpp","epoch":2451545.0,"equinox":"J2000","origin":"sun","M":[10.0],"a":[4.0],"e":[0.05],"w":[20.0],"node":[30.0],"i":[1.5]})";

// The same body as kind-4 elements, as a client would send it.
eph::Object invented_as_elements(double mean_anomaly_deg = 10.0) {
    eph::Object o;
    o.kind = eph::kObjElements;
    o.epoch = eph::Time{2451545.0, 0.0};
    o.equinox = eph::kEqJ2000;
    o.centre = 0;
    o.nTerms = 1;
    o.coef = {mean_anomaly_deg, 4.0, 0.05, 20.0, 30.0, 1.5}; // M, a, e, w, node, i
    o.name = "sent elements";
    return o;
}

eph::Object named(const char* token) {
    eph::Object o;
    o.kind = eph::kObjHypothetical;
    o.name = token;
    return o;
}

struct ElementFile {
    std::string path = (std::filesystem::temp_directory_path() /
                        ("prometheia-server-hyp-" + std::to_string(::getpid()) + ".jsonl"))
                           .string();
    explicit ElementFile(const std::string& text) {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f);
        std::fputs(text.c_str(), f);
        std::fclose(f);
    }
    ~ElementFile() { std::remove(path.c_str()); }
};

} // namespace

TEST_CASE("server_hypotheticals") {
    SUBCASE("with only the shipped set, WELCOME advertises exactly its tokens") {
        Fixture f;
        eph::Welcome w = f.welcome();
        eph::Capabilities c;
        std::string why;
        REQUIRE(eph::ParseCapabilities(w.caps_, &c, &why) == eph::kOk);
        CHECK(c.Kind(eph::kObjElements));
        // Whatever data/hypotheticals.jsonl holds, in its order; kind 3 is
        // advertised exactly when that is not empty.
        auto shipped = hypotheticals::parse(hypotheticals::shipped(), "shipped");
        REQUIRE(shipped.ok());
        std::vector<std::string> want;
        for (const hypotheticals::Body& b : shipped.value()) {
            if (std::find(want.begin(), want.end(), b.token) == want.end()) {
                want.push_back(b.token);
            }
        }
        CHECK(c.hypotheticals == want);
        CHECK(c.Kind(eph::kObjHypothetical) == !want.empty());
        // A.3 0x0012: every A.16 equinox.
        bool saw_equinoxes = false;
        for (const eph::Tlv& t : w.caps_) {
            if (t.tag == eph::kCapTagEquinoxes) {
                REQUIRE(t.value.size() == 4);
                const auto* b = reinterpret_cast<const uint8_t*>(t.value.data());
                const uint32_t mask = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 |
                                      uint32_t(b[3]) << 24;
                CHECK(mask == 0x1Fu);
                saw_equinoxes = true;
            }
        }
        CHECK(saw_equinoxes);
    }

    ElementFile file(std::string(kInventedBody) + "\n");
    Fixture f(file.path);
    Session& s = *f.session;
    eph::Welcome w = f.welcome();
    eph::Capabilities c;
    std::string why;
    REQUIRE(eph::ParseCapabilities(w.caps_, &c, &why) == eph::kOk);
    CHECK(c.Kind(eph::kObjHypothetical));
    CHECK(std::find(c.hypotheticals.begin(), c.hypotheticals.end(), "testbody") !=
          c.hypotheticals.end());

    SUBCASE("by name and by elements, the same body answers the same") {
        eph::Request req = base_request(2451545.0 + 300.0, 3, 10.0);
        req.objs = {named("testbody"), invented_as_elements(), named("nosuchbody")};
        CHECK(s.on_message(request(req, 7), true));
        const Data d = join(drain(s));
        REQUIRE(d.meta.size() == 3);

        const eph::Meta& by_name = d.meta[0];
        CHECK(by_name.errCode == eph::kOErrNone);
        CHECK(by_name.name == "Test Body");
        CHECK(by_name.rowsOk == 3);
        CHECK(by_name.resolvedNaif == eph::kNaifNone); // not a NAIF body
        CHECK(by_name.corrApplied == eph::kCorrMask);  // geocentric: all three

        const eph::Meta& by_elements = d.meta[1];
        CHECK(by_elements.errCode == eph::kOErrNone);
        CHECK(by_elements.name == "sent elements");
        CHECK(by_elements.resolvedNaif == eph::kNaifNone);

        // The same elements, however they arrive: bit for bit.
        for (int k = 0; k < 3 * d.n_cols; ++k) {
            CHECK(std::isfinite(d.cols[k]));
            CHECK(d.cols[k] == d.cols[size_t(3) * d.n_cols + k]);
        }

        // A.17: a token this server does not define is an unknown body.
        CHECK(d.meta[2].errCode == eph::kOErrUnknownBody);
        CHECK(d.meta[2].rowsOk == 0);
    }

    SUBCASE("fitted cells are keyed by the elements, not only the kind") {
        // Two requests for kind-4 bodies differing only in M. Were the cell
        // key blind to the elements, the second would be served the first's
        // cells from the cache.
        const auto fit = [&](double m, uint32_t id) {
            eph::Request req = base_request(2451545.5, 10, 4.0);
            req.representation = 1;
            req.segTargetErrArcsec = 0.1f;
            req.profiles[0].observer = eph::kObsHelio;
            req.profiles[0].corrections = eph::kCorrLightTime | eph::kCorrAberration;
            req.profiles[0].form = eph::kFormRectangular;
            req.objs = {invented_as_elements(m)};
            CHECK(s.on_message(request(req, id), true));
            return join_segs(drain(s));
        };
        const Segments a = fit(10.0, 11);
        const Segments b = fit(100.0, 12);
        REQUIRE(a.segs.size() == 1);
        REQUIRE(b.segs.size() == 1);
        REQUIRE(!a.segs[0].empty());
        REQUIRE(!b.segs[0].empty());
        CHECK(a.meta[0].errCode == eph::kOErrNone);
        double pa[3], pb[3], va[3], vb[3];
        a.segs[0][0].Eval(eph::Time{2451545.5, 0.0}, pa, va);
        b.segs[0][0].Eval(eph::Time{2451545.5, 0.0}, pb, vb);
        // 90 degrees of mean anomaly apart: nowhere near each other.
        const double gap =
            std::sqrt((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) +
                      (pa[2] - pb[2]) * (pa[2] - pb[2]));
        CHECK(gap > 1.0); // AU
    }
}

TEST_CASE("server_error_text_never_quotes_the_request") {
    // 3.8: META errText never contains request contents. Each object below
    // fails, and each carries something recognisable the client sent; none
    // of it may come back.
    Fixture f;
    Session& s = *f.session;
    f.welcome();
    eph::Request req = base_request(2451545.0, 1);
    eph::Object star;
    star.kind = eph::kObjStar;
    star.name = "Zzstarzz";
    eph::Object designation;
    designation.kind = eph::kObjDesignation;
    designation.name = "Zzdesignationzz";
    eph::Object hyp;
    hyp.kind = eph::kObjHypothetical;
    hyp.name = "zztokenzz";
    req.objs = {star, designation, body_obj(-5), body_obj(987654321), hyp};
    CHECK(s.on_message(request(req, 3), true));
    const Data d = join(drain(s));
    REQUIRE(d.meta.size() == 5);
    for (const eph::Meta& m : d.meta) {
        CHECK(m.errCode != eph::kOErrNone);
        CHECK(!m.errText.empty());
        for (const char* sent : {"Zzstarzz", "zzstarzz", "Zzdesignationzz", "zzdesignationzz",
                                 "zztokenzz", "-5", "987654321", "2451545"}) {
            CHECK_MESSAGE(m.errText.find(sent) == std::string::npos, m.errText);
        }
    }
    CHECK(d.meta[0].errCode == eph::kOErrUnknownBody);
    CHECK(d.meta[4].errCode == eph::kOErrUnknownBody);

    // Nor may what the client sends steer the classification: unknown names
    // that contain the words the classifier listens for are still unknown.
    eph::Request steer = base_request(2451545.0, 1);
    eph::Object sneaky_hyp;
    sneaky_hyp.kind = eph::kObjHypothetical;
    sneaky_hyp.name = "x is ambiguous";
    eph::Object sneaky_star;
    sneaky_star.kind = eph::kObjStar;
    sneaky_star.name = "Zz nodes are undefined";
    eph::Object sneaky_name;
    sneaky_name.kind = eph::kObjDesignation;
    sneaky_name.name = "integration failed";
    steer.objs = {sneaky_hyp, sneaky_star, sneaky_name};
    CHECK(s.on_message(request(steer, 5), true));
    const Data st = join(drain(s));
    REQUIRE(st.meta.size() == 3);
    for (const eph::Meta& m : st.meta) {
        CHECK_MESSAGE(m.errCode == eph::kOErrUnknownBody, m.errText);
    }

    // And a whole-request refusal: an unserved zodiac is ERROR 11, and its
    // text does not repeat the token either.
    eph::Request zod = base_request(2451545.0, 1);
    zod.profiles[0].zodiac = "zzzodiaczz";
    zod.objs = {body_obj(10)};
    CHECK(s.on_message(request(zod, 4), true));
    const auto replies = drain(s);
    REQUIRE(replies.size() == 1);
    const eph::Error e = error_of(replies[0]);
    CHECK(e.code == eph::kErrUnsupported);
    CHECK(e.text.find("zzzodiaczz") == std::string::npos);
}

TEST_CASE("server_refuses_a_correction_mask_it_does_not_advertise") {
    // 3.5a: "A server advertises, per observer, the masks it can honour (A.3
    // 0x0004); any other combination is ERROR 11." The Sun's centre cannot be
    // deflected, so a heliocentric profile asking for deflection is refused
    // whole -- not answered quietly without it.
    Fixture f;
    Session& s = *f.session;
    f.welcome();
    eph::Request req = base_request(2451545.0, 1);
    req.profiles[0].observer = eph::kObsHelio;
    req.profiles[0].corrections = eph::kCorrMask; // includes deflection
    req.objs = {body_obj(5)};
    CHECK(s.on_message(request(req, 9), true));
    const auto replies = drain(s);
    REQUIRE(replies.size() == 1);
    CHECK(error_of(replies[0]).code == eph::kErrUnsupported);

    // The same observer with a mask it does advertise is answered, and so is
    // mask 0 anywhere: the geometric position, the portable comparison.
    for (uint8_t mask : {uint8_t(eph::kCorrLightTime | eph::kCorrAberration), uint8_t(0)}) {
        req.profiles[0].corrections = mask;
        CHECK(s.on_message(request(req, 10 + mask), true));
        const Data d = join(drain(s));
        REQUIRE(d.meta.size() == 1);
        CHECK(d.meta[0].errCode == eph::kOErrNone);
    }
}

// The log traces a request from HELLO to its last chunk, and never carries
// what was asked: no instant, no site (server/log.hpp).
TEST_CASE("server_log_traces_a_request_without_its_contents") {
    const auto read_all = [](std::FILE* f) {
        std::fflush(f);
        std::rewind(f);
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            text.append(buf, n);
        }
        return text;
    };
    std::FILE* f = std::tmpfile();
    REQUIRE(f);
    const Log log(LogLevel::Info, f);
    Fixture fx({}, &log);
    fx.welcome();

    eph::Request req = base_request(2451545.123456, 3);
    req.profiles[0].observer = eph::kObsTopo;
    req.profiles[0].siteLonEastDeg = 8.5501;
    req.profiles[0].siteLatDeg = 47.3701;
    req.profiles[0].siteHeightM = 432.1;
    req.objs = {body_obj(10), body_obj(301)};
    CHECK(fx.session->on_message(request(req, 5), true));
    drain(*fx.session);

    eph::Request helio = base_request(2451545.0, 1);
    helio.profiles[0].observer = eph::kObsHelio;
    helio.profiles[0].corrections = eph::kCorrMask; // deflection at the Sun: ERROR 11
    helio.objs = {body_obj(4)};
    CHECK(fx.session->on_message(request(helio, 6), true));
    drain(*fx.session);

    const std::string text = read_all(f);
    INFO(text);
    CHECK(text.find("c=0.7 hello v=4") != std::string::npos);
    CHECK(text.find("c=0.7 req=5 accepted rows objs=2 (body:2) rows=3 profiles=1") !=
          std::string::npos);
    CHECK(text.find("c=0.7 req=5 done rows objs=2 rows=3 chunks=1") != std::string::npos);
    CHECK(text.find("c=0.7 req=6 error=11") != std::string::npos);
    for (const char* secret : {"2451545", "8.55", "47.37", "432"}) {
        CHECK_MESSAGE(text.find(secret) == std::string::npos, secret);
    }
    std::fclose(f);

    std::FILE* q = std::tmpfile();
    REQUIRE(q);
    const Log quiet(LogLevel::Quiet, q);
    Fixture fq({}, &quiet);
    fq.welcome();
    CHECK(fq.session->on_message(request(req, 5), true));
    drain(*fq.session);
    CHECK(read_all(q).empty());
    std::fclose(q);

    CHECK(log_addr("0000:0000:0000:0000:0000:ffff:7f00:0001") == "127.0.0.1");
    CHECK(log_addr("2001:0db8:0000:0000:0000:0000:0000:0001") ==
          "2001:0db8:0000:0000:0000:0000:0000:0001");
    CHECK(log_safe("a\"b\\c\n") == "a?b?c?");
    CHECK(parse_log_level("debug") == LogLevel::Debug);
    CHECK_FALSE(parse_log_level("loud"));
}

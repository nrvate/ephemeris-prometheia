// SPDX-License-Identifier: GPL-2.0-or-later
//
// The object registry (prometheia/registry.hpp, docs/REGISTRY.md): ids that
// round-trip, lookup across every namespace, info and listing.
//
// Part A runs everywhere: the synthetic engine with tests/data/sample-100.epm
// (the first 100 numbered asteroids). Part B (skipped by default; run with
// -tc='registry_full_catalog*' --no-skip) reads the full catalog
// (PROMETHEIA_CATALOG, default sbdb-20261027.epm in the source tree) for the
// names astromcp sends.
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <prometheia/engine.hpp>
#include <prometheia/registry.hpp>
#include <prometheia/stars.hpp>

#include "synthetic_spk.hpp"
#include <doctest/doctest.h>

using namespace prometheia;
using namespace prometheia::registry;
using synth::open_synthetic;

namespace {

Engine open_with_sample(synth::TempFile& tf) {
    Engine e = open_synthetic(tf);
    REQUIRE(e.add_catalog(std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/sample-100.epm").ok());
    return e;
}

// The single best match, failing the test when the query is ambiguous.
Match only(Registry& r, const std::string& q, bool prefix = false) {
    auto m = r.lookup(q, prefix);
    REQUIRE_MESSAGE(m.ok(), m.error().message);
    REQUIRE_MESSAGE(!m.value().empty(), q);
    const auto& v = m.value();
    if (v.size() > 1) {
        const bool tie =
            (v[0].quality == MatchQuality::Exact || v[0].quality == MatchQuality::OldDesignation) ==
                (v[1].quality == MatchQuality::Exact ||
                 v[1].quality == MatchQuality::OldDesignation) &&
            v[0].quality != MatchQuality::Prefix;
        CHECK_MESSAGE(!tie, q, " is ambiguous: ", v[0].id, " and ", v[1].id);
    }
    return v[0];
}

} // namespace

TEST_CASE("registry_fold") {
    CHECK(fold("Gǃkúnǁʼhòmdímà") == "gkunhomdima");
    CHECK(fold("G!kun||'homdima") == "gkunhomdima");
    CHECK(fold("2003 VB12") == "2003vb12");
    CHECK(fold("Ŀłœß") == "lloess");
    CHECK(fold("\xff\xfe") == ""); // malformed UTF-8 is dropped, not misread
}

TEST_CASE("registry_ids_round_trip") {
    for (const char* id : {"naif:10", "naif:301", "naif:5", "sb:136199", "sb:2017 OF201",
                           "hyp:cupido", "orbit:naif:301:ascending-node:mean",
                           "orbit:sb:2060:perihelion:osculating", "star:HR 5984"}) {
        auto p = parse_id(id);
        REQUIRE_MESSAGE(p.ok(), id);
        CHECK(to_string(p.value()) == id);
    }
    // Aliases parse to the canonical form.
    CHECK(to_string(parse_id("orbit:naif:301:apogee:mean").value()) ==
          "orbit:naif:301:aphelion:mean");
    CHECK(to_string(parse_id("star:Regulus").value()) == "star:HR 3982");
    // Every star's id resolves back to it: designations are unique.
    for (size_t i = 0; i < stars::count(); ++i) {
        Id id;
        id.space = Id::Space::Star;
        id.star = i;
        auto back = parse_id(to_string(id));
        REQUIRE_MESSAGE(back.ok(), to_string(id));
        CHECK_MESSAGE(back.value().star == i, to_string(id));
    }
    for (const char* bad :
         {"", "mars", "naif:", "naif:x", "orbit:naif:301:top:mean", "orbit:naif:301:perihelion",
          "orbit:hyp:cupido:perihelion:mean", "planet:5"})
        CHECK_MESSAGE(parse_id(bad).error().code == ErrorCode::ArgumentError, bad);
    CHECK(parse_id("star:Nosuchstar").error().code == ErrorCode::NotFound);
}

TEST_CASE("registry_lookup") {
    synth::TempFile tf("registry-lookup");
    Engine e = open_with_sample(tf);
    Registry r(e);
    CHECK(only(r, "Mars").id == "naif:4");
    CHECK(only(r, "mars").kind == Kind::Planet);
    CHECK(only(r, "Moon").kind == Kind::Moon);
    CHECK(only(r, "true node").id == "orbit:naif:301:ascending-node:osculating");
    CHECK(only(r, "Lilith").id == "orbit:naif:301:aphelion:mean");
    CHECK(only(r, "Black Moon Lilith").name == "mean apogee");
    CHECK(only(r, "cupido").id == "hyp:cupido");
    CHECK(only(r, "Cupido").kind == Kind::Hypothetical);
    CHECK(only(r, "Regulus").id == "star:HR 3982");
    CHECK(only(r, "alpha1 Cap").kind == Kind::Star);
    const Match m44 = only(r, "M44");
    CHECK(m44.id == "star:M 44");
    CHECK(m44.kind == Kind::DeepSky);
    const Match ceres = only(r, "Ceres");
    CHECK(ceres.id == "sb:1");
    CHECK(ceres.kind == Kind::Asteroid);
    CHECK(only(r, "CERES").id == "sb:1");
    CHECK(only(r, "4").id == "sb:4"); // a number is a numbered body's designation
    // Prefixes reach every namespace that holds names in a list, and the
    // catalog's proper names.
    auto pre = r.lookup("cer", true);
    REQUIRE(pre.ok());
    bool saw_ceres = false;
    for (const Match& m : pre.value())
        saw_ceres = saw_ceres || m.id == "sb:1";
    CHECK(saw_ceres);
    auto node = r.lookup("node", true);
    REQUIRE(node.ok());
    CHECK(node.value().size() >= 4);
    for (const Match& m : node.value())
        CHECK(m.quality == MatchQuality::Prefix);
    // Nothing, and the empty query.
    CHECK(r.lookup("Nosuchbody").value().empty());
    CHECK(r.lookup("!!").value().empty());
}

TEST_CASE("registry_info") {
    synth::TempFile tf("registry-info");
    Engine e = open_with_sample(tf);
    Registry r(e);
    const double p = 5028.796195 / 3600.0 / 36525.0; // general precession, deg/day

    auto jup = r.info("naif:5");
    REQUIRE(jup.ok());
    CHECK(jup.value().name == "Jupiter");
    CHECK(jup.value().standing == Standing::Iau);
    CHECK(std::fabs(jup.value().sidereal_mean_motion_deg_day - 0.083091) < 2e-5);
    CHECK(std::fabs(jup.value().period_days - 4332.6) < 1.0);
    CHECK(jup.value().mean_motion_deg_day - jup.value().sidereal_mean_motion_deg_day ==
          doctest::Approx(p));

    auto sun = r.info("naif:10");
    REQUIRE(sun.ok());
    CHECK(std::fabs(sun.value().mean_motion_deg_day - 0.9856474) < 2e-6); // the tropical year
    auto moon = r.info("naif:301");
    REQUIRE(moon.ok());
    CHECK(std::fabs(moon.value().mean_motion_deg_day - 13.1763965) < 1e-6);
    CHECK(std::fabs(moon.value().period_days - 27.321662) < 1e-5); // the sidereal month

    auto node = r.info("orbit:naif:301:ascending-node:mean");
    REQUIRE(node.ok());
    CHECK(node.value().kind == Kind::LunarPoint);
    CHECK(node.value().name == "mean node");
    CHECK(std::fabs(node.value().mean_motion_deg_day - -0.0529539) < 1e-6); // retrograde
    auto apogee = r.info("orbit:naif:301:aphelion:mean");
    REQUIRE(apogee.ok());
    CHECK(std::fabs(apogee.value().mean_motion_deg_day - 0.1114041) < 1e-6);
    auto true_node = r.info("orbit:naif:301:ascending-node:osculating");
    REQUIRE(true_node.ok());
    CHECK(std::isnan(true_node.value().mean_motion_deg_day));
    auto sun_perigee = r.info("orbit:naif:10:perihelion:mean");
    REQUIRE(sun_perigee.ok());
    CHECK(sun_perigee.value().name == "Sun mean perigee");
    CHECK(sun_perigee.value().kind == Kind::OrbitPoint);
    auto jup_node = r.info("orbit:naif:5:ascending-node:mean");
    REQUIRE(jup_node.ok());
    CHECK(jup_node.value().name == "Jupiter mean ascending node");
    CHECK(std::fabs(jup_node.value().mean_motion_deg_day) < 1e-4);

    auto ceres = r.info("sb:1");
    REQUIRE(ceres.ok());
    const Info& c = ceres.value();
    CHECK(c.kind == Kind::Asteroid);
    CHECK(c.name == "Ceres");
    CHECK(c.number == 1);
    CHECK(c.designation == "1");
    CHECK(c.standing == Standing::Iau);
    CHECK(c.standing_source == "IAU WGSBN");
    CHECK(c.source == "sample-100.epm");
    CHECK(std::isfinite(c.h_mag));
    CHECK(std::isfinite(c.elements_epoch_jd));
    CHECK(std::fabs(c.sidereal_mean_motion_deg_day - 0.2141) < 0.0005);
    CHECK(c.ref.kind == Engine::ObjectRef::Kind::Body);
    CHECK(c.ref.id == 20000001);
    // An id in another spelling answers with the canonical one.
    CHECK(r.info("sb:ceres").value().id == "sb:1");

    auto cupido = r.info("hyp:cupido");
    REQUIRE(cupido.ok());
    CHECK(cupido.value().kind == Kind::Hypothetical);
    CHECK(cupido.value().standing == Standing::Defined);
    CHECK(!cupido.value().standing_source.empty());
    CHECK(cupido.value().mean_motion_deg_day > 0.0);
    CHECK(cupido.value().period_days > 200.0 * 365.25);

    auto regulus = r.info("star:HR 3982");
    REQUIRE(regulus.ok());
    CHECK(regulus.value().name == "Regulus");
    CHECK(regulus.value().standing == Standing::Iau);
    CHECK(std::isfinite(regulus.value().vmag));
    CHECK(std::isnan(regulus.value().mean_motion_deg_day));
    auto praesepe = r.info("star:M 44");
    REQUIRE(praesepe.ok());
    CHECK(praesepe.value().standing == Standing::Traditional);

    CHECK(r.info("naif:12345").error().code == ErrorCode::NotFound);
    CHECK(r.info("sb:Nosuchbody").error().code == ErrorCode::NotFound);
    CHECK(r.info("hyp:nosuch").error().code == ErrorCode::NotFound);
}

TEST_CASE("registry_refs_compute_what_they_name") {
    synth::TempFile tf("registry-refs");
    Engine e = open_with_sample(tf);
    Registry r(e);
    const double jd = 2451600.5;
    const auto same = [&](const std::string& id, const Result<CalcResult>& direct) {
        auto info = r.info(id);
        REQUIRE_MESSAGE(info.ok(), id);
        auto via = e.calc_object(info.value().ref, jd);
        REQUIRE_MESSAGE(via.ok(), id);
        REQUIRE(direct.ok());
        CHECK_MESSAGE(via.value().pos.lon_deg == direct.value().pos.lon_deg, id);
        CHECK_MESSAGE(via.value().pos.lat_deg == direct.value().pos.lat_deg, id);
    };
    same("naif:5", e.calc(5, jd));
    same("sb:1", e.calc(20000001, jd));
    same("orbit:naif:301:ascending-node:mean",
         e.calc_orbit_point(301, OrbitPoint::AscendingNode, OrbitElements::Mean, jd));
    same("hyp:cupido", e.calc_hypothetical("cupido", jd));
    same("star:HR 3982", e.calc_star(stars::find("Regulus").value(), jd));
}

TEST_CASE("registry_list") {
    synth::TempFile tf("registry-list");
    Engine e = open_with_sample(tf);
    Registry r(e);
    // Everything, page by page, equals everything at once, and holds each
    // object once.
    const auto all = [&](const Filter& f, size_t page_size) {
        std::vector<std::string> out;
        uint64_t cursor = 0;
        for (int guard = 0; guard < 100000; ++guard) {
            auto p = r.list(f, cursor, page_size);
            REQUIRE(p.ok());
            for (const Match& m : p.value().objects)
                out.push_back(m.id);
            if (p.value().next == 0)
                break;
            cursor = p.value().next;
        }
        return out;
    };
    const std::vector<std::string> one_go = all(Filter{}, 100000);
    CHECK(one_go == all(Filter{}, 7));
    CHECK(std::set<std::string>(one_go.begin(), one_go.end()).size() == one_go.size());
    CHECK(one_go.size() == 12 + 10 + e.hypothetical_tokens().size() + stars::count() + 100);
    CHECK(one_go.front() == "naif:10");
    CHECK(one_go.back() == "sb:100");

    Filter hyp;
    hyp.kinds = 1u << int(Kind::Hypothetical);
    CHECK(all(hyp, 3).size() == e.hypothetical_tokens().size());

    Filter bright;
    bright.vmag_max = 1.0; // the first-magnitude stars, and nothing without a V
    const auto b = all(bright, 1000);
    CHECK(b.size() >= 15);
    CHECK(b.size() <= 25);
    for (const std::string& id : b)
        CHECK(id.rfind("star:", 0) == 0);

    Filter mba;
    mba.orbit_class = "MBA";
    mba.named_only = true;
    for (const std::string& id : all(mba, 1000))
        CHECK(id.rfind("sb:", 0) == 0);
    Filter bad;
    bad.orbit_class = "XYZ";
    CHECK(r.list(bad, 0, 10).error().code == ErrorCode::ArgumentError);
}

namespace {

// Lookup's queries for the catalogs' own names: each name, an accented
// spelling of it, its two- and three-letter prefixes, and each designation
// and old designation as given and with its spaces taken out.
std::vector<std::pair<std::string, bool>> catalog_queries(const Engine& e) {
    std::set<std::pair<std::string, bool>> q;
    auto each = e.for_each_catalog_name([&](size_t, uint64_t, const catalog::Names& n) {
        for (std::string_view s : {n.pdes, n.name, n.prov_des}) {
            if (s.empty())
                continue;
            std::string spaced(s), unspaced;
            for (char c : spaced)
                if (c != ' ')
                    unspaced += c;
            q.emplace(spaced, false);
            q.emplace(unspaced, false);
        }
        if (n.name.empty())
            return;
        const std::string name(n.name);
        std::string accented;
        for (char c : name)
            accented += c == 'e' ? std::string("\xC3\xA9") : std::string(1, c); // e -> é
        q.emplace(accented, false);
        q.emplace(name.substr(0, 2), true);
        q.emplace(name.substr(0, 3), true);
    });
    REQUIRE(each.ok());
    return {q.begin(), q.end()};
}

std::string describe(const Result<std::vector<Match>>& m) {
    if (!m)
        return "error: " + m.error().message;
    std::string out;
    for (const Match& x : m.value())
        out += x.id + "/" + std::to_string(int(x.kind)) + "/" +
               std::string(quality_name(x.quality)) + "/" + x.name + "/" + x.matched + "; ";
    return out;
}

} // namespace

// The folded name index lives in the corpus (A-15's sharing, 2026-10-28):
// engines on one frozen corpus read the same index, built once, and answer
// lookup exactly as an engine of its own does, from threads at once.
TEST_CASE("registry_contexts_share_names") {
    synth::TempFile tf("registry-contexts");
    synth::write_linear_spk(tf.path);
    const std::string sample = std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/sample-100.epm";
    const std::string overlay = std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/covariance-7.epm";

    // The answers of an engine of its own, the catalogs added in turn: a
    // catalog added after a lookup is searched by the next one.
    auto own = Engine::open(tf.path.string());
    REQUIRE(own.ok());
    REQUIRE(own.value().add_catalog(sample).ok());
    Registry own_r(own.value());
    REQUIRE(own_r.lookup("Ceres").ok());
    REQUIRE(own.value().add_catalog(overlay).ok());
    const auto queries = catalog_queries(own.value());
    REQUIRE(queries.size() > 300);
    std::vector<std::string> want;
    size_t found = 0;
    for (const auto& [q, prefix] : queries) {
        auto m = own_r.lookup(q, prefix);
        found += m && !m.value().empty();
        want.push_back(describe(m));
    }
    CHECK(found == queries.size()); // every name, prefix and designation answers

    auto opened = Corpus::open(tf.path.string());
    REQUIRE(opened.ok());
    std::shared_ptr<Corpus> corpus = opened.value();
    REQUIRE(corpus->add_catalog(sample).ok());
    REQUIRE(corpus->add_catalog(overlay).ok());
    corpus->freeze();

    // Four contexts, each on its own thread, the first lookup of all four at
    // once (the index is built under the corpus's lock).
    std::vector<Engine> contexts;
    for (int t = 0; t < 4; ++t) {
        auto c = Engine::open_context(corpus);
        REQUIRE(c.ok());
        contexts.push_back(std::move(c).value());
    }
    std::vector<std::vector<std::string>> got(4);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            Registry r(contexts[size_t(t)]);
            for (size_t k = 0; k < queries.size(); ++k) {
                // Each thread asks in its own order.
                const auto& [q, prefix] = queries[(k * size_t(2 * t + 1)) % queries.size()];
                got[size_t(t)].push_back(describe(r.lookup(q, prefix)));
            }
        });
    for (auto& th : threads)
        th.join();
    for (int t = 0; t < 4; ++t) {
        int differ = 0;
        for (size_t k = 0; k < queries.size(); ++k) {
            const size_t i = (k * size_t(2 * t + 1)) % queries.size();
            differ += got[size_t(t)][k] != want[i];
        }
        CHECK_MESSAGE(differ == 0, "context " << t);
    }
    // One index for every context, not one each.
    for (size_t c = 0; c < 2; ++c) {
        auto a = contexts[0].catalog_folded_names(c), b = contexts[3].catalog_folded_names(c);
        REQUIRE(a.ok());
        REQUIRE(b.ok());
        CHECK(a.value() == b.value());
        CHECK(!a.value()->empty());
    }
    CHECK(!contexts[0].catalog_folded_names(2).ok());
}

namespace {

std::string full_catalog() {
    const char* v = std::getenv("PROMETHEIA_CATALOG");
    return v ? v : std::string(PROMETHEIA_SOURCE_DIR) + "/sbdb-20261027.epm";
}

} // namespace

TEST_CASE("registry_full_catalog_names" * doctest::skip()) {
    if (access(full_catalog().c_str(), R_OK) != 0) {
        MESSAGE("SKIP: no catalog at " << full_catalog());
        return;
    }
    synth::TempFile tf("registry-full");
    Engine e = open_synthetic(tf);
    REQUIRE(e.add_catalog(full_catalog()).ok());
    Registry r(e);
    CHECK(only(r, "Eris").id == "sb:136199");
    CHECK(only(r, "136199").id == "sb:136199");
    const Match sedna = only(r, "2003 VB12");
    CHECK(sedna.id == "sb:90377");
    CHECK(sedna.quality == MatchQuality::OldDesignation);
    CHECK(only(r, "2003VB12").id == "sb:90377");
    CHECK(only(r, "1994 TA").kind == Kind::Asteroid);
    CHECK(only(r, "Chiron").id == "sb:2060");
    CHECK(only(r, "Gǃkúnǁʼhòmdímà").id == "sb:229762");
    CHECK(only(r, "gkunhomdima").id == "sb:229762");
    CHECK(only(r, "523671").id == "sb:523671");
    CHECK(only(r, "2017 OF201").kind == Kind::Asteroid);
    CHECK(only(r, "2012 VP113").kind == Kind::Asteroid);
    auto sedna_info = r.info("sb:90377");
    REQUIRE(sedna_info.ok());
    CHECK(sedna_info.value().provisional_designation == "2003 VB12");
    CHECK(sedna_info.value().orbit_class == "TNO");
    CHECK(r.info("sb:2003 VB12").value().id == "sb:90377");
    // The named centaurs, by listing.
    Filter cen;
    cen.orbit_class = "CEN";
    cen.named_only = true;
    auto page = r.list(cen, 0, 1000);
    REQUIRE(page.ok());
    bool chiron = false;
    for (const Match& m : page.value().objects)
        chiron = chiron || m.id == "sb:2060";
    CHECK(chiron);
    MESSAGE(page.value().objects.size() << " named centaurs");
}

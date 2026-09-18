// SPDX-License-Identifier: GPL-2.0-or-later
//
// Bodies from polynomial orbital elements (Engine::calc_elements), end to end
// through the engine: frames, centres and corrections. The mean-anomaly rule
// itself is pinned in tests/test_elements.cpp.
//
// The oracles are independent of the code under test. A real planet's own
// osculating elements, taken from DE440 at an epoch, must give back that
// planet's DE440 position at the epoch; and away from it, the closed-form
// kepler_propagate oracle must agree. Every set here has one term, so the
// body moves at the Kepler mean motion from its epoch.
//
// PROMETHEIA_DE440 (default ephe/linux_p1550p2650.440); SKIP when absent.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

#include <prometheia/elements.hpp>
#include <prometheia/engine.hpp>
#include <prometheia/hypotheticals.hpp>
#include <prometheia/kepler.hpp>

#include <doctest/doctest.h>

using namespace prometheia;

namespace {

constexpr double kRad2Deg = 180.0 / 3.14159265358979323846;
constexpr double kMuSun = elements::kGaussK * elements::kGaussK;
constexpr double kMuEarth = kMuSun / elements::kSunEarthMassRatio;
constexpr double kJ2000 = 2451545.0;

std::string de440_path() {
    const char* v = std::getenv("PROMETHEIA_DE440");
    return v && *v ? v : std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_p1550p2650.440";
}

// A body's geometric rectangular state, AU and AU/day, in the given frame's
// ecliptic, from the given centre.
State state_of(Engine& e, int body, double jd, Frame frame, Center center) {
    CalcOptions o = CalcOptions::geometric();
    o.frame = frame;
    o.center = center;
    o.coords = Coords::Ecliptic;
    auto r = e.calc(body, jd, o);
    REQUIRE_MESSAGE(r.ok(), r.error().message);
    const Position& p = r.value().pos;
    return {{p.xyz_au[0], p.xyz_au[1], p.xyz_au[2]},
            {p.vel_au_day[0], p.vel_au_day[1], p.vel_au_day[2]}};
}

// One-term elements from an osculating state.
PolynomialElements elements_from(const State& s, double mu, double epoch, ElementEquinox eq,
                                 ElementOrigin origin) {
    auto k = state_to_elements(mu, s);
    REQUIRE(k.ok());
    const Elements& el = k.value();
    PolynomialElements p;
    p.epoch_jd_tt = epoch;
    p.equinox = eq;
    p.origin = origin;
    p.n_terms = 1;
    p.mean_anomaly[0] = el.mean_anom * kRad2Deg;
    p.semi_major_axis[0] = el.a;
    p.eccentricity[0] = el.e;
    p.arg_perihelion[0] = el.argp * kRad2Deg;
    p.ascending_node[0] = el.node * kRad2Deg;
    p.inclination[0] = el.inc * kRad2Deg;
    return p;
}

Position elements_at(Engine& e, const PolynomialElements& el, double jd, CalcOptions o) {
    auto r = e.calc_elements(el, jd, o);
    REQUIRE_MESSAGE(r.ok(), r.error().message);
    return r.value().pos;
}

double distance_au(const double a[3], const Vec3& b) {
    return std::sqrt((a[0] - b.x) * (a[0] - b.x) + (a[1] - b.y) * (a[1] - b.y) +
                     (a[2] - b.z) * (a[2] - b.z));
}

// Angle between two directions, arcsec. atan2 of the cross and dot products:
// acos of the dot rounds a milliarcsecond to exactly zero.
double sep_arcsec(const double a[3], const double b[3]) {
    const double c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                         a[0] * b[1] - a[1] * b[0]};
    const double cross = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return std::atan2(cross, dot) * kRad2Deg * 3600.0;
}

} // namespace

TEST_CASE("elements_reproduce_a_planet_at_its_epoch_and_kepler_away_from_it") {
    const std::string de = de440_path();
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    auto opened = Engine::open(de);
    REQUIRE(opened.ok());
    Engine& e = opened.value();

    // Jupiter's heliocentric state in the mean ecliptic of J2000, at an epoch
    // chosen off any round number.
    const double epoch = kJ2000 + 123.4;
    const State s = state_of(e, 5, epoch, Frame::J2000, Center::Heliocentric);
    const PolynomialElements el =
        elements_from(s, kMuSun, epoch, ElementEquinox::J2000, ElementOrigin::Sun);

    CalcOptions o = CalcOptions::geometric();
    o.frame = Frame::J2000;
    o.center = Center::Heliocentric;
    o.coords = Coords::Ecliptic;

    // At the epoch the elements are the planet: back to DE440 to roundoff.
    CHECK(distance_au(elements_at(e, el, epoch, o).xyz_au, s.pos) < 1e-11);

    // Away from it they are a two-body orbit, and the closed-form propagator
    // is the oracle. 200 days and 30 years both ways.
    for (double dt : {200.0, -200.0, 30 * 365.25, -30 * 365.25}) {
        auto oracle = kepler_propagate(kMuSun, s, epoch, epoch + dt);
        REQUIRE(oracle.ok());
        CHECK_MESSAGE(distance_au(elements_at(e, el, epoch + dt, o).xyz_au, oracle.value().pos) <
                          1e-10,
                      "dt=" << dt);
    }
}

TEST_CASE("elements_refer_to_the_equinox_they_name") {
    const std::string de = de440_path();
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    auto opened = Engine::open(de);
    REQUIRE(opened.ok());
    Engine& e = opened.value();

    // For each equinox, take Saturn's state in that equinox's mean ecliptic
    // -- the engine's mean-of-date output AT that equinox's epoch -- and make
    // it the elements' epoch too. The elements must then place Saturn where
    // DE440 does, in the ICRF: the element frame's rotation checked against
    // the output side's, from the opposite direction.
    struct Case {
        const char* name;
        ElementEquinox eq;
        double epoch;
    };
    const Case cases[] = {
        {"J1900", ElementEquinox::J1900, 2415020.0},
        {"B1950", ElementEquinox::B1950, 2433282.42345905},
        {"of date", ElementEquinox::OfDate, kJ2000 + 9876.5},
        {"explicit", ElementEquinox::Explicit, 2460000.25},
    };
    CalcOptions icrf = CalcOptions::geometric();
    icrf.frame = Frame::ICRF;
    icrf.center = Center::Heliocentric;
    icrf.coords = Coords::Equatorial;
    for (const Case& c : cases) {
        const State s = state_of(e, 6, c.epoch, Frame::MeanOfDate, Center::Heliocentric);
        PolynomialElements el = elements_from(s, kMuSun, c.epoch, c.eq, ElementOrigin::Sun);
        if (c.eq == ElementEquinox::Explicit)
            el.equinox_jd_tt = c.epoch;
        auto truth = e.calc(6, c.epoch, icrf);
        REQUIRE(truth.ok());
        const Position got = elements_at(e, el, c.epoch, icrf);
        const double* t = truth.value().pos.xyz_au;
        CHECK_MESSAGE(distance_au(got.xyz_au, Vec3{t[0], t[1], t[2]}) < 1e-11, c.name);
    }
}

TEST_CASE("earth_centred_elements_reproduce_the_moon_and_move_slower") {
    const std::string de = de440_path();
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    auto opened = Engine::open(de);
    REQUIRE(opened.ok());
    Engine& e = opened.value();

    // The Moon's geocentric osculating orbit about the Earth, with the
    // Earth's mu -- the Gaussian constant divided by the mass ratio.
    const double epoch = kJ2000 + 42.0;
    const State s = state_of(e, body::kMoon, epoch, Frame::J2000, Center::Geocentric);
    const PolynomialElements el =
        elements_from(s, kMuEarth, epoch, ElementEquinox::J2000, ElementOrigin::Earth);

    CalcOptions o = CalcOptions::geometric();
    o.frame = Frame::J2000;
    o.center = Center::Geocentric;
    o.coords = Coords::Ecliptic;
    CHECK(distance_au(elements_at(e, el, epoch, o).xyz_au, s.pos) < 1e-13);
    for (double dt : {3.0, -3.0, 40.0}) {
        auto oracle = kepler_propagate(kMuEarth, s, epoch, epoch + dt);
        REQUIRE(oracle.ok());
        CHECK_MESSAGE(distance_au(elements_at(e, el, epoch + dt, o).xyz_au, oracle.value().pos) <
                          1e-12,
                      "dt=" << dt);
    }
}

TEST_CASE("elements_rates_and_corrections") {
    const std::string de = de440_path();
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    auto opened = Engine::open(de);
    REQUIRE(opened.ok());
    Engine& e = opened.value();

    // A circular orbit in the ecliptic: longitude advances at exactly n.
    PolynomialElements circle;
    circle.semi_major_axis[0] = 4.0;
    const double n_deg = elements::kGaussK / 8.0 * kRad2Deg; // a^1.5 = 8
    CalcOptions helio = CalcOptions::geometric();
    helio.frame = Frame::J2000;
    helio.center = Center::Heliocentric;
    const Position p = elements_at(e, circle, kJ2000 + 100.0, helio);
    CHECK(p.lon_deg == doctest::Approx(100.0 * n_deg).epsilon(1e-12));
    CHECK(p.lat_deg == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(p.lon_speed == doctest::Approx(n_deg).epsilon(1e-9));

    // A distant, slow point like the Hamburg School's: the light time solved
    // is the distance the answer sits at, over c.
    PolynomialElements far;
    far.semi_major_axis[0] = 41.0;
    far.inclination[0] = 1.0;
    far.ascending_node[0] = 100.0;
    CalcOptions lt = CalcOptions::geometric();
    lt.light_time = true;
    auto r = e.calc_elements(far, kJ2000 + 5000.0, lt);
    REQUIRE(r.ok());
    constexpr double kCAuPerDay = 299792.458 * 86400.0 / 149597870.7;
    CHECK(r.value().provenance.light_time_days ==
          doctest::Approx(r.value().pos.dist_au / kCAuPerDay).epsilon(1e-12));

    // An observer at the Sun's centre is never deflected, and aberration is
    // the large term for a point this far out: the full constant times the
    // sine of its angle from the Earth's apex, so under ~20.5 arcsec.
    CalcOptions geo = CalcOptions::geometric();
    CalcOptions aber = geo;
    aber.aberration = true;
    const Position g = elements_at(e, far, kJ2000 + 5000.0, geo);
    const Position a = elements_at(e, far, kJ2000 + 5000.0, aber);
    const double moved = sep_arcsec(g.xyz_au, a.xyz_au);
    CHECK(moved > 1.0);
    CHECK(moved < 20.6);
    CalcOptions sun = CalcOptions::geometric();
    sun.center = Center::Heliocentric;
    CalcOptions sun_defl = sun;
    sun_defl.deflection = true;
    CHECK(sep_arcsec(elements_at(e, far, kJ2000 + 5000.0, sun).xyz_au,
                     elements_at(e, far, kJ2000 + 5000.0, sun_defl).xyz_au) == 0.0);
}

TEST_CASE("elements_that_are_not_a_bound_orbit_are_refused") {
    const std::string de = de440_path();
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    auto opened = Engine::open(de);
    REQUIRE(opened.ok());
    Engine& e = opened.value();

    PolynomialElements ok;
    ok.semi_major_axis[0] = 10.0;
    REQUIRE(e.calc_elements(ok, kJ2000).ok());

    PolynomialElements hyperbolic = ok;
    hyperbolic.eccentricity[0] = 1.2;
    CHECK(!e.calc_elements(hyperbolic, kJ2000).ok());
    PolynomialElements no_axis = ok;
    no_axis.semi_major_axis[0] = -1.0;
    CHECK(!e.calc_elements(no_axis, kJ2000).ok());
    PolynomialElements none = ok;
    none.n_terms = 0;
    CHECK(!e.calc_elements(none, kJ2000).ok());
    PolynomialElements six = ok;
    six.n_terms = 6;
    CHECK(!e.calc_elements(six, kJ2000).ok());
}

// ---- element files ----------------------------------------------------------

namespace {

// An invented body: every number here is made up for the test.
constexpr const char* kInvented =
    R"({"token":"testbody","name":"Test Body","set":"Invented for tests","citation":"tests/test_hypotheticals.cpp","epoch":2415020.0,"equinox":"J1900","origin":"sun","M":[10.0],"a":[40.0],"e":[0.01],"w":[20.0],"node":[30.0],"i":[1.5]})";

std::string why_rejected(const std::string& line) {
    auto r = hypotheticals::parse(line, "t.jsonl");
    return r ? std::string() : r.error().message;
}

} // namespace

TEST_CASE("element_files_read_strictly") {
    auto ok = hypotheticals::parse(std::string(kInvented) + "\n\n  \n" + kInvented + "\r\n", "t");
    REQUIRE_MESSAGE(ok.ok(), ok.error().message);
    REQUIRE(ok.value().size() == 2);
    const hypotheticals::Body& b = ok.value()[0];
    CHECK(b.token == "testbody");
    CHECK(b.name == "Test Body");
    CHECK(b.set == "Invented for tests");
    CHECK(b.elements.equinox == ElementEquinox::J1900);
    CHECK(b.elements.n_terms == 1);
    CHECK(b.elements.semi_major_axis[0] == 40.0);

    // Lists of different lengths: the longest sets the term count, the rest
    // are zero-padded.
    std::string drift = kInvented;
    drift.replace(drift.find("\"node\":[30.0]"), 13, "\"node\":[30.0,0.5,-0.01]");
    auto d = hypotheticals::parse(drift, "t");
    REQUIRE(d.ok());
    CHECK(d.value()[0].elements.n_terms == 3);
    CHECK(d.value()[0].elements.mean_anomaly[1] == 0.0);
    CHECK(d.value()[0].elements.ascending_node[2] == -0.01);

    // An explicit equinox, and the Earth as centre.
    std::string explicit_eq = kInvented;
    explicit_eq.replace(explicit_eq.find("\"J1900\""), 7, "2440000.5");
    explicit_eq.replace(explicit_eq.find("\"sun\""), 5, "\"earth\"");
    auto x = hypotheticals::parse(explicit_eq, "t");
    REQUIRE(x.ok());
    CHECK(x.value()[0].elements.equinox == ElementEquinox::Explicit);
    CHECK(x.value()[0].elements.equinox_jd_tt == 2440000.5);
    CHECK(x.value()[0].elements.origin == ElementOrigin::Earth);

    // Refusals name the line and the reason. A mistyped field is an error,
    // not a silently zero element.
    const auto swap = [](std::string s, const char* from, const char* to) {
        s.replace(s.find(from), std::string(from).size(), to);
        return s;
    };
    std::string s = std::string(kInvented) + "\n" + swap(kInvented, "\"node\"", "\"nodes\"");
    CHECK(why_rejected(s).find("t.jsonl:2: unknown field \"nodes\"") != std::string::npos);
    CHECK(why_rejected(swap(kInvented, ",\"i\":[1.5]", "")).find("all six") != std::string::npos);
    CHECK(why_rejected(swap(kInvented, "\"testbody\"", "\"Test Body\"")).find("lowercase") !=
          std::string::npos);
    CHECK(why_rejected(swap(kInvented, "\"e\":[0.01]", "\"e\":[1.0]")).find("[0, 1)") !=
          std::string::npos);
    CHECK(why_rejected(swap(kInvented, "\"a\":[40.0]", "\"a\":[1,2,3,4,5,6]")).find("1 to 5") !=
          std::string::npos);
    CHECK(why_rejected(swap(kInvented, "\"J1900\"", "\"B1875\"")).find("equinox") !=
          std::string::npos);
    CHECK(why_rejected(swap(kInvented, "\"citation\":\"tests/test_hypotheticals.cpp\",", ""))
              .find("citation") != std::string::npos);
    CHECK(why_rejected(swap(kInvented, "\"set\"", "\"token\"")).find("appears twice") !=
          std::string::npos);
    CHECK(!why_rejected(std::string(kInvented) + " {}").empty());   // two values on a line
    CHECK(!why_rejected(swap(kInvented, "10.0", "1e999")).empty()); // not finite
    CHECK(!why_rejected(swap(kInvented, "10.0", "NaN")).empty());   // not JSON
    CHECK(!why_rejected(swap(kInvented, "10.0", "0x10")).empty());  // not JSON
    CHECK(!why_rejected(swap(kInvented, "10.0", "+10.0")).empty()); // not JSON
    CHECK(!why_rejected(std::string(40, '[') + std::string(40, ']')).empty()); // too deep
    CHECK(!why_rejected("[1,2,3]").empty());                                   // not an object
}

TEST_CASE("the_shipped_element_set_parses") {
    // The engine refuses to open if its compiled-in set does not parse, so
    // this is checked here directly as well, without needing an ephemeris.
    const std::string path = std::string(PROMETHEIA_SOURCE_DIR) + "/data/hypotheticals.jsonl";
    FILE* f = std::fopen(path.c_str(), "rb");
    REQUIRE(f);
    std::string text;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
        text.append(buf, n);
    std::fclose(f);
    auto r = hypotheticals::parse(text, "data/hypotheticals.jsonl");
    CHECK_MESSAGE(r.ok(), r.error().message);
}

TEST_CASE("named_hypotheticals_compute_as_their_elements_and_later_files_win") {
    const std::string de = de440_path();
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    auto opened = Engine::open(de);
    REQUIRE(opened.ok());
    Engine& e = opened.value();

    // A per-process file in the system temp directory, removed on the way out.
    struct Scratch {
        std::string path = (std::filesystem::temp_directory_path() /
                            ("prometheia-hyp-" + std::to_string(::getpid()) + ".jsonl"))
                               .string();
        ~Scratch() { std::remove(path.c_str()); }
    } scratch;
    const std::string& path = scratch.path;
    const auto write = [&](const std::string& text) {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f);
        std::fputs(text.c_str(), f);
        std::fclose(f);
    };
    write(std::string(kInvented) + "\n");
    REQUIRE(e.hypothetical("testbody") == nullptr);
    auto missing = e.calc_hypothetical("testbody", kJ2000);
    CHECK(!missing.ok());
    CHECK(missing.error().code == ErrorCode::NotFound);

    auto added = e.add_hypotheticals(path);
    REQUIRE_MESSAGE(added.ok(), added.error().message);
    const auto tokens = e.hypothetical_tokens();
    CHECK(std::find(tokens.begin(), tokens.end(), "testbody") != tokens.end());

    // By name it is exactly its elements, and the answer names the set.
    const hypotheticals::Body* b = e.hypothetical("TestBody"); // case-insensitive
    REQUIRE(b);
    auto named = e.calc_hypothetical("testbody", kJ2000 + 777.0);
    auto direct = e.calc_elements(b->elements, kJ2000 + 777.0);
    REQUIRE(named.ok());
    REQUIRE(direct.ok());
    CHECK(named.value().pos.lon_deg == direct.value().pos.lon_deg);
    CHECK(named.value().pos.lat_deg == direct.value().pos.lat_deg);
    CHECK(named.value().provenance.source == "Invented for tests");

    // A later file redefines the token and wins; the token keeps its place.
    std::string moved = kInvented;
    moved.replace(moved.find("\"M\":[10.0]"), 10, "\"M\":[70.0]");
    moved.replace(moved.find("Invented for tests"), 18, "Revised for tests!");
    write(moved + "\n");
    REQUIRE(e.add_hypotheticals(path).ok());
    auto revised = e.calc_hypothetical("testbody", kJ2000 + 777.0);
    REQUIRE(revised.ok());
    CHECK(revised.value().provenance.source == "Revised for tests!");
    CHECK(std::fabs(revised.value().pos.lon_deg - named.value().pos.lon_deg) > 1.0);
    CHECK(e.hypothetical_tokens().size() == tokens.size());

    // A bad file is refused whole, and the engine keeps what it had.
    write(std::string(kInvented) + "\n{\"token\":\"broken\"}\n");
    CHECK(!e.add_hypotheticals(path).ok());
    CHECK(e.hypothetical("broken") == nullptr);
    CHECK(e.calc_hypothetical("testbody", kJ2000).ok());
}

TEST_CASE("an_equinox_date_only_with_an_explicit_equinox") {
    const std::string de = de440_path();
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    auto opened = Engine::open(de);
    REQUIRE(opened.ok());
    Engine& e = opened.value();
    PolynomialElements el;
    el.semi_major_axis[0] = 10.0;
    REQUIRE(e.calc_elements(el, kJ2000).ok());
    el.equinox_jd_tt = 2440000.5; // a date, but the equinox is J2000
    CHECK(!e.calc_elements(el, kJ2000).ok());
    el.equinox = ElementEquinox::Explicit;
    CHECK(e.calc_elements(el, kJ2000).ok());
    el.equinox_jd_tt = 0.0; // explicit, but no date
    CHECK(!e.calc_elements(el, kJ2000).ok());
}

TEST_CASE("shipped_bodies_reproduce_their_sources_own_check_positions") {
    // Where a source prints a position it derived from its own elements, the
    // shipped transcription has to give it back: a misread digit would miss
    // by degrees. Heliocentric, geometric, the mean ecliptic of the epoch.
    const std::string de = de440_path();
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    auto opened = Engine::open(de);
    REQUIRE(opened.ok());
    Engine& e = opened.value();
    CalcOptions o = CalcOptions::geometric();
    o.center = Center::Heliocentric;
    o.frame = Frame::MeanOfDate;

    // Le Verrier, Comptes rendus 23 (1846) p. 432: at 1er janvier 1847, true
    // heliocentric longitude 326 deg 32', distance 33,06. The transcription
    // gives 326 deg 31.3' and 33.080: agreement within 0.7' and 0.02 AU, a
    // residual a little larger than the printed elements' rounding explains,
    // recorded here rather than tuned away (docs/HYPOTHETICALS.md).
    auto lv = e.calc_hypothetical("neptune-leverrier", 2395662.5, o);
    REQUIRE_MESSAGE(lv.ok(), lv.error().message);
    CHECK(std::fabs(lv.value().pos.lon_deg - (326.0 + 32.0 / 60.0)) < 1.0 / 60.0);
    CHECK(std::fabs(lv.value().pos.dist_au - 33.06) < 0.03);
    CHECK(lv.value().provenance.source == "Le Verrier 1846");
}

TEST_CASE("the_shipped_hamburg_points_match_swetest") {
    // The eight Hamburg School points ship with the elements Astrolog
    // distributes (docs/DESIGN.md, "Exposures": a recorded exception, taken
    // by a separate session). The oracle is swetest's OUTPUT on the same
    // elements -- heliocentric, true positions, J2000 (-hel -true -j2000),
    // TT JD 2451545.0, JPL DE440 -- as for the other swetest fixtures. It
    // prints 1e-7 deg; the 24 positions checked at 1900, 2000 and 2100 agreed
    // to 0.00076" and 5e-10 AU, so 0.002" and 2e-9 AU are the bounds.
    const std::string de = de440_path();
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    auto opened = Engine::open(de);
    REQUIRE(opened.ok());
    Engine& e = opened.value();
    struct Want {
        const char* token;
        double lon, lat, dist;
    };
    const Want want[] = {
        {"cupido", 243.0869163, 0.9933286, 40.902232863},
        {"hades", 78.5992663, -1.0315179, 50.743141041},
        {"zeus", 184.4449817, -0.0021180, 59.245435610},
        {"kronos", 87.9863168, 0.0130480, 64.922077248},
        {"apollon", 200.5252405, -0.0056058, 70.299490000},
        {"admetos", 49.7129506, 0.0106488, 73.627650000},
        {"vulcanus", 110.3098697, 0.0118215, 77.255680000},
        {"poseidon", 213.9506677, -0.0081925, 83.669070000},
    };
    CalcOptions o = CalcOptions::geometric();
    o.center = Center::Heliocentric;
    o.frame = Frame::J2000;
    for (const Want& w : want) {
        auto r = e.calc_hypothetical(w.token, kJ2000, o);
        REQUIRE_MESSAGE(r.ok(), w.token);
        const Position& p = r.value().pos;
        const double a[3] = {std::cos(w.lat / kRad2Deg) * std::cos(w.lon / kRad2Deg),
                             std::cos(w.lat / kRad2Deg) * std::sin(w.lon / kRad2Deg),
                             std::sin(w.lat / kRad2Deg)};
        const double b[3] = {std::cos(p.lat_deg / kRad2Deg) * std::cos(p.lon_deg / kRad2Deg),
                             std::cos(p.lat_deg / kRad2Deg) * std::sin(p.lon_deg / kRad2Deg),
                             std::sin(p.lat_deg / kRad2Deg)};
        CHECK_MESSAGE(sep_arcsec(a, b) < 0.002, w.token);
        CHECK_MESSAGE(std::fabs(p.dist_au - w.dist) < 2e-9, w.token);
        CHECK(r.value().provenance.source == "Hamburg School (Swiss Ephemeris seorbel.txt)");
    }
}

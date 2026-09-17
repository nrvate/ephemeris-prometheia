// SPDX-License-Identifier: GPL-2.0-or-later
//
// JPL Horizons as the independent referee (M5). The corpus in
// tests/horizons_corpus.inc comes from tools/fetch/horizons_fetch.py via
// tools/gen/gen_horizons_corpus.py; the comparisons, their models and the
// measured numbers are written up in docs/VALIDATION.md.
//
// Horizons computes from DE441 and integrates small bodies with 16 asteroid
// perturbers and relativity; we run DE440 (sub-mas from DE441 over these
// epochs). Gates sit a little above the measured residuals; the rows where
// the two systems use different published models are reported, not gated:
// apparent/ecliptic of date outside Horizons' EOP era (it uses IAU 1976/80
// precession-nutation, we use IAU 2006/2000A), the Sun-centred apparent
// quantities (Horizons refers them to the Sun's equator), and small bodies
// beyond 10 years from their element epoch (our force model omits the
// asteroid perturbers and relativity).
//
// Kept fast for the pre-commit gate: small bodies are gated within 10 years
// of their element epoch with sigma off. The long arcs and the uncertainty
// comparison are a report case skipped by default; run it with
//   build/test_horizons -tc=horizons_small_bodies_long_arc_report --no-skip
//
// Needs the DE440 binary (PROMETHEIA_DE440, default ephe/); SKIP otherwise.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <unistd.h>

#include <doctest/doctest.h>
#include <prometheia/engine.hpp>
#include <prometheia/frames.hpp>
#include <prometheia/time.hpp>

using namespace prometheia;

namespace {

constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kArcsec = 3600.0;
constexpr double kAuM = 149597870700.0;
constexpr int kCenterGeo = 0, kCenterHelio = 1, kCenterTopo = 2;
constexpr double kSmallEpoch = 2461200.5; // sample-100.epm element epoch (TDB)

// Horizons' apparent RA is referred to the EOP-corrected IAU 1976/80
// equinox, which it documents as offset -53 mas from the IAU 2006/2000A
// equinox we use; measured over the EOP-era rows: +51.8 mas (ours - theirs).
constexpr double kEquinoxOffsetArcsec = 0.0518;
// Horizons' ecliptic of date uses the IAU 1980 obliquity (84381.448" at
// J2000, 42 mas above IAU 2006), and the same equinox offset.
constexpr double kEclipticLonOffsetArcsec = 0.048;

struct HorizonsObs {
    const char* request;
    int body;
    int center; // kCenterGeo, kCenterHelio, kCenterTopo
    double site_lon_deg, site_lat_deg, site_h_km;
    double jd_tt;
    double ra_icrf, dec_icrf;
    double ra_app, dec_app;
    double delta_au;
    double ecl_lon, ecl_lat;
    double tdb_minus_ut;
    double last_hours;
    double ra_3s, dec_3s, pos_3s;
};

struct HorizonsVec {
    const char* request;
    int body;
    double jd_tdb;
    double x, y, z, vx, vy, vz;
};

#include "horizons_corpus.inc"

std::string env_or(const char* var, const std::string& fallback) {
    const char* env = std::getenv(var);
    return (env && *env) ? std::string(env) : fallback;
}

// The shared engine (DE440 + the in-tree sample catalog + the covariance
// overlay for the corpus bodies and Rumina), or nullptr when the DE440
// binary is absent.
Engine* engine() {
    static std::unique_ptr<Engine> e;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const std::string de = env_or("PROMETHEIA_DE440", std::string(PROMETHEIA_SOURCE_DIR) +
                                                              "/ephe/linux_p1550p2650.440");
        if (access(de.c_str(), F_OK) != 0) {
            std::printf("  SKIP: %s not present\n", de.c_str());
            return nullptr;
        }
        auto opened = Engine::open(de);
        if (!opened.ok())
            return nullptr;
        e = std::make_unique<Engine>(std::move(opened).value());
        const std::string data = std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/";
        if (!e->add_catalog(data + "sample-100.epm").ok() ||
            !e->add_catalog(data + "covariance-7.epm").ok())
            e.reset();
    }
    return e.get();
}

// The same engine plus JPL's SB441-N16 asteroid perturbers
// (PROMETHEIA_SB441, default ephe/sb441-n16.bsp; 616 MB, not in the tree),
// or nullptr when the kernel or DE440 is absent.
Engine* engine_sb441() {
    static std::unique_ptr<Engine> e;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const std::string kernel =
            env_or("PROMETHEIA_SB441", std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/sb441-n16.bsp");
        if (access(kernel.c_str(), F_OK) != 0 || !engine()) {
            std::printf("  SKIP: %s not present\n", kernel.c_str());
            return nullptr;
        }
        const std::string de = env_or("PROMETHEIA_DE440", std::string(PROMETHEIA_SOURCE_DIR) +
                                                              "/ephe/linux_p1550p2650.440");
        auto opened = Engine::open(de);
        if (!opened.ok())
            return nullptr;
        e = std::make_unique<Engine>(std::move(opened).value());
        const std::string data = std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/";
        if (!e->add_catalog(data + "sample-100.epm").ok() ||
            !e->add_catalog(data + "covariance-7.epm").ok() || !e->add_perturbers(kernel).ok())
            e.reset();
    }
    return e.get();
}

double separation_arcsec(double lon1, double lat1, double lon2, double lat2) {
    const double a1 = lon1 * kDeg, b1 = lat1 * kDeg, a2 = lon2 * kDeg, b2 = lat2 * kDeg;
    const double u[3] = {std::cos(b1) * std::cos(a1), std::cos(b1) * std::sin(a1), std::sin(b1)};
    const double v[3] = {std::cos(b2) * std::cos(a2), std::cos(b2) * std::sin(a2), std::sin(b2)};
    const double c[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                         u[0] * v[1] - u[1] * v[0]};
    const double s = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    const double d = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
    return std::atan2(s, d) / kDeg * kArcsec;
}

double wrap180(double deg) {
    deg = std::fmod(deg + 180.0, 360.0);
    return deg < 0 ? deg + 180.0 : deg - 180.0;
}

bool in_eop_era(double jd_tt) {
    return jd_tt >= 2437684.5 && jd_tt <= 2461300.5; // 1962-01-20 .. 2026-09-17
}

bool is_small(const HorizonsObs& h) {
    return h.body > 1000000;
}

struct FixedDeltaT final : time::DeltaTModel {
    double seconds = 0.0;
    double delta_t_seconds(double) const override { return seconds; }
};

// TT - UT1 (s) for a topocentric row. Horizons' TDB-UT column is TDB-UTC
// after 1962, so UT1 is solved from its local apparent sidereal time with
// our GAST (the equinox offset is worth 3.5 ms: 1.6 m of site rotation).
double delta_t_from_sidereal_time(const HorizonsObs& h) {
    double jd_ut1 = h.jd_tt - (h.tdb_minus_ut - time::tdb_minus_tt(h.jd_tt)) / 86400.0;
    const double target = h.last_hours / 24.0 * 2.0 * kPi;
    for (int k = 0; k < 4; ++k) {
        const double ours = frames::gast_rad(jd_ut1, h.jd_tt) + h.site_lon_deg * kDeg;
        jd_ut1 += std::remainder(target - ours, 2.0 * kPi) / (2.0 * kPi * 1.00273781191135448);
    }
    return (h.jd_tt - jd_ut1) * 86400.0;
}

CalcOptions options_for(const HorizonsObs& h, bool sigma = false) {
    CalcOptions o;
    o.sigma = sigma;
    o.center = h.center == kCenterGeo
                   ? Center::Geocentric
                   : (h.center == kCenterHelio ? Center::Heliocentric : Center::Topocentric);
    o.site = {h.site_lon_deg * kDeg, h.site_lat_deg * kDeg, h.site_h_km * 1000.0};
    o.speed = false;
    return o;
}

// Our answers for one Horizons row, computed with Horizons' Earth rotation.
struct Ours {
    CalcResult astrometric, apparent, ecliptic;
};

Ours compute(Engine& e, const HorizonsObs& h, bool sigma = false) {
    FixedDeltaT fixed;
    if (h.center == kCenterTopo) {
        fixed.seconds = delta_t_from_sidereal_time(h);
        e.set_delta_t_model(&fixed);
    }
    CalcOptions ast = options_for(h, sigma);
    ast.deflection = ast.aberration = false;
    ast.frame = Frame::ICRF;
    ast.coords = Coords::Equatorial;
    CalcOptions app = options_for(h, sigma);
    app.coords = Coords::Equatorial;
    const CalcOptions ecl = options_for(h, sigma);
    auto a = e.calc(h.body, h.jd_tt, ast);
    auto p = e.calc(h.body, h.jd_tt, app);
    auto c = e.calc(h.body, h.jd_tt, ecl);
    e.set_delta_t_model(nullptr);
    REQUIRE(a.ok());
    REQUIRE(p.ok());
    REQUIRE(c.ok());
    return {a.value(), p.value(), c.value()};
}

struct Worst {
    double v = 0.0;
    std::string where;
    void add(double x, const HorizonsObs& h) {
        if (std::fabs(x) > std::fabs(v)) {
            v = x;
            where = std::string(h.request) + " JD " + std::to_string(h.jd_tt);
        }
    }
    void print(const char* what, const char* unit) const {
        std::printf("  %-44s %12.6f %s  (%s)\n", what, v, unit, where.c_str());
    }
};

TEST_CASE("horizons_astrometric_and_range") {
    Engine* e = engine();
    if (!e)
        return;
    Worst planet, moon, planet_topo, moon_topo, range, range_topo, range_moon;
    for (const HorizonsObs& h : kHorizonsObs) {
        if (is_small(h))
            continue;
        const Ours o = compute(*e, h);
        const double sep = separation_arcsec(o.astrometric.pos.lon_deg, o.astrometric.pos.lat_deg,
                                             h.ra_icrf, h.dec_icrf);
        const double dr = (o.astrometric.pos.dist_au - h.delta_au) * kAuM;
        const bool moon_row = h.body == body::kMoon;
        const bool topo = h.center == kCenterTopo;
        (moon_row ? (topo ? moon_topo : moon) : (topo ? planet_topo : planet)).add(sep, h);
        (moon_row ? range_moon : (topo ? range_topo : range)).add(dr, h);
        // Light-time astrometric place: geometry, light time and frame only.
        CHECK(sep < (moon_row ? (topo ? 0.015 : 0.03) : 1e-4));
        CHECK(std::fabs(dr) < (topo || moon_row ? 10.0 : 3.0));
    }
    planet.print("Sun/planets astrometric, geo + helio", "\"");
    planet_topo.print("Sun/planets astrometric, topocentric", "\"");
    moon.print("Moon astrometric, geocentric", "\"");
    moon_topo.print("Moon astrometric, topocentric", "\"");
    range.print("Sun/planets light-time range, geo + helio", "m");
    range_topo.print("Sun/planets light-time range, topocentric", "m");
    range_moon.print("Moon light-time range", "m");
}

TEST_CASE("horizons_apparent_of_date") {
    Engine* e = engine();
    if (!e)
        return;
    Worst dra, ddec, dra_moon, ddec_moon, dlon, dlat, out_sep;
    for (const HorizonsObs& h : kHorizonsObs) {
        // Sun-centred apparent quantities are in the Sun's equatorial frame.
        if (is_small(h) || h.center == kCenterHelio)
            continue;
        const Ours o = compute(*e, h);
        const double ra = wrap180(o.apparent.pos.lon_deg - h.ra_app) * kArcsec;
        const double dec = (o.apparent.pos.lat_deg - h.dec_app) * kArcsec;
        const double lon =
            wrap180(o.ecliptic.pos.lon_deg - h.ecl_lon) * kArcsec * std::cos(h.ecl_lat * kDeg);
        const double lat = (o.ecliptic.pos.lat_deg - h.ecl_lat) * kArcsec;
        if (!in_eop_era(h.jd_tt)) {
            // IAU 1976/80 without EOP corrections vs IAU 2006/2000A: reported.
            out_sep.add(separation_arcsec(o.apparent.pos.lon_deg, o.apparent.pos.lat_deg, h.ra_app,
                                          h.dec_app),
                        h);
            continue;
        }
        const bool moon_row = h.body == body::kMoon;
        (moon_row ? dra_moon : dra).add(ra - kEquinoxOffsetArcsec, h);
        (moon_row ? ddec_moon : ddec).add(dec, h);
        dlon.add(lon - kEclipticLonOffsetArcsec, h);
        dlat.add(lat, h);
        CHECK(std::fabs(ra - kEquinoxOffsetArcsec) < (moon_row ? 0.006 : 0.003));
        CHECK(std::fabs(dec) < (moon_row ? 0.010 : 0.002));
        CHECK(std::fabs(lon - kEclipticLonOffsetArcsec) < 0.012);
        CHECK(std::fabs(lat) < 0.05); // the 42 mas obliquity difference
    }
    dra.print("apparent dRA - equinox offset, EOP era", "\"");
    ddec.print("apparent dDec, EOP era", "\"");
    dra_moon.print("Moon apparent dRA - equinox offset, EOP era", "\"");
    ddec_moon.print("Moon apparent dDec, EOP era", "\"");
    dlon.print("ecliptic dLon*cos(b) - offset, EOP era", "\"");
    dlat.print("ecliptic dLat (obliquity models), EOP era", "\"");
    out_sep.print("apparent separation outside EOP era (report)", "\"");
}

double small_body_offset_arcsec(Engine& e, const HorizonsObs& h, double* ours3) {
    const Ours o = compute(e, h, ours3 != nullptr);
    if (ours3)
        *ours3 = o.apparent.sigma_arcsec ? 3.0 * *o.apparent.sigma_arcsec : kNa;
    return separation_arcsec(o.astrometric.pos.lon_deg, o.astrometric.pos.lat_deg, h.ra_icrf,
                             h.dec_icrf);
}

double heliocentric_km(Engine& e, const HorizonsVec& v) {
    CalcOptions g = CalcOptions::geometric();
    g.center = Center::Heliocentric;
    g.frame = Frame::ICRF;
    g.coords = Coords::Equatorial;
    g.speed = false;
    g.sigma = false;
    auto r = e.calc(v.body, time::tt_from_tdb(v.jd_tdb), g);
    REQUIRE(r.ok());
    const double dx = r.value().pos.xyz_au[0] - v.x, dy = r.value().pos.xyz_au[1] - v.y,
                 dz = r.value().pos.xyz_au[2] - v.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz) * kAuM / 1000.0;
}

// Within 10 years of the element epoch, where the missing perturbers and
// relativity stay below the gates.
TEST_CASE("horizons_small_bodies") {
    Engine* e = engine();
    if (!e)
        return;
    Worst at_epoch, ten_years, seed_km;
    for (const HorizonsObs& h : kHorizonsObs) {
        const double years = std::fabs(h.jd_tt - kSmallEpoch) / 365.25;
        if (!is_small(h) || years > 10.5)
            continue;
        const double sep = small_body_offset_arcsec(*e, h, nullptr);
        (years < 1.0 ? at_epoch : ten_years).add(sep, h);
        CHECK(sep < (years < 1.0 ? 0.005 : 0.15));
    }
    for (const HorizonsVec& v : kHorizonsVec) {
        if (std::fabs(v.jd_tdb - kSmallEpoch) > 1.0)
            continue;
        const double km = heliocentric_km(*e, v);
        if (km > std::fabs(seed_km.v)) {
            seed_km.v = km;
            seed_km.where = v.request;
        }
        CHECK(km < 10.0); // the seed: elements, frame and epoch conventions
    }
    at_epoch.print("small bodies astrometric, element epoch", "\"");
    ten_years.print("small bodies astrometric, +-10 years", "\"");
    seed_km.print("small bodies heliocentric, element epoch", "km");
}

// With JPL's 16 asteroid perturbers (as Horizons integrates), within 10 years
// of the element epoch; skipped without the SB441-N16 kernel.
TEST_CASE("horizons_small_bodies_sb441") {
    Engine* e = engine_sb441();
    if (!e)
        return;
    Worst at_epoch, ten_years;
    for (const HorizonsObs& h : kHorizonsObs) {
        const double years = std::fabs(h.jd_tt - kSmallEpoch) / 365.25;
        if (!is_small(h) || years > 10.5)
            continue;
        const double sep = small_body_offset_arcsec(*e, h, nullptr);
        (years < 1.0 ? at_epoch : ten_years).add(sep, h);
        CHECK(sep < (years < 1.0 ? 0.005 : 0.01));
    }
    at_epoch.print("SB441: small bodies astrometric, element epoch", "\"");
    ten_years.print("SB441: small bodies astrometric, +-10 years", "\"");
}

// sigma_arcsec (the propagated full JPL covariance) against Horizons' own
// 3-sigma plane-of-sky uncertainty, within 10 years of the element epoch.
// POS_3sigma is the root-sum-square of the error ellipse's semi-axes while
// sigma_arcsec is the major semi-axis, so JPL / (3 sigma) lies in [1, sqrt 2];
// JPL prints 0.001" steps, which dominates below ~0.02".
TEST_CASE("horizons_sigma_calibration") {
    Engine* e = engine();
    if (!e)
        return;
    Worst worst_ratio;
    int checked = 0;
    for (const HorizonsObs& h : kHorizonsObs) {
        const double years = std::fabs(h.jd_tt - kSmallEpoch) / 365.25;
        if (!is_small(h) || years > 10.5 || std::isnan(h.pos_3s))
            continue;
        CalcOptions o = options_for(h, true);
        auto r = e->calc(h.body, h.jd_tt, o);
        REQUIRE(r.ok());
        REQUIRE(r.value().sigma_arcsec.has_value());
        const double ours3 = 3.0 * *r.value().sigma_arcsec;
        CHECK(ours3 <= h.pos_3s + 0.0006);
        CHECK(ours3 * std::sqrt(2.0) >= h.pos_3s - 0.0006);
        if (h.pos_3s >= 0.02) {
            const double ratio = h.pos_3s / ours3;
            worst_ratio.add(ratio - 1.0, h);
            CHECK(ratio > 0.97);
            CHECK(ratio < 1.45);
        }
        ++checked;
    }
    CHECK(checked == 21); // 7 bodies x 3 epochs
    worst_ratio.print("JPL pos 3-sigma / ours - 1 (>= 0.02\")", "");
}

// Report only (seconds: +-100-year integrations and the sigma tracks).
TEST_CASE("horizons_small_bodies_long_arc_report" * doctest::skip()) {
    Engine* e = engine();
    if (!e)
        return;
    Engine* p = engine_sb441(); // optional column
    std::printf("  %-16s %6s %12s %12s %14s %14s\n", "request", "years", "astrometric",
                "with SB441", "3-sigma ours", "3-sigma JPL");
    for (const HorizonsObs& h : kHorizonsObs) {
        if (!is_small(h))
            continue;
        double ours3 = kNa;
        const double sep = small_body_offset_arcsec(*e, h, &ours3);
        const double sep_p = p ? small_body_offset_arcsec(*p, h, nullptr) : kNa;
        std::printf("  %-16s %+6.0f %11.4f\" %11.4f\" %13.4f\" %13.3f\"\n", h.request,
                    (h.jd_tt - kSmallEpoch) / 365.25, sep, sep_p, ours3, h.pos_3s);
    }
    for (const HorizonsVec& v : kHorizonsVec)
        std::printf("  %-16s %+6.0f %12.1f km %12.1f km heliocentric (without / with SB441)\n",
                    v.request, (v.jd_tdb - kSmallEpoch) / 365.25, heliocentric_km(*e, v),
                    p ? heliocentric_km(*p, v) : kNa);
}

} // namespace

// SPDX-License-Identifier: GPL-2.0-or-later
//
// House systems (docs/HOUSES.md). The geometry is held to swetest's printed
// cusps and angles at swetest's own ARMC (tools/gen/gen_house_fixtures.py),
// and the sidereal time to ERFA's IAU 2006/2000A gst06a, so each is checked
// apart from the other. The polar-circle rules are checked from their
// definitions, since there swetest substitutes or mirrors.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include <prometheia/engine.hpp>
#include <prometheia/frames.hpp>
#include <prometheia/houses.hpp>

#include <doctest/doctest.h>

using namespace prometheia;

namespace {

#include "house_fixtures.inc"

constexpr double kDeg = 3.14159265358979323846 / 180.0;

double diff_arcsec(double a, double b) {
    double d = std::fmod(a - b, 360.0);
    if (d > 180.0)
        d -= 360.0;
    if (d < -180.0)
        d += 360.0;
    return std::fabs(d) * 3600.0;
}

// swetest's true obliquity: Vondrak 2011's epsilon_A series plus the
// nutation (docs/CROSS-TEST.md, swetest_mean_obliquity_is_the_epsilon_a_series),
// so the geometry is graded on the oracle's own obliquity.
double swetest_obliquity(double jd_tt) {
    double dpsi = 0.0, deps = 0.0;
    frames::nutation(jd_tt, dpsi, deps);
    return frames::ltp_obliquity_series(jd_tt) + deps;
}

// The engine's true obliquity of date under its default precession.
double true_obliquity(double jd_tt) {
    double dpsi = 0.0, deps = 0.0;
    frames::nutation(jd_tt, dpsi, deps);
    return frames::mean_obliquity(jd_tt, frames::PrecessionModel::Vondrak2011) + deps;
}

// Altitude (deg) of an ecliptic longitude for an ARMC, obliquity and latitude.
double altitude(double lon_deg, double armc_deg, double eps, double lat_deg) {
    const double l = lon_deg * kDeg, phi = lat_deg * kDeg;
    const double x = std::cos(l), y = std::sin(l) * std::cos(eps), z = std::sin(l) * std::sin(eps);
    const double ha = armc_deg * kDeg - std::atan2(y, x);
    const double dec = std::asin(z);
    return std::asin(std::sin(phi) * std::sin(dec) + std::cos(phi) * std::cos(dec) * std::cos(ha)) /
           kDeg;
}

} // namespace

TEST_CASE("houses_letters_and_tokens_round_trip") {
    for (char c : std::string("PKORCAWBMXT")) {
        auto s = houses::from_letter(c);
        REQUIRE(s.has_value());
        CHECK(houses::letter(*s) == c);
        CHECK(houses::from_token(houses::token(*s)) == s);
    }
    CHECK(houses::from_letter('E') == houses::System::Equal);
    CHECK_FALSE(houses::from_letter('G').has_value());
    CHECK_FALSE(houses::from_token("gauquelin").has_value());
}

TEST_CASE("houses_match_swetest_at_its_own_armc") {
    // Every system, latitudes to +-66 deg, 1600-2200, and the polar rows the
    // definitions share with swetest. 0.01" bounds the obliquity models'
    // difference (the pole angle against swetest's epsilon_A) as the high
    // latitudes amplify it, and swetest's 7 printed decimals (0.0004").
    double worst = 0.0, worst_koch = 0.0;
    int compared = 0;
    for (const HouseFixture& f : kHouseFixtures) {
        const double jd_tt = f.jd_ut + f.delta_t_s / 86400.0;
        auto h = houses::compute(*houses::from_letter(f.system), f.v[14] * kDeg,
                                 swetest_obliquity(jd_tt), f.lat_deg * kDeg);
        REQUIRE_MESSAGE(h.ok(), f.system, " ", f.lat_deg, ": ", h.error().message);
        const auto& g = h.value().angles;
        const double ours[17] = {h.value().cusp_deg[0],
                                 h.value().cusp_deg[1],
                                 h.value().cusp_deg[2],
                                 h.value().cusp_deg[3],
                                 h.value().cusp_deg[4],
                                 h.value().cusp_deg[5],
                                 h.value().cusp_deg[6],
                                 h.value().cusp_deg[7],
                                 h.value().cusp_deg[8],
                                 h.value().cusp_deg[9],
                                 h.value().cusp_deg[10],
                                 h.value().cusp_deg[11],
                                 g.asc_deg,
                                 g.mc_deg,
                                 g.armc_deg,
                                 g.vertex_deg,
                                 g.equatorial_asc_deg};
        for (int i = 0; i < 17; ++i) {
            if (i == 13 && !f.mc_compared)
                continue;
            const double d = diff_arcsec(ours[i], f.v[i]);
            // Within a degree of the polar circle Koch's MC semi-arc moves
            // cusp 11 by 0.087" per mas of obliquity (measured at -66 deg,
            // 2026), and swetest's nutation differs from the full IAU 2000A
            // at the sub-mas level: 0.1" is 1 mas there.
            const bool koch_near_circle = f.system == 'K' && std::fabs(f.lat_deg) > 65.0;
            CHECK_MESSAGE(d < (koch_near_circle ? 0.1 : 0.01), f.system, " lat ", f.lat_deg, " jd ",
                          f.jd_ut, " value ", i + 1, ": ", d, "\"");
            (koch_near_circle ? worst_koch : worst) =
                std::max(koch_near_circle ? worst_koch : worst, d);
            ++compared;
        }
    }
    std::printf("  houses vs swetest: %d values, worst %.4f\" (Koch within 1 deg of the polar "
                "circle %.4f\")\n",
                compared, worst, worst_koch);
}

TEST_CASE("placidus_and_koch_refuse_inside_the_polar_circle") {
    const double eps = true_obliquity(2451545.0);
    const double circle = 90.0 - eps / kDeg; // 66.56 deg
    for (auto s : {houses::System::Placidus, houses::System::Koch}) {
        for (double armc = 0.0; armc < 360.0; armc += 15.0) {
            for (double lat : {circle + 0.001, 70.0, 89.9, -(circle + 0.001), -80.0}) {
                auto h = houses::compute(s, armc * kDeg, eps, lat * kDeg);
                REQUIRE_FALSE(h.ok());
                CHECK(h.error().code == ErrorCode::ArgumentError);
                CHECK(h.error().message.find("polar circle") != std::string::npos);
            }
            for (double lat : {circle - 0.001, -(circle - 0.001), 60.0}) {
                auto h = houses::compute(s, armc * kDeg, eps, lat * kDeg);
                CHECK_MESSAGE(h.ok(), houses::name(s).data(), " armc ", armc, " lat ", lat);
            }
        }
    }
    // Every other system answers there.
    for (char c : std::string("ORCAWBMXT"))
        for (double armc = 0.0; armc < 360.0; armc += 15.0)
            CHECK(houses::compute(*houses::from_letter(c), armc * kDeg, eps, 80.0 * kDeg).ok());
    CHECK_FALSE(houses::compute(houses::System::Porphyry, 0.0, eps, 90.0 * kDeg).ok());
}

TEST_CASE("house_axes_follow_their_definitions_inside_the_polar_circle") {
    // The MC is the ecliptic's culminating point (hour angle 0) in every
    // system, even below the horizon. Regiomontanus' and Campanus' cusp 10 is
    // the meridian's ecliptic point above the horizon (their circles' zenith
    // half), which there can be the IC; Topocentric's cusp 10 is the MC
    // (pole height 0), whatever the horizon.
    const double eps = true_obliquity(2451545.0);
    int ic_cases = 0;
    for (double lat : {67.0, 70.0, 80.0, -70.0}) {
        for (double armc = 0.0; armc < 360.0; armc += 5.0) {
            for (char c : std::string("RCT")) {
                auto h = houses::compute(*houses::from_letter(c), armc * kDeg, eps, lat * kDeg);
                REQUIRE(h.ok());
                const auto& g = h.value().angles;
                const double mc_ra = std::atan2(std::sin(g.mc_deg * kDeg) * std::cos(eps),
                                                std::cos(g.mc_deg * kDeg)) /
                                     kDeg;
                CHECK(diff_arcsec(mc_ra, armc) < 1e-6);
                const double c10 = h.value().cusp_deg[9];
                if (c == 'T') {
                    CHECK(diff_arcsec(c10, g.mc_deg) < 1e-6);
                } else {
                    CHECK(altitude(c10, armc, eps, lat) > -1e-9);
                    if (diff_arcsec(c10, g.mc_deg) > 1.0)
                        ++ic_cases;
                }
            }
        }
    }
    CHECK(ic_cases > 0); // the case exists in this sample
}

TEST_CASE("houses_every_cusp_follows_the_last") {
    // Cusps run eastward round the ecliptic, each house less than 180 deg,
    // wherever a system answers (short of the polar circle's own reversals).
    const double eps = true_obliquity(2451545.0);
    for (char c : std::string("PKORCAWBMXT")) {
        for (double lat : {-60.0, -30.0, 0.0, 30.0, 60.0}) {
            for (double armc = 0.0; armc < 360.0; armc += 7.5) {
                auto h = houses::compute(*houses::from_letter(c), armc * kDeg, eps, lat * kDeg);
                REQUIRE(h.ok());
                for (int k = 0; k < 12; ++k) {
                    double span = std::fmod(
                        h.value().cusp_deg[(k + 1) % 12] - h.value().cusp_deg[k] + 360.0, 360.0);
                    CHECK_MESSAGE((span > 0.0 && span < 180.0), c, " lat ", lat, " armc ", armc,
                                  " house ", k + 1, " spans ", span);
                }
            }
        }
    }
}

TEST_CASE("engine_houses_sidereal_time_is_iau_2006") {
    const std::string path = std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_p1550p2650.440";
    if (!std::filesystem::exists(path)) {
        std::printf("  [SKIP] %s not found\n", path.c_str());
        return;
    }
    auto e = Engine::open(path);
    REQUIRE(e.ok());
    double worst = 0.0;
    for (const GastFixture& f : kGastFixtures) {
        frames::GeoSite site; // Greenwich
        auto h = e.value().houses_ut(houses::System::Porphyry, f.jd_ut, site);
        REQUIRE(h.ok());
        const double d = diff_arcsec(h.value().houses.angles.armc_deg, f.gast_deg);
        CHECK(d < 0.001);
        worst = std::max(worst, d);
    }
    std::printf("  ARMC vs ERFA gst06a: worst %.5f\"\n", worst);
}

TEST_CASE("engine_houses_sidereal_whole_sign_counts_from_the_sidereal_ascendant") {
    const std::string path = std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_p1550p2650.440";
    if (!std::filesystem::exists(path)) {
        std::printf("  [SKIP] %s not found\n", path.c_str());
        return;
    }
    auto e = Engine::open(path);
    REQUIRE(e.ok());
    frames::GeoSite site;
    site.lon_rad = 8.55 * kDeg;
    site.lat_rad = 47.6 * kDeg;
    CalcOptions o;
    o.sidereal = SiderealMode::Lahiri;
    for (double jd = 2451545.0; jd < 2451546.0; jd += 1.0 / 24.0) {
        auto trop = e.value().houses_ut(houses::System::Equal, jd, site);
        auto sid = e.value().houses_ut(houses::System::WholeSign, jd, site, o);
        auto sid_eq = e.value().houses_ut(houses::System::Equal, jd, site, o);
        REQUIRE((trop.ok() && sid.ok() && sid_eq.ok()));
        const double aya = *sid.value().ayanamsa_deg;
        const double asc = sid.value().houses.angles.asc_deg;
        CHECK(diff_arcsec(asc, std::fmod(trop.value().houses.angles.asc_deg - aya + 360.0, 360.0)) <
              1e-6);
        CHECK(sid.value().houses.cusp_deg[0] == std::floor(asc / 30.0) * 30.0);
        CHECK(diff_arcsec(sid_eq.value().houses.cusp_deg[0], asc) < 1e-6);
        CHECK(diff_arcsec(sid.value().houses.angles.armc_deg,
                          trop.value().houses.angles.armc_deg) == 0.0);
    }
    CalcOptions fixed = o;
    fixed.sidereal_plane = SiderealPlane::Invariable;
    CHECK_FALSE(e.value().houses_ut(houses::System::Placidus, 2451545.0, site, fixed).ok());
    CalcOptions j2000;
    j2000.frame = Frame::J2000;
    CHECK_FALSE(e.value().houses_ut(houses::System::Placidus, 2451545.0, site, j2000).ok());
}

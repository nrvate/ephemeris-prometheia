// SPDX-License-Identifier: GPL-2.0-or-later
//
// Phenomena and magnitudes (prometheia/phenomena.hpp, docs/PHENOMENA.md).
//
// Part A runs everywhere: the published formulas at points their papers fix.
// Part B (PROMETHEIA_DE440, default ephe/linux_p1550p2650.440; SKIP when
// absent): Engine::phenomena against swetest's printed output on the same
// DE440 (tests/phenomena_fixtures.inc, tools/gen/gen_phenomena_fixtures.py).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <unistd.h>

#include <prometheia/engine.hpp>
#include <prometheia/phenomena.hpp>

#include "phenomena_fixtures.inc"
#include <doctest/doctest.h>

using namespace prometheia;
using namespace prometheia::phenomena;

namespace {

MagnitudeInputs at(double alpha, double r = 1.0, double d = 1.0) {
    MagnitudeInputs in;
    in.r_au = r;
    in.d_au = d;
    in.phase_angle_deg = alpha;
    in.jd_tt = 2461042.0;
    return in;
}

std::string de440() {
    const char* v = std::getenv("PROMETHEIA_DE440");
    return v ? v : std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_p1550p2650.440";
}

} // namespace

TEST_CASE("phenomena_formulas_at_their_published_points") {
    // V(1,0): every formula at r = d = 1 au and zero phase.
    CHECK(body_magnitude(199, at(0.0)).v == doctest::Approx(-0.613));
    CHECK(body_magnitude(299, at(0.0)).v == doctest::Approx(-4.384));
    CHECK(body_magnitude(399, at(0.0)).v == doctest::Approx(-3.99));
    CHECK(body_magnitude(4, at(0.0)).v == doctest::Approx(-1.601));
    CHECK(body_magnitude(5, at(0.0)).v == doctest::Approx(-9.395));
    CHECK(body_magnitude(7, at(0.0)).v == doctest::Approx(-7.110)); // pole-on aside
    CHECK(body_magnitude(8, at(0.0)).v == doctest::Approx(-7.00));  // after 2000
    CHECK(body_magnitude(10, at(0.0)).v == doctest::Approx(-26.76));
    CHECK(body_magnitude(10, at(0.0, 1.0, 0.98)).v ==
          doctest::Approx(-26.76 + 5.0 * std::log10(0.98)));
    // Saturn with the rings edge-on is the globe's eq. 10 constant.
    CHECK(body_magnitude(6, at(0.0)).v == doctest::Approx(-8.914));
    // The paper's own joins: eq. 9 meets eq. 8 at 12 deg, eq. 7 meets eq. 6
    // at 50 deg (to the constants' rounding).
    CHECK(std::fabs(body_magnitude(5, at(12.0)).v - body_magnitude(5, at(12.0001)).v) < 0.003);
    CHECK(std::fabs(body_magnitude(4, at(50.0)).v - body_magnitude(4, at(50.0001)).v) < 0.01);
    CHECK(body_magnitude(5, at(12.0)).model == "Mallama & Hilton 2018 eq. 8");
    CHECK(body_magnitude(5, at(12.0001)).model == "Mallama & Hilton 2018 eq. 9");
    // Neptune's secular term: -6.89 before 1980, falling 0.0054/yr to 2000.
    MagnitudeInputs n1990 = at(0.0);
    n1990.jd_tt = 2447892.5; // 1990-01-01
    CHECK(body_magnitude(8, n1990).v == doctest::Approx(-6.89 - 0.0054 * 10.0).epsilon(1e-4));
    MagnitudeInputs n1970 = at(0.0);
    n1970.jd_tt = 2440587.5;
    CHECK(body_magnitude(8, n1970).v == doctest::Approx(-6.89));
    // Uranus: planetographic latitudes, |avg| at 8.4e-4 mag/deg.
    MagnitudeInputs u = at(0.0);
    u.sub_observer_lat_deg = 0.0;
    u.sub_solar_lat_deg = 0.0;
    CHECK(body_magnitude(7, u).v == doctest::Approx(-7.110));
    u.sub_observer_lat_deg = -90.0 + 1e-9;
    u.sub_solar_lat_deg = 90.0 - 1e-9;
    CHECK(body_magnitude(7, u).v == doctest::Approx(-7.110 - 8.4e-4 * 90.0).epsilon(1e-6));
    // The Moon full at its mean distance, 1 au from the Sun.
    CHECK(body_magnitude(301, at(0.0, 1.0, 384400.0 / 149597870.7)).v == doctest::Approx(-12.73));
    // Out of range: named, not extrapolated. Every name fits the C API's
    // 64-byte magnitude_model.
    CHECK(std::isnan(body_magnitude(301, at(120.5)).v));
    CHECK(std::isfinite(body_magnitude(301, at(120.0)).v));
    for (long id : {10L, 301L, 199L, 299L, 399L, 4L, 5L, 6L, 7L, 8L, 9L, 3L})
        for (double a : {0.0, 2.0, 7.0, 13.0, 60.0, 125.0, 140.0, 155.0, 165.0, 179.5})
            CHECK(body_magnitude(id, at(a)).model.size() < 64);
    CHECK(std::isnan(body_magnitude(299, at(179.5)).v));
    CHECK(std::isnan(body_magnitude(5, at(131.0)).v));
    CHECK(std::isnan(body_magnitude(9, at(1.0)).v));
    CHECK(std::isnan(body_magnitude(3, at(1.0)).v));
    // H, G at zero phase is H at 1 au; G = 0 and 1 give phi1 and phi2.
    CHECK(hg_magnitude(3.34, 0.12, 1.0, 1.0, 0.0).v == doctest::Approx(3.34));
    const double t = std::tan(0.5 * 20.0 * 3.14159265358979323846 / 180.0);
    CHECK(hg_magnitude(5.0, 0.0, 2.0, 1.5, 20.0).v ==
          doctest::Approx(5.0 + 5.0 * std::log10(3.0) -
                          2.5 * std::log10(std::exp(-3.33 * std::pow(t, 0.63)))));
    CHECK(std::isnan(hg_magnitude(std::nan(""), 0.15, 1.0, 1.0, 0.0).v));
    // Radii and poles.
    CHECK(mean_radius_km(10) == 695700.0);
    CHECK(mean_radius_km(5) == mean_radius_km(599));
    double pole[3];
    REQUIRE(pole_icrf(699, 2451545.0, pole));
    CHECK(std::asin(pole[2]) * 180.0 / 3.14159265358979323846 == doctest::Approx(83.537));
    CHECK(!pole_icrf(5, 2451545.0, pole));
}

TEST_CASE("phenomena_against_swetest") {
    if (access(de440().c_str(), R_OK) != 0) {
        MESSAGE("SKIP: no DE440 at " << de440());
        return;
    }
    auto opened = Engine::open(de440());
    REQUIRE(opened.ok());
    Engine e = std::move(opened).value();
    // The bounds are the measured differences (docs/PHENOMENA.md, "Validation"),
    // rounded up; swetest prints V to 0.001 and angles to 0.0001".
    const std::map<long, double> mag_bound = {{199, 0.001}, {299, 0.001}, {4, 0.001}, {5, 0.001},
                                              {6, 0.015},   {7, 0.025},   {8, 0.002}, {301, 0.012}};
    std::map<long, double> worst_mag, worst_alpha, worst_elong, worst_diam;
    for (const PhenomenaFixture& f : kPhenomenaFixtures) {
        Engine::ObjectRef ref;
        ref.id = f.naif;
        auto r = e.phenomena(ref, f.jd_tt);
        REQUIRE_MESSAGE(r.ok(), f.naif);
        const Engine::Phenomena& p = r.value();
        const auto worse = [](std::map<long, double>& m, long k, double v) {
            m[k] = std::max(m[k], std::fabs(v));
        };
        worse(worst_elong, f.naif, (p.elongation_deg - f.elongation_deg) * 3600.0);
        worse(worst_alpha, f.naif, (p.phase_angle_deg - f.phase_angle_deg) * 3600.0);
        // swetest's Sun and Moon radii are 696,000 and 1737.5 km, ours the
        // IAU's 695,700 and 1737.4: compare the discs at one radius.
        const double radius_ratio = f.naif == 10    ? 695700.0 / 696000.0
                                    : f.naif == 301 ? 1737.4 / 1737.5
                                                    : 1.0;
        worse(worst_diam, f.naif, p.diameter_arcsec - f.diameter_arcsec * radius_ratio);
        if (f.naif == 10) {
            // Willmer's -26.76 at 1 au against swetest's -26.86: a constant.
            CHECK(p.magnitude - f.magnitude == doctest::Approx(0.10).epsilon(0.006));
        } else if (f.naif == 301 && f.phase_angle_deg > 120.0) {
            // Past the observations behind the formula: none (docs/PHENOMENA.md).
            CHECK(std::isnan(p.magnitude));
            CHECK(p.magnitude_model == "no magnitude: Moon beyond 120 deg (Lane & Irvine 1973)");
        } else {
            worse(worst_mag, f.naif, p.magnitude - f.magnitude);
            CHECK_MESSAGE(std::fabs(p.magnitude - f.magnitude) <= mag_bound.at(f.naif), "naif ",
                          f.naif, " at ", f.jd_tt);
        }
        CHECK_MESSAGE(std::fabs(p.elongation_deg - f.elongation_deg) * 3600.0 <=
                          (f.naif == 301 ? 0.002 : 0.0002),
                      "naif ", f.naif, " at ", f.jd_tt);
        // The phase angle is the angle at the body at emission; swetest's
        // is another construction, up to 40" away (docs/PHENOMENA.md).
        CHECK(std::fabs(p.phase_angle_deg - f.phase_angle_deg) * 3600.0 <= 40.0);
        CHECK(std::fabs(p.diameter_arcsec - f.diameter_arcsec * radius_ratio) <=
              1e-4 + 1e-6 * f.diameter_arcsec);
    }
    for (const auto& [naif, d] : worst_alpha)
        MESSAGE("naif " << naif << ": |dV| " << worst_mag[naif] << ", |d alpha| " << d
                        << "\", |d elong| " << worst_elong[naif] << "\", |d diam| "
                        << worst_diam[naif] << "\"");
}

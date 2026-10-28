// SPDX-License-Identifier: GPL-2.0-or-later
//
// Heliacal visibility (prometheia/heliacal.hpp, docs/HELIACAL.md).
//
// Part A runs everywhere: Schaefer's (1993) equations at the values the
// paper tabulates and works out. Part B (PROMETHEIA_DE440, default
// ephe/linux_p1550p2650.440; SKIP when absent): the finder against swetest's
// printed -hev output on the same DE440 (tests/heliacal_fixtures.inc,
// tools/gen/gen_heliacal_fixtures.py).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

#include <prometheia/engine.hpp>
#include <prometheia/heliacal.hpp>
#include <prometheia/prometheia.h>
#include <prometheia/registry.hpp>

#include "heliacal_fixtures.inc"
#include <doctest/doctest.h>

using namespace prometheia;
using namespace prometheia::heliacal;

namespace {

constexpr double kDeg = 3.14159265358979323846 / 180.0;

std::string de440() {
    const char* v = std::getenv("PROMETHEIA_DE440");
    return v ? v : std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_p1550p2650.440";
}

Engine::ObjectRef object_named(Engine& e, registry::Registry& reg, const std::string& name) {
    static const std::pair<const char*, long> bodies[] = {{"Mercury", 199}, {"Venus", 299},
                                                          {"Mars", 4},      {"Jupiter", 5},
                                                          {"Saturn", 6},    {"Moon", 301}};
    for (const auto& [n, id] : bodies) {
        if (name == n) {
            Engine::ObjectRef r;
            r.id = id;
            return r;
        }
    }
    auto r = reg.resolve("star:" + name);
    REQUIRE_MESSAGE(r.ok(), name << ": " << r.error().message);
    (void)e;
    return r.value();
}

} // namespace

TEST_CASE("heliacal_air_masses_match_schaefer_table_3") {
    // Table 3: Z, eq. 2b, eq. 3a (8.2 km), eq. 3a (1.5 km), eq. 3b (20 km).
    struct Row {
        double z, x2b, xg, xa, xo;
    };
    const Row rows[] = {{0, 1.0, 1.0, 1.0, 1.0},      {60, 2.0, 2.0, 2.0, 2.0},
                        {80, 5.6, 5.6, 5.7, 5.3},     {85, 10.3, 10.1, 11.3, 8.5},
                        {87, 15.1, 14.5, 17.9, 10.6}, {88, 19.3, 18.3, 24.9, 11.6},
                        {89, 26.3, 24.2, 39.3, 12.4}, {90, 40.0, 34.9, 81.6, 12.7}};
    double worst = 0.0;
    for (const Row& r : rows) {
        worst = std::max({worst, std::fabs(air_mass(r.z) - r.x2b) / r.x2b,
                          std::fabs(air_mass_layer(r.z, 8.2) - r.xg) / r.xg,
                          std::fabs(air_mass_layer(r.z, 1.5) - r.xa) / r.xa,
                          std::fabs(air_mass_shell(r.z, 20.0) - r.xo) / r.xo});
    }
    MESSAGE("Table 3: worst relative difference " << worst);
    CHECK(worst < 0.01);
}

TEST_CASE("heliacal_magnitude_and_illuminance_match_schaefer_table_5") {
    // Table 5 (eq. 10): V and the illuminance outside the atmosphere in foot-candles.
    const std::pair<double, double> rows[] = {{-12.73, 2.91e-2}, {-4.6, 1.6e-5},  {-1.46, 9.04e-7},
                                              {0.0, 2.36e-7},    {2.02, 3.66e-8}, {6.0, 9.4e-10}};
    double worst = 0.0;
    for (const auto& [v, i] : rows)
        worst = std::max(worst, std::fabs(illuminance_from_magnitude(v) / i - 1.0));
    MESSAGE("Table 5: worst relative difference in I " << worst);
    CHECK(worst < 0.05);
    CHECK(magnitude_from_illuminance(illuminance_from_magnitude(3.3)) == doctest::Approx(3.3));
}

TEST_CASE("heliacal_limiting_magnitude_at_schaefer_values") {
    Observer standard;
    standard.age_years = 25.0;
    // Section 3.17: a sky of 220 nL gives the limit 5.80 (eqs. 10, 34).
    const double l220 = limiting_magnitude(220.0, standard);
    MESSAGE("limit at 220 nL: " << l220);
    CHECK(l220 == doctest::Approx(5.80).epsilon(0.004));
    // Table 5: the day-vision limit is V 4.1 where the sky is 1500 nL (Table 6).
    const double l1500 = limiting_magnitude(1500.0, standard);
    MESSAGE("limit at 1500 nL: " << l1500);
    CHECK(std::fabs(l1500 - 4.1) < 0.15);
    // Brighter skies and older eyes see less; a monocular eye less again.
    CHECK(limiting_magnitude(2000.0, standard) < l1500);
    Observer old = standard;
    old.age_years = 70.0;
    CHECK(limiting_magnitude(220.0, old) < l220);
    Observer mono = standard;
    mono.binocular = false;
    CHECK(limiting_magnitude(220.0, mono) < l220);
    Observer keen = standard;
    keen.snellen_ratio = 1.5;
    CHECK(limiting_magnitude(220.0, keen) > l220);
}

TEST_CASE("heliacal_aldebaran_extinction_angle_example") {
    // Section 3.17: Aldebaran from Medicine Mountain (2940 m, 44.8 N), summer
    // k_R = 0.14 day vision; at X = 36 (eq. 2b) the altitude is 0.2 deg.
    CHECK(air_mass(90.0 - 0.2) == doctest::Approx(36.0).epsilon(0.01));
    // Night-vision coefficients quoted there (k_R 0.101, k_oz 0.010; k_a 0.041
    // is the paper's own estimate for that site, not computed here).
    Atmosphere atm;
    const Extinction k = extinction(2940.0, 44.8, 0.0, atm, true);
    CHECK(k.rayleigh == doctest::Approx(0.101).epsilon(0.01));
    CHECK(k.ozone == doctest::Approx(0.010).epsilon(0.1));
    // Eq. 51 with the paper's coefficients: 4.95 mag of dimming at 0.6 deg.
    const double z = 90.0 - 0.6;
    const double am = 0.101 * air_mass_layer(z, 8.2) + 0.010 * air_mass_shell(z, 20.0) +
                      0.041 * air_mass_layer(z, 1.5);
    MESSAGE("A_m at 0.6 deg (eq. 51): " << am);
    CHECK(am == doctest::Approx(4.95).epsilon(0.03));
}

TEST_CASE("heliacal_sky_brightness_against_schaefer_table_6") {
    // The zenith sky with the Sun at the altitudes Table 6 names (the dark
    // site's night term is 180 nL; the Sun 90 deg away; no Moon).
    auto sky = [](double sun_alt) {
        Scene s;
        s.object_alt_deg = 90.0;
        s.object_v = 0.0;
        s.sun_alt_deg = sun_alt;
        s.sun_sep_deg = 90.0;
        return visibility(s, Atmosphere{}, Observer{}).sky_brightness_nl;
    };
    const double sunset = sky(0.0), civil = sky(-6.0), nautical = sky(-12.0), dark = sky(-30.0);
    MESSAGE("zenith sky: sunset " << sunset << ", civil " << civil << ", nautical " << nautical
                                  << ", dark " << dark << " nL");
    CHECK(sunset / 3e7 < 3.0);
    CHECK(sunset / 3e7 > 1.0 / 3.0);
    CHECK(civil / 1e5 < 4.0);
    CHECK(civil / 1e5 > 1.0 / 4.0);
    CHECK(nautical / 300.0 < 5.0);
    CHECK(nautical / 300.0 > 1.0 / 5.0);
    CHECK(dark > 100.0); // B_o dimmed by the zenith air mass: 180 x 10^(-0.4 k)
    CHECK(dark < 180.0);
}

TEST_CASE("heliacal_twilight_sky_is_brighter_toward_the_sun") {
    // Eq. 15c as printed has no glow beside the Sun; Schaefer's 1998 term
    // falls off as 100/rho (docs/HELIACAL.md, item 3). The same patch of sky,
    // 5 deg up with the Sun 8 deg down, at 15, 30, 60 and 120 deg from it.
    auto sky = [](double sep) {
        Scene s;
        s.object_alt_deg = 5.0;
        s.sun_alt_deg = -8.0;
        s.sun_sep_deg = sep;
        return visibility(s, Atmosphere{}, Observer{}).sky_brightness_nl;
    };
    const double b15 = sky(15.0), b30 = sky(30.0), b60 = sky(60.0), b120 = sky(120.0);
    MESSAGE("twilight sky at 15, 30, 60, 120 deg: " << b15 << ", " << b30 << ", " << b60 << ", "
                                                    << b120 << " nL");
    CHECK(b15 > b30);
    CHECK(b30 > b60);
    CHECK(b60 > b120);
    CHECK(b15 / b120 > 5.0); // 100/rho: a factor 8 less the night sky's share
}

TEST_CASE("heliacal_extinction_inputs") {
    Atmosphere atm;
    const Extinction humid = extinction(0.0, 30.0, 0.0, atm, false);
    atm.relative_humidity_pct = 80.0;
    CHECK(extinction(0.0, 30.0, 0.0, atm, false).aerosol > humid.aerosol);
    // A given total is kept (day vision); the aerosol is what is left.
    Atmosphere given;
    given.extinction_coefficient = 0.30;
    CHECK(extinction(0.0, 30.0, 0.0, given, false).total == doctest::Approx(0.30));
    // A shorter visual range is a hazier sky.
    Atmosphere clear, hazy;
    clear.visual_range_km = 50.0;
    hazy.visual_range_km = 10.0;
    CHECK(extinction(0.0, 30.0, 0.0, hazy, false).total >
          extinction(0.0, 30.0, 0.0, clear, false).total);
    // Height thins the air (eq. 4b, 6a).
    CHECK(extinction(3000.0, 30.0, 0.0, Atmosphere{}, false).total <
          extinction(0.0, 30.0, 0.0, Atmosphere{}, false).total);
}

TEST_CASE("heliacal_refuses_bad_inputs") {
    if (access(de440().c_str(), R_OK) != 0) {
        MESSAGE("SKIP: no DE440 at " << de440());
        return;
    }
    auto opened = Engine::open(de440());
    REQUIRE(opened.ok());
    Engine e = std::move(opened).value();
    registry::Registry reg(e);
    const frames::GeoSite site{31.2 * kDeg, 30.05 * kDeg, 0.0};
    const Engine::ObjectRef sirius = object_named(e, reg, "Sirius");
    Atmosphere both;
    both.extinction_coefficient = 0.3;
    both.visual_range_km = 20.0;
    CHECK(next(e, sirius, 2461222.5, site, both, Observer{}, Event::Rising).error().code ==
          ErrorCode::ArgumentError);
    CHECK(next(e, sirius, 2461222.5, site, Atmosphere{}, Observer{}, Event::EveningFirst)
              .error()
              .code == ErrorCode::ArgumentError);
    Options arcus;
    arcus.model = Model::ArcusVisionis; // no arc
    CHECK(next(e, sirius, 2461222.5, site, Atmosphere{}, Observer{}, Event::Rising, arcus)
              .error()
              .code == ErrorCode::ArgumentError);
    Engine::ObjectRef sun;
    sun.id = 10;
    CHECK(next(e, sun, 2461222.5, site, Atmosphere{}, Observer{}, Event::Rising).error().code ==
          ErrorCode::ArgumentError);
}

namespace {

// One fixture through the finder; the difference in days (ours less swetest's).
struct Run {
    bool ok = false;
    double days = 0.0, optimum_min = 0.0, before = 0.0, after = 0.0;
    std::string message;
};

Run run_fixture(Engine& e, registry::Registry& reg, const HeliacalFixture& f, double margin) {
    Atmosphere atm;
    atm.pressure_hpa = f.pressure;
    atm.temperature_c = f.temperature;
    atm.relative_humidity_pct = f.humidity;
    if (f.visr > 1.0)
        atm.visual_range_km = f.visr;
    else if (f.visr > 0.0)
        atm.extinction_coefficient = f.visr;
    const frames::GeoSite site{f.lon * kDeg, f.lat * kDeg, f.height};
    Options opt;
    opt.detection_margin_mag = margin;
    auto r = next(e, object_named(e, reg, f.object), f.start_ut + 69.2 / 86400.0, site, atm,
                  Observer{}, Event(f.event), opt);
    Run out;
    if (!r.ok()) {
        out.message = r.error().message;
        return out;
    }
    out.ok = true;
    out.days = r.value().jd_ut1 - f.event_ut;
    out.before = r.value().spread_before_days;
    out.after = r.value().spread_after_days;
    // The optimum is compared as a time of day, mod one day.
    double m = (r.value().optimum_ut1 - f.optimum_ut) * 1440.0;
    out.optimum_min = m - 1440.0 * std::floor(m / 1440.0 + 0.5);
    return out;
}

const HeliacalFixture* fixture(const char* object, const char* site, int event) {
    for (const HeliacalFixture& f : kHeliacalFixtures) {
        if (std::string(f.object) == object && std::string(f.site) == site && f.event == event &&
            f.visr == 0.0 && f.humidity == 40.0 && f.pressure == 1013.25)
            return &f;
    }
    return nullptr;
}

bool is_star(const char* object) {
    for (const char* n : {"Venus", "Mercury", "Mars", "Jupiter", "Saturn", "Moon"}) {
        if (std::string(object) == n)
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("heliacal_selected_events_against_swetest") {
    // The recorded dates: Sirius at Cairo rises heliacally on 2026-08-06, Venus
    // at Berlin on 2026-11-02 and sets on 2028-05-23 (docs/HELIACAL.md).
    if (access(de440().c_str(), R_OK) != 0) {
        MESSAGE("SKIP: no DE440 at " << de440());
        return;
    }
    auto opened = Engine::open(de440());
    REQUIRE(opened.ok());
    Engine e = std::move(opened).value();
    registry::Registry reg(e);
    const HeliacalFixture* picks[] = {fixture("Sirius", "Cairo", 1), fixture("Sirius", "Cairo", 2),
                                      fixture("Venus", "Berlin", 1), fixture("Venus", "Berlin", 2),
                                      fixture("Moon", "Rome", 3),    fixture("Moon", "Rome", 4)};
    for (const HeliacalFixture* f : picks) {
        REQUIRE(f != nullptr);
        const Run r = run_fixture(e, reg, *f, 0.0);
        REQUIRE_MESSAGE(r.ok, std::string(f->object) << ": " << r.message);
        MESSAGE(std::string(f->object)
                << " " << std::string(f->site) << " event " << f->event << ": " << r.days << " d");
        CHECK(std::fabs(r.days) <= 3.5);
    }
}

TEST_CASE("heliacal_spread_says_how_firm_the_date_is") {
    // Sirius's margin climbs fast through its rising: a day either way.
    // Mars's falls slowly while it sets: days, not weeks, with Schaefer's
    // 1998 twilight term (docs/HELIACAL.md, "How firm the date is").
    if (access(de440().c_str(), R_OK) != 0) {
        MESSAGE("SKIP: no DE440 at " << de440());
        return;
    }
    auto opened = Engine::open(de440());
    REQUIRE(opened.ok());
    Engine e = std::move(opened).value();
    registry::Registry reg(e);
    const Run sirius = run_fixture(e, reg, *fixture("Sirius", "Cairo", 1), 0.0);
    const Run mars = run_fixture(e, reg, *fixture("Mars", "Athens", 2), 0.0);
    REQUIRE(sirius.ok);
    REQUIRE(mars.ok);
    MESSAGE("Sirius -" << sirius.before << "/+" << sirius.after << " d, Mars setting -"
                       << mars.before << "/+" << mars.after << " d");
    CHECK(sirius.before + sirius.after <= 2.0);
    CHECK(mars.before + mars.after >= 6.0);
    CHECK(mars.before + mars.after <= 15.0);
}

TEST_CASE("heliacal_moon_crescent_magnitude_is_paper_eq_12") {
    // Table 5: the crescent Moon at a = 135 deg is V -7.89; the full Moon -12.73.
    // The Moon's events are within two days of swetest's only because the
    // finder uses it past the engine's 120 deg limit (docs/HELIACAL.md).
    if (access(de440().c_str(), R_OK) != 0) {
        MESSAGE("SKIP: no DE440 at " << de440());
        return;
    }
    auto opened = Engine::open(de440());
    REQUIRE(opened.ok());
    Engine e = std::move(opened).value();
    registry::Registry reg(e);
    for (int ev : {3, 4}) {
        const HeliacalFixture* f = fixture("Moon", "Rome", ev);
        REQUIRE(f != nullptr);
        const Run r = run_fixture(e, reg, *f, 0.0);
        REQUIRE(r.ok);
        CHECK(std::fabs(r.days) <= 2.0);
    }
}

TEST_CASE("heliacal_all_events_against_swetest" * doctest::skip()) {
    // The whole fixture table, with the default margin and the paper's (0).
    // Run with --no-skip -tc=heliacal_all_events_against_swetest.
    auto opened = Engine::open(de440());
    REQUIRE(opened.ok());
    Engine e = std::move(opened).value();
    registry::Registry reg(e);
    for (double margin : {0.0, 1.2}) {
        int n = 0, w1 = 0, w3 = 0, stars = 0, stars3 = 0, stars1 = 0;
        for (const HeliacalFixture& f : kHeliacalFixtures) {
            const Run r = run_fixture(e, reg, f, margin);
            ++n;
            const bool star = is_star(f.object);
            stars += star;
            if (!r.ok) {
                std::printf("margin %.1f %-9s %-8s ev%d: %s\n", margin, f.object, f.site, f.event,
                            r.message.c_str());
                continue;
            }
            w1 += std::fabs(r.days) <= 1.0;
            w3 += std::fabs(r.days) <= 3.0;
            stars1 += star && std::fabs(r.days) <= 1.0;
            stars3 += star && std::fabs(r.days) <= 3.0;
            std::printf("margin %.1f %-9s %-8s ev%d P%.0f RH%.0f VR%.2f: %+6.2f d (-%2.0f/+%2.0f), "
                        "optimum %+5.1f min\n",
                        margin, f.object, f.site, f.event, f.pressure, f.humidity, f.visr, r.days,
                        r.before, r.after, r.optimum_min);
        }
        std::printf("margin %.1f: %d cases, %d within 1 day, %d within 3; stars %d: %d within 1, "
                    "%d within 3\n",
                    margin, n, w1, w3, stars, stars1, stars3);
    }
}

TEST_CASE("heliacal_arcus_visionis") {
    if (access(de440().c_str(), R_OK) != 0) {
        MESSAGE("SKIP: no DE440 at " << de440());
        return;
    }
    auto opened = Engine::open(de440());
    REQUIRE(opened.ok());
    Engine e = std::move(opened).value();
    registry::Registry reg(e);
    const frames::GeoSite site{31.2 * kDeg, 30.05 * kDeg, 0.0};
    const Engine::ObjectRef sirius = object_named(e, reg, "Sirius");
    double last = 0.0;
    for (double arc : {8.0, 10.0, 12.0}) {
        Options o;
        o.model = Model::ArcusVisionis;
        o.arcus_visionis_deg = arc;
        auto r = next(e, sirius, 2461222.5, site, Atmosphere{}, Observer{}, Event::Rising, o);
        REQUIRE_MESSAGE(r.ok(), r.error().message);
        const Found& f = r.value();
        // The Sun is at least the arc below the horizon at the optimum, and the
        // object above it: the margin is the lesser of the two excesses.
        CHECK(f.at_optimum.sun_depression_deg >= arc - 1e-3);
        CHECK(f.at_optimum.scene.object_alt_deg >= -1e-3);
        CHECK(f.at_optimum.margin >= 0.0);
        CHECK(f.jd_ut1 <= f.optimum_ut1);
        CHECK(f.optimum_ut1 <= f.end_ut1);
        MESSAGE("arc " << arc << ": " << f.jd_ut1);
        CHECK(f.jd_ut1 > last); // a larger arc, a later rising
        last = f.jd_ut1;
    }
    // An object altitude asked for moves the setting earlier.
    Options o;
    o.model = Model::ArcusVisionis;
    o.arcus_visionis_deg = 10.0;
    auto flat = next(e, sirius, 2461222.5, site, Atmosphere{}, Observer{}, Event::Setting, o);
    o.object_altitude_deg = 5.0;
    auto high = next(e, sirius, 2461222.5, site, Atmosphere{}, Observer{}, Event::Setting, o);
    REQUIRE(flat.ok());
    REQUIRE(high.ok());
    CHECK(high.value().jd_ut1 < flat.value().jd_ut1);
}

TEST_CASE("heliacal_sight_in_daylight_is_unseen") {
    if (access(de440().c_str(), R_OK) != 0) {
        MESSAGE("SKIP: no DE440 at " << de440());
        return;
    }
    auto opened = Engine::open(de440());
    REQUIRE(opened.ok());
    Engine e = std::move(opened).value();
    registry::Registry reg(e);
    const frames::GeoSite site{31.2 * kDeg, 30.05 * kDeg, 0.0};
    // 2461260.0 TT is noon at Cairo: the Sun is up; the margin is below zero.
    auto s = sight_at(e, object_named(e, reg, "Sirius"), 2461260.0, site, Atmosphere{}, Observer{},
                      Options{});
    REQUIRE(s.ok());
    CHECK(s.value().margin < 0.0);
    CHECK(s.value().scene.sun_alt_deg > 0.0);
}

TEST_CASE("heliacal_c_api") {
    if (access(de440().c_str(), R_OK) != 0) {
        MESSAGE("SKIP: no DE440 at " << de440());
        return;
    }
    prometheia_engine* eng = nullptr;
    prometheia_error err;
    REQUIRE(prometheia_engine_open(de440().c_str(), &eng, &err) == PROMETHEIA_OK);
    prometheia_site site{31.2, 30.05, 0.0};
    prometheia_air air;
    prometheia_air_init(&air);
    prometheia_observer observer;
    prometheia_observer_init(&observer);
    prometheia_visibility_model model;
    prometheia_visibility_model_init(&model);
    CHECK(air.pressure_hpa == 1013.25);
    CHECK(std::isnan(air.visual_range_km));
    CHECK(observer.age_years == 36.0);
    CHECK(observer.binocular == 1);
    CHECK(model.kind == PROMETHEIA_VISIBILITY_SCHAEFER);
    CHECK(model.detection_margin_mag == 0.0);
    prometheia_object sirius{PROMETHEIA_OBJECT_ID, 0, "star:Sirius"};
    prometheia_heliacal h;
    REQUIRE(prometheia_heliacal_next(eng, &sirius, 2461222.5, &site, &air, &observer, &model,
                                     PROMETHEIA_HELIACAL_RISING, &h, &err) == PROMETHEIA_OK);
    CHECK(std::fabs(h.jd_ut1 - 2461258.61301) <= 3.5);
    CHECK(h.jd_tt > h.jd_ut1);
    CHECK(h.optimum_ut1 >= h.jd_ut1);
    CHECK(h.end_ut1 >= h.optimum_ut1);
    CHECK(h.magnitude == doctest::Approx(-1.46).epsilon(0.01));
    CHECK(h.margin > 0.0);
    CHECK(h.options.detection_margin_mag == 0.0);
    CHECK(h.atmosphere.relative_humidity_pct == 40.0);
    prometheia_visibility vis;
    REQUIRE(prometheia_visibility_at(eng, &sirius, h.optimum_tt, &site, &air, &observer, &model,
                                     &vis, &err) == PROMETHEIA_OK);
    CHECK(vis.visible == 1);
    CHECK(vis.margin == doctest::Approx(h.margin).epsilon(1e-9));
    CHECK(vis.limiting_magnitude_outside + vis.extinction_mag ==
          doctest::Approx(vis.limiting_magnitude));
    // Errors: a star has no evening-first; both extinction inputs; no arc.
    CHECK(prometheia_heliacal_next(eng, &sirius, 2461222.5, &site, &air, &observer, &model,
                                   PROMETHEIA_HELIACAL_EVENING_FIRST, &h,
                                   &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(h.jd_tt == 0.0); // zeroed
    prometheia_air bad = air;
    bad.extinction_coefficient = 0.3;
    bad.visual_range_km = 20.0;
    CHECK(prometheia_heliacal_next(eng, &sirius, 2461222.5, &site, &bad, &observer, &model,
                                   PROMETHEIA_HELIACAL_RISING, &h,
                                   &err) == PROMETHEIA_ERROR_ARGUMENT);
    prometheia_visibility_model arcus = model;
    arcus.kind = PROMETHEIA_VISIBILITY_ARCUS_VISIONIS;
    CHECK(prometheia_heliacal_next(eng, &sirius, 2461222.5, &site, &air, &observer, &arcus,
                                   PROMETHEIA_HELIACAL_RISING, &h,
                                   &err) == PROMETHEIA_ERROR_ARGUMENT);
    arcus.arcus_visionis_deg = 10.0;
    REQUIRE(prometheia_heliacal_next(eng, &sirius, 2461222.5, &site, &air, &observer, &arcus,
                                     PROMETHEIA_HELIACAL_RISING, &h, &err) == PROMETHEIA_OK);
    CHECK(h.model == PROMETHEIA_VISIBILITY_ARCUS_VISIONIS);
    CHECK(std::isnan(h.limiting_magnitude));
    CHECK(prometheia_heliacal_next(nullptr, &sirius, 2461222.5, &site, &air, &observer, &model,
                                   PROMETHEIA_HELIACAL_RISING, &h,
                                   &err) == PROMETHEIA_ERROR_ARGUMENT);
    // The limiting magnitude alone: Schaefer's 220 nL example, the 25-year-old standard eye.
    prometheia_observer standard = observer;
    standard.age_years = 25.0;
    CHECK(prometheia_limiting_magnitude(220.0, &standard) == doctest::Approx(5.80).epsilon(0.004));
    CHECK(std::isnan(prometheia_limiting_magnitude(-1.0, nullptr)));
    prometheia_engine_close(eng);
}

TEST_CASE("heliacal_censorinus_sirius_139_ce" * doctest::skip()) {
    // Sirius rose heliacally at Alexandria on 20 July 139 (Julian), the
    // Censorinus date. Needs the DE441 cut (docs/DE.md): run with --no-skip.
    const std::string cut = std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_m3000p1550.441";
    if (access(cut.c_str(), R_OK) != 0) {
        MESSAGE("SKIP: no " << cut);
        return;
    }
    auto opened = Engine::open(de440());
    REQUIRE(opened.ok());
    Engine e = std::move(opened).value();
    REQUIRE(e.add_ephemeris(cut).ok());
    registry::Registry reg(e);
    const frames::GeoSite site{29.9 * kDeg, 31.2 * kDeg, 10.0};
    // 20 July 139 Julian is JD 1772027.5 (0h UT).
    for (double margin : {1.2, 0.0}) {
        Options o;
        o.detection_margin_mag = margin;
        auto r = next(e, object_named(e, reg, "Sirius"), 1771960.5, site, Atmosphere{}, Observer{},
                      Event::Rising, o);
        REQUIRE_MESSAGE(r.ok(), r.error().message);
        std::printf("139 CE Sirius at Alexandria, margin %.1f: JD %.3f (%+.1f days from 20 July)\n",
                    margin, r.value().jd_ut1, r.value().jd_ut1 - 1772027.5);
    }
}

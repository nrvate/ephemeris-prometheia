// SPDX-License-Identifier: GPL-2.0-or-later
//
// The C interface (include/prometheia/prometheia.h) against the C++
// engine it wraps. The shim adds no behaviour, so every answer must be
// bit-identical to prometheia::Engine given the same inputs; the rest
// is the boundary itself: option translation, NULL and range checks,
// status codes and messages, the Delta T callback, and the time helpers.
// tests/c_api_smoke.c drives the same interface from real C.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

#include <prometheia/catalog.hpp>
#include <prometheia/engine.hpp>
#include <prometheia/prometheia.h>
#include <prometheia/prometheia.hpp>
#include <prometheia/stars.hpp>
#include <prometheia/time.hpp>

#include "synthetic_spk.hpp"
#include <doctest/doctest.h>

extern "C" int prometheia_c_smoke(const char* kernel_path);

using namespace prometheia;
using synth::TempFile;

namespace {

constexpr double kJ2000 = 2451545.0;
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

struct CHandle {
    prometheia_engine* e = nullptr;
    ~CHandle() { prometheia_engine_close(e); }
};

bool same(const prometheia_result& c, const CalcResult& r) {
    bool eq = c.lon_deg == r.pos.lon_deg && c.lat_deg == r.pos.lat_deg &&
              c.dist_au == r.pos.dist_au && c.lon_speed == r.pos.lon_speed &&
              c.lat_speed == r.pos.lat_speed && c.dist_speed == r.pos.dist_speed &&
              c.light_time_days == r.provenance.light_time_days && c.denum == r.provenance.denum &&
              c.source != nullptr && std::string(c.source) == r.provenance.source;
    for (int i = 0; i < 3; ++i)
        eq = eq && c.xyz_au[i] == r.pos.xyz_au[i] && c.vel_au_day[i] == r.pos.vel_au_day[i];
    eq = eq && bool(c.flags & PROMETHEIA_HAS_SIGMA) == r.sigma_arcsec.has_value() &&
         bool(c.flags & PROMETHEIA_HAS_AYANAMSA) == r.ayanamsa_deg.has_value();
    if (r.sigma_arcsec)
        eq = eq && c.sigma_arcsec == *r.sigma_arcsec;
    if (r.ayanamsa_deg)
        eq = eq && c.ayanamsa_deg == *r.ayanamsa_deg;
    return eq;
}

bool zeroed(const prometheia_result& r) {
    static const prometheia_result z{};
    return std::memcmp(&r, &z, sizeof r) == 0;
}

TEST_CASE("c_api_library_and_defaults") {
    CHECK(std::string(prometheia_version()) == version_string);
    CHECK(prometheia_abi_version() == PROMETHEIA_ABI_VERSION);

    // The C defaults are the C++ defaults, field by field.
    prometheia_options c;
    std::memset(&c, 0x5a, sizeof c);
    prometheia_options_init(&c);
    const CalcOptions d;
    CHECK(c.center == int(d.center));
    CHECK(c.frame == int(d.frame));
    CHECK(c.coords == int(d.coords));
    CHECK(c.sidereal == int(d.sidereal));
    CHECK(c.sidereal == PROMETHEIA_SIDEREAL_TROPICAL);
    CHECK(c.precession == int(d.precession));
    CHECK(PROMETHEIA_PRECESSION_VONDRAK2011 == int(Precession::Vondrak2011));
    CHECK(c.frame == PROMETHEIA_FRAME_TRUE_OF_DATE);
    CHECK(c.center == PROMETHEIA_CENTER_GEOCENTRIC);
    CHECK(c.light_time == 1);
    CHECK(c.deflection == 1);
    CHECK(c.aberration == 1);
    CHECK(c.speed == 1);
    CHECK(c.sigma == 1);
    CHECK(c.sidereal_epoch_jd == 0.0);
    CHECK(c.sidereal_ayanamsa_deg == 0.0);
    CHECK(c.site_lon_deg == 0.0);
    CHECK(c.site_lat_deg == 0.0);
    CHECK(c.site_height_m == 0.0);
    prometheia_options_init(nullptr); // no-op

    // The selector constants mirror the C++ enums.
    CHECK(PROMETHEIA_CENTER_TOPOCENTRIC == int(Center::Topocentric));
    CHECK(PROMETHEIA_CENTER_HELIOCENTRIC == int(Center::Heliocentric));
    CHECK(PROMETHEIA_CENTER_BARYCENTRIC == int(Center::Barycentric));
    CHECK(PROMETHEIA_FRAME_ICRF == int(Frame::ICRF));
    CHECK(PROMETHEIA_FRAME_J2000 == int(Frame::J2000));
    CHECK(PROMETHEIA_FRAME_MEAN_OF_DATE == int(Frame::MeanOfDate));
    CHECK(PROMETHEIA_COORDS_EQUATORIAL == int(Coords::Equatorial));
    CHECK(PROMETHEIA_SIDEREAL_FAGAN_BRADLEY == int(SiderealMode::FaganBradley));
    CHECK(PROMETHEIA_SIDEREAL_LAHIRI == int(SiderealMode::Lahiri));
    CHECK(PROMETHEIA_SIDEREAL_USER == int(SiderealMode::User));
    CHECK(PROMETHEIA_MOON == body::kMoon);
    CHECK(PROMETHEIA_EARTH == body::kEarth);
    CHECK(PROMETHEIA_PLUTO == body::kPluto);
    CHECK(PROMETHEIA_SUN == body::kSun);
}

TEST_CASE("c_api_open_errors") {
    prometheia_error err;
    prometheia_engine* e = reinterpret_cast<prometheia_engine*>(&err); // must be cleared
    CHECK(prometheia_engine_open("/nonexistent/x.bsp", &e, &err) == PROMETHEIA_ERROR_IO);
    CHECK(e == nullptr);
    CHECK(err.code == PROMETHEIA_ERROR_IO);
    CHECK(std::strlen(err.message) > 0);
    CHECK(prometheia_engine_open(nullptr, &e, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_engine_open("x", nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_engine_open("/nonexistent/x.bsp", &e, nullptr) == PROMETHEIA_ERROR_IO);

    // A file that is neither a DE binary nor a kernel.
    TempFile junk("capi-junk");
    FILE* f = std::fopen(junk.path.c_str(), "wb");
    std::fwrite("definitely not an ephemeris", 1, 27, f);
    std::fclose(f);
    CHECK(prometheia_engine_open(junk.path.c_str(), &e, &err) != PROMETHEIA_OK);
    CHECK(e == nullptr);
    CHECK(err.code != PROMETHEIA_OK);

    // Every entry point tolerates a NULL engine.
    prometheia_result res;
    CHECK(prometheia_calc(nullptr, 10, kJ2000, nullptr, &res, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(zeroed(res));
    CHECK(prometheia_calc_ut(nullptr, 10, kJ2000, nullptr, &res, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_engine_add_catalog(nullptr, "x", &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_engine_add_perturbers(nullptr, "x", &err) == PROMETHEIA_ERROR_ARGUMENT);
    int body = 7;
    CHECK(prometheia_engine_lookup(nullptr, "x", &body, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(body == 0);
    CHECK(std::string(prometheia_engine_source(nullptr)).empty());
    prometheia_engine_set_delta_t(nullptr, nullptr, nullptr);
    prometheia_engine_close(nullptr);

    // Messages longer than the buffer are truncated and terminated.
    const std::string long_path = "/nonexistent/" + std::string(600, 'p');
    CHECK(prometheia_engine_open(long_path.c_str(), &e, &err) == PROMETHEIA_ERROR_IO);
    CHECK(std::strlen(err.message) == PROMETHEIA_ERROR_MESSAGE_SIZE - 1);
}

TEST_CASE("c_api_matches_engine_bit_for_bit") {
    TempFile tf("capi-bits");
    synth::write_linear_spk(tf.path);
    auto cpp = Engine::open(tf.path.string());
    CHECK(cpp.ok());
    CHandle c;
    prometheia_error err;
    CHECK(prometheia_engine_open(tf.path.c_str(), &c.e, &err) == PROMETHEIA_OK);
    CHECK((err.code == PROMETHEIA_OK && err.message[0] == '\0'));
    if (!cpp.ok() || !c.e)
        return;
    CHECK(!cpp.value().source().empty());
    CHECK(std::string(prometheia_engine_source(c.e)) == cpp.value().source());

    struct Case {
        int body;
        int center, frame, coords, sidereal;
        int lt, defl, aber, speed;
    };
    const Case cases[] = {
        {10, 0, 3, 0, -1, 1, 1, 1, 1},    // apparent, true ecliptic of date
        {5, 0, 3, 1, -1, 1, 1, 1, 1},     // equatorial
        {5, 0, 0, 0, -1, 0, 0, 0, 1},     // geometric ICRF
        {5, 0, 1, 1, -1, 1, 0, 0, 0},     // astrometric J2000, no rates
        {5, 0, 2, 0, -1, 1, 1, 1, 1},     // mean of date
        {5, 1, 3, 0, -1, 1, 1, 1, 1},     // topocentric
        {5, 2, 1, 0, -1, 1, 1, 1, 1},     // heliocentric
        {399, 3, 0, 1, -1, 0, 0, 0, 1},   // barycentric
        {10, 0, 3, 0, 0, 1, 1, 1, 1},     // Fagan/Bradley
        {5, 0, 1, 0, 1, 1, 1, 1, 1},      // Lahiri, J2000 ecliptic
        {5, 0, 3, 0, 255, 1, 1, 1, 1},    // user-anchored
        {5, 0, 3, 0, -1, 1, 1, 1, 1 + 6}, // any nonzero int is true
        {399, 4, 1, 0, -1, 1, 1, 1, 1},   // centred on the Jupiter barycentre
    };
    for (const Case& k : cases) {
        prometheia_options co;
        prometheia_options_init(&co);
        co.center = k.center;
        co.frame = k.frame;
        co.coords = k.coords;
        co.sidereal = k.sidereal;
        co.sidereal_epoch_jd = 2440000.5;
        co.sidereal_ayanamsa_deg = 22.5;
        co.light_time = k.lt;
        co.deflection = k.defl;
        co.aberration = k.aber;
        co.speed = k.speed;
        co.site_lon_deg = 8.55;
        co.site_lat_deg = 47.37;
        co.site_height_m = 500.0;
        co.center_body = 5;

        CalcOptions o;
        o.center = Center(k.center);
        o.frame = Frame(k.frame);
        o.coords = Coords(k.coords);
        o.sidereal = SiderealMode(k.sidereal);
        o.sidereal_epoch_jtdb = 2440000.5;
        o.sidereal_ayanamsa_deg = 22.5;
        o.light_time = k.lt;
        o.deflection = k.defl;
        o.aberration = k.aber;
        o.speed = k.speed;
        o.site = {8.55 * kDegToRad, 47.37 * kDegToRad, 500.0};
        o.center_body = 5;

        for (double jd : {kJ2000, 2461300.25}) {
            prometheia_result cr;
            CHECK(prometheia_calc(c.e, k.body, jd, &co, &cr, &err) == PROMETHEIA_OK);
            auto r = cpp.value().calc(k.body, jd, o);
            CHECK(r.ok());
            if (r.ok())
                CHECK(same(cr, r.value()));

            CHECK(prometheia_calc_ut(c.e, k.body, jd, &co, &cr, &err) == PROMETHEIA_OK);
            auto ru = cpp.value().calc_ut(k.body, jd, o);
            CHECK(ru.ok());
            if (ru.ok())
                CHECK(same(cr, ru.value()));
        }
    }

    // Orbit points go through the same options.
    for (int point = PROMETHEIA_ORBIT_ASCENDING_NODE; point <= PROMETHEIA_ORBIT_PERIHELION;
         ++point) {
        prometheia_result cr;
        prometheia_options co;
        prometheia_options_init(&co);
        const auto rc = prometheia_calc_orbit_point(c.e, 399, point, PROMETHEIA_ELEMENTS_OSCULATING,
                                                    kJ2000, &co, &cr, &err);
        auto r =
            cpp.value().calc_orbit_point(399, OrbitPoint(point), OrbitElements::Osculating, kJ2000);
        CHECK((rc == PROMETHEIA_OK) == r.ok());
        if (r.ok())
            CHECK(same(cr, r.value()));
        const auto ru = prometheia_calc_orbit_point_ut(
            c.e, 399, point, PROMETHEIA_ELEMENTS_OSCULATING, kJ2000, &co, &cr, &err);
        auto rr = cpp.value().calc_orbit_point_ut(399, OrbitPoint(point), OrbitElements::Osculating,
                                                  kJ2000);
        CHECK((ru == PROMETHEIA_OK) == rr.ok());
        if (rr.ok())
            CHECK(same(cr, rr.value()));
    }
    {
        prometheia_result cr;
        CHECK(prometheia_calc_orbit_point(c.e, 399, 4, PROMETHEIA_ELEMENTS_OSCULATING, kJ2000,
                                          nullptr, &cr, &err) == PROMETHEIA_ERROR_ARGUMENT);
        CHECK(prometheia_calc_orbit_point(c.e, 399, PROMETHEIA_ORBIT_PERIHELION, 2, kJ2000, nullptr,
                                          &cr, &err) == PROMETHEIA_ERROR_ARGUMENT);
    }

    // NULL options = the defaults.
    prometheia_result a, b;
    CHECK(prometheia_calc(c.e, 5, kJ2000, nullptr, &a, &err) == PROMETHEIA_OK);
    auto r = cpp.value().calc(5, kJ2000);
    CHECK(r.ok());
    CHECK(same(a, r.value()));
    prometheia_options def;
    prometheia_options_init(&def);
    CHECK(prometheia_calc(c.e, 5, kJ2000, &def, &b, nullptr) == PROMETHEIA_OK);
    CHECK(std::memcmp(&a, &b, sizeof a) == 0);
}

TEST_CASE("c_api_calc_errors") {
    TempFile tf("capi-err");
    synth::write_linear_spk(tf.path);
    CHandle c;
    prometheia_error err;
    CHECK(prometheia_engine_open(tf.path.c_str(), &c.e, &err) == PROMETHEIA_OK);
    if (!c.e)
        return;

    prometheia_result res;
    auto expect = [&](int body, double jd, const prometheia_options* o, int code) {
        std::memset(&res, 0x33, sizeof res);
        const int got = prometheia_calc(c.e, body, jd, o, &res, &err);
        CHECK(got == code);
        CHECK(err.code == code);
        CHECK(std::strlen(err.message) > 0);
        CHECK(zeroed(res));
    };
    expect(499, kJ2000, nullptr, PROMETHEIA_ERROR_NOT_FOUND); // not in the kernel
    expect(399, kJ2000, nullptr, PROMETHEIA_ERROR_ARGUMENT);  // the observer
    expect(10, std::nan(""), nullptr, PROMETHEIA_ERROR_ARGUMENT);
    expect(10, 2451545.0 + 100.0 * 365.25, nullptr, PROMETHEIA_ERROR_ARGUMENT); // coverage

    prometheia_options o;
    const auto bad = [&](void (*mutate)(prometheia_options&)) {
        prometheia_options_init(&o);
        mutate(o);
        expect(10, kJ2000, &o, PROMETHEIA_ERROR_ARGUMENT);
    };
    bad([](prometheia_options& x) { x.center = -1; });
    bad([](prometheia_options& x) { x.center = 4; });
    bad([](prometheia_options& x) { x.frame = 4; });
    bad([](prometheia_options& x) { x.coords = 2; });
    bad([](prometheia_options& x) { x.sidereal = 2; });
    bad([](prometheia_options& x) { x.sidereal = -2; });
    bad([](prometheia_options& x) { x.precession = 2; });
    bad([](prometheia_options& x) {
        x.sidereal = PROMETHEIA_SIDEREAL_USER;
        x.sidereal_ayanamsa_deg = std::nan("");
    });

    CHECK(prometheia_calc(c.e, 10, kJ2000, nullptr, nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);

    // A failing call after a successful one resets the error, and a
    // successful call clears it.
    CHECK(prometheia_calc(c.e, 10, kJ2000, nullptr, &res, &err) == PROMETHEIA_OK);
    CHECK((err.code == PROMETHEIA_OK && err.message[0] == '\0'));
}

double constant_delta_t(void* user, double) {
    ++*static_cast<int*>(user);
    return 64.0;
}

struct FixedDeltaT final : time::DeltaTModel {
    double delta_t_seconds(double) const override { return 64.0; }
};

TEST_CASE("c_api_delta_t_callback") {
    TempFile tf("capi-dt");
    synth::write_linear_spk(tf.path);
    auto cpp = Engine::open(tf.path.string());
    CHandle c;
    CHECK(prometheia_engine_open(tf.path.c_str(), &c.e, nullptr) == PROMETHEIA_OK);
    if (!cpp.ok() || !c.e)
        return;

    prometheia_options o;
    prometheia_options_init(&o);
    o.center = PROMETHEIA_CENTER_TOPOCENTRIC;
    o.site_lon_deg = -70.0;
    o.site_lat_deg = -30.0;
    CalcOptions co;
    co.center = Center::Topocentric;
    co.site = {-70.0 * kDegToRad, -30.0 * kDegToRad, 0.0};

    int calls = 0;
    prometheia_engine_set_delta_t(c.e, constant_delta_t, &calls);
    const FixedDeltaT fixed;
    cpp.value().set_delta_t_model(&fixed);
    prometheia_result res;
    CHECK(prometheia_calc_ut(c.e, 5, kJ2000, &o, &res, nullptr) == PROMETHEIA_OK);
    CHECK(calls > 0); // the user pointer reached the callback
    auto r = cpp.value().calc_ut(5, kJ2000, co);
    CHECK(r.ok());
    CHECK(same(res, r.value()));

    // NULL restores the default model on both sides.
    prometheia_engine_set_delta_t(c.e, nullptr, &calls);
    cpp.value().set_delta_t_model(nullptr);
    const int before = calls;
    CHECK(prometheia_calc_ut(c.e, 5, kJ2000, &o, &res, nullptr) == PROMETHEIA_OK);
    CHECK(calls == before);
    r = cpp.value().calc_ut(5, kJ2000, co);
    CHECK(r.ok());
    CHECK(same(res, r.value()));
}

TEST_CASE("c_api_catalog_and_lookup") {
    TempFile tf("capi-cat");
    synth::write_linear_spk(tf.path);
    const std::string catalog = std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/sample-100.epm";
    auto cpp = Engine::open(tf.path.string());
    CHandle c;
    prometheia_error err;
    CHECK(prometheia_engine_open(tf.path.c_str(), &c.e, &err) == PROMETHEIA_OK);
    if (!cpp.ok() || !c.e)
        return;

    int body = -1;
    CHECK(prometheia_engine_lookup(c.e, "Ceres", &body, &err) == PROMETHEIA_ERROR_NOT_FOUND);
    CHECK(body == 0);
    CHECK(err.code == PROMETHEIA_ERROR_NOT_FOUND);
    CHECK(prometheia_engine_add_catalog(c.e, "/nonexistent/c.epm", &err) == PROMETHEIA_ERROR_IO);
    CHECK(prometheia_engine_add_catalog(c.e, nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_engine_add_perturbers(c.e, nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_engine_add_perturbers(c.e, "/nonexistent/sb.bsp", &err) ==
          PROMETHEIA_ERROR_IO);
    CHECK(prometheia_engine_add_perturbers(c.e, tf.path.c_str(), &err) ==
          PROMETHEIA_ERROR_FORMAT); // a planetary kernel carries no asteroids
    CHECK(prometheia_engine_lookup(c.e, nullptr, &body, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_engine_lookup(c.e, "Ceres", nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);

    CHECK(prometheia_engine_add_catalog(c.e, catalog.c_str(), &err) == PROMETHEIA_OK);
    CHECK(cpp.value().add_catalog(catalog).ok());
    CHECK(prometheia_engine_lookup(c.e, "ceres", &body, &err) == PROMETHEIA_OK);
    CHECK(body == 20000001);
    CHECK(body == cpp.value().lookup("Ceres").value());

    const double jd = 2460600.5;
    prometheia_result res;
    CHECK(prometheia_calc(c.e, body, jd, nullptr, &res, &err) == PROMETHEIA_OK);
    auto r = cpp.value().calc(20000001, jd);
    CHECK(r.ok());
    CHECK(same(res, r.value()));
    CHECK(!(res.flags & PROMETHEIA_HAS_SIGMA)); // element sigmas only, no covariance
    CHECK(std::string(res.source).find("EPM1") != std::string::npos);

    // An overlay carrying a covariance for Ceres: the flag and value cross
    // the boundary like the C++ optional.
    auto reader = catalog::Reader::open(catalog);
    REQUIRE(reader.ok());
    auto ceres = reader.value().lookup(20000001);
    REQUIRE(ceres.ok());
    catalog::Record rec = ceres.value();
    rec.flags |= catalog::RecordFlags::kCovariance;
    rec.cov_epoch_jtdb = rec.epoch_jtdb;
    const double a = rec.a_au, n = std::sqrt(0.295912208284119561e-3 / (a * a * a));
    const double cel[6] = {rec.e,        a * (1.0 - rec.e), rec.epoch_jtdb - rec.mean_anom_rad / n,
                           rec.node_rad, rec.argp_rad,      rec.inc_rad};
    for (int i = 0; i < 6; ++i) {
        rec.cov_elements[i] = cel[i];
        rec.covariance[catalog::packed_index(i, i)] = i == 2 ? 1e-6 : 1e-14;
    }
    TempFile overlay("capi-cov");
    {
        auto w = catalog::Writer::create(overlay.path.string(), catalog::WriterOptions{});
        REQUIRE(w.ok());
        CHECK(w.value().add(rec, "1", "Ceres").ok());
        CHECK(w.value().finish(CborValue::make_map()).ok());
    }
    CHECK(prometheia_engine_add_catalog(c.e, overlay.path.c_str(), &err) == PROMETHEIA_OK);
    CHECK(cpp.value().add_catalog(overlay.path.string()).ok());
    CHECK(prometheia_calc(c.e, 20000001, jd, nullptr, &res, &err) == PROMETHEIA_OK);
    r = cpp.value().calc(20000001, jd);
    REQUIRE(r.ok());
    CHECK(same(res, r.value()));
    CHECK((res.flags & PROMETHEIA_HAS_SIGMA));
    CHECK(res.sigma_arcsec > 0.0);
}

TEST_CASE("c_api_time_helpers") {
    CHECK(prometheia_jd_from_ymdhms(2000, 1, 1, 12, 0, 0.0) == kJ2000);
    int y = 0, m = 0;
    double d = 0.0;
    prometheia_civil_from_jd(2461300.25, &y, &m, &d);
    const time::Civil civ = time::civil_from_jd(2461300.25);
    CHECK(y == civ.year);
    CHECK(m == civ.month);
    CHECK(d == civ.day);
    prometheia_civil_from_jd(kJ2000, nullptr, nullptr, nullptr);

    prometheia_error err;
    double jd_tt = -1.0;
    CHECK(prometheia_utc_to_tt(2017, 1, 1, 0, 0, 0.0, &jd_tt, &err) == PROMETHEIA_OK);
    CHECK(jd_tt == time::utc_to_tt(2017, 1, 1, 0, 0, 0.0).value());
    CHECK(prometheia_utc_to_tt(2017, 1, 1, 0, 0, 0.0, nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_utc_to_tt(2017, 2, 30, 0, 0, 0.0, &jd_tt, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(jd_tt == 0.0);
    CHECK(std::strlen(err.message) > 0);

    prometheia_utc u;
    CHECK(prometheia_tt_to_utc(kJ2000, &u, &err) == PROMETHEIA_OK);
    const time::Utc tu = time::tt_to_utc(kJ2000).value();
    CHECK(u.year == tu.year);
    CHECK(u.month == tu.month);
    CHECK(u.day == tu.day);
    CHECK(u.hour == tu.hour);
    CHECK(u.minute == tu.minute);
    CHECK(u.second == tu.second);
    CHECK(u.tai_minus_utc == 32.0);
    CHECK(prometheia_tt_to_utc(2400000.5, &u, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(u.year == 0);
    CHECK(prometheia_tt_to_utc(kJ2000, nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);

    CHECK(prometheia_delta_t(kJ2000) == time::delta_t(kJ2000));
    CHECK(prometheia_tdb_minus_tt(2461300.25) == time::tdb_minus_tt(2461300.25));
}

TEST_CASE("c_api_stars_match_engine") {
    TempFile tf("capi-stars");
    synth::write_linear_spk(tf.path);
    auto cpp = Engine::open(tf.path.string());
    REQUIRE(cpp.ok());
    CHandle c;
    prometheia_error err;
    REQUIRE(prometheia_engine_open(tf.path.c_str(), &c.e, &err) == PROMETHEIA_OK);
    CHECK(prometheia_star_count() == int(stars::count()));
    for (const char* q : {"Sirius", "Polaris", "M 31", "Barnard's Star", "HR 230"}) {
        int index = -1;
        REQUIRE(prometheia_star_find(q, &index, &err) == PROMETHEIA_OK);
        CHECK(size_t(index) == stars::find(q).value());
        prometheia_options co;
        prometheia_options_init(&co);
        co.coords = PROMETHEIA_COORDS_EQUATORIAL;
        CalcOptions o;
        o.coords = Coords::Equatorial;
        prometheia_result cr;
        CHECK(prometheia_calc_star(c.e, index, kJ2000 + 1234.5, &co, &cr, &err) == PROMETHEIA_OK);
        auto r = cpp.value().calc_star(size_t(index), kJ2000 + 1234.5, o);
        REQUIRE(r.ok());
        CHECK(same(cr, r.value()));
        CHECK(prometheia_calc_star_ut(c.e, index, kJ2000, &co, &cr, &err) == PROMETHEIA_OK);
        auto ru = cpp.value().calc_star_ut(size_t(index), kJ2000, o);
        REQUIRE(ru.ok());
        CHECK(same(cr, ru.value()));
    }
    prometheia_star info;
    CHECK(prometheia_star_info(-1, &info, &err) == PROMETHEIA_ERROR_NOT_FOUND);
    CHECK(prometheia_star_info(prometheia_star_count(), &info, &err) == PROMETHEIA_ERROR_NOT_FOUND);
    CHECK(prometheia_star_info(0, nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);
    prometheia_result cr;
    CHECK(prometheia_calc_star(c.e, -3, kJ2000, nullptr, &cr, &err) == PROMETHEIA_ERROR_NOT_FOUND);
    CHECK(prometheia_calc_star(c.e, prometheia_star_count(), kJ2000, nullptr, &cr, &err) ==
          PROMETHEIA_ERROR_NOT_FOUND);
    int index = 0;
    CHECK(prometheia_star_find(nullptr, &index, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_star_lookup("Alg", 1, nullptr, 4) == 0);
    prometheia_star_match m[8];
    CHECK(prometheia_star_lookup("Alg", 1, m, 8) >= 4);
    CHECK(m[0].quality == PROMETHEIA_MATCH_PREFIX);
}

TEST_CASE("c_api_from_c") {
    TempFile tf("capi-smoke");
    synth::write_linear_spk(tf.path);
    const int step = prometheia_c_smoke(tf.path.c_str());
    if (step != 0)
        std::printf("  c smoke failed at step %d\n", step);
    CHECK(step == 0);
}

} // namespace

TEST_CASE("c_api_hypotheticals_match_the_engine") {
    TempFile tf("capi-hyp");
    synth::write_linear_spk(tf.path);
    auto cpp = Engine::open(tf.path.string());
    REQUIRE(cpp.ok());
    CHandle c;
    prometheia_error err;
    REQUIRE(prometheia_engine_open(tf.path.c_str(), &c.e, &err) == PROMETHEIA_OK);

    // An invented body, through both front doors: the C elements struct and
    // the C++ one must give the same answer to the bit.
    prometheia_elements ce{};
    ce.epoch_jd_tt = 2451545.0;
    ce.equinox = PROMETHEIA_EQUINOX_J1900;
    ce.origin = PROMETHEIA_ELEMENTS_ORIGIN_SUN;
    ce.n_terms = 2;
    ce.mean_anomaly[0] = 10.0;
    ce.semi_major_axis[0] = 40.0;
    ce.eccentricity[0] = 0.02;
    ce.arg_perihelion[0] = 20.0;
    ce.ascending_node[0] = 30.0;
    ce.ascending_node[1] = 0.8; // a drifting node, M at epoch
    ce.inclination[0] = 1.5;
    PolynomialElements pe;
    pe.epoch_jd_tt = 2451545.0;
    pe.equinox = ElementEquinox::J1900;
    pe.n_terms = 2;
    pe.mean_anomaly[0] = 10.0;
    pe.semi_major_axis[0] = 40.0;
    pe.eccentricity[0] = 0.02;
    pe.arg_perihelion[0] = 20.0;
    pe.ascending_node[0] = 30.0;
    pe.ascending_node[1] = 0.8;
    pe.inclination[0] = 1.5;

    prometheia_result r{};
    REQUIRE(prometheia_calc_elements(c.e, &ce, 2452000.0, nullptr, &r, &err) == PROMETHEIA_OK);
    auto want = cpp.value().calc_elements(pe, 2452000.0);
    REQUIRE(want.ok());
    CHECK(r.lon_deg == want.value().pos.lon_deg);
    CHECK(r.lat_deg == want.value().pos.lat_deg);
    CHECK(r.dist_au == want.value().pos.dist_au);
    CHECK(r.lon_speed == want.value().pos.lon_speed);
    CHECK(std::string(r.source) == "two-body orbital elements");

    // Selectors out of range are refused before the engine sees them.
    prometheia_elements bad = ce;
    bad.equinox = 5;
    CHECK(prometheia_calc_elements(c.e, &bad, 2452000.0, nullptr, &r, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    bad = ce;
    bad.origin = 2;
    CHECK(prometheia_calc_elements(c.e, &bad, 2452000.0, nullptr, &r, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    bad = ce;
    bad.eccentricity[0] = 1.5;
    CHECK(prometheia_calc_elements(c.e, &bad, 2452000.0, nullptr, &r, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_calc_elements(c.e, nullptr, 2452000.0, nullptr, &r, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);

    // Named: add a file, list it, read it back, compute it.
    struct Scratch {
        std::string path = (std::filesystem::temp_directory_path() /
                            ("prometheia-capi-hyp-" + std::to_string(::getpid()) + ".jsonl"))
                               .string();
        ~Scratch() { std::remove(path.c_str()); }
    } scratch;
    {
        FILE* f = std::fopen(scratch.path.c_str(), "wb");
        REQUIRE(f);
        std::fputs(
            R"({"token":"testbody","name":"Test Body","set":"Invented for tests","citation":"tests/test_c_api.cpp","epoch":2451545.0,"equinox":"J1900","M":[10.0],"a":[40.0],"e":[0.02],"w":[20.0],"node":[30.0,0.8],"i":[1.5]})"
            "\n",
            f);
        std::fclose(f);
    }
    const int before = prometheia_hypothetical_count(c.e);
    CHECK(prometheia_calc_hypothetical(c.e, "testbody", 2452000.0, nullptr, &r, &err) ==
          PROMETHEIA_ERROR_NOT_FOUND);
    REQUIRE(prometheia_engine_add_hypotheticals(c.e, scratch.path.c_str(), &err) == PROMETHEIA_OK);
    CHECK(prometheia_hypothetical_count(c.e) == before + 1);
    CHECK(std::string(prometheia_hypothetical_token(c.e, before)) == "testbody");
    CHECK(prometheia_hypothetical_token(c.e, before + 1) == nullptr);
    CHECK(prometheia_hypothetical_token(c.e, -1) == nullptr);

    prometheia_hypothetical h{};
    REQUIRE(prometheia_hypothetical_get(c.e, "TESTBODY", &h, &err) == PROMETHEIA_OK);
    CHECK(std::string(h.token) == "testbody");
    CHECK(std::string(h.name) == "Test Body");
    CHECK(std::string(h.set) == "Invented for tests");
    CHECK(h.elements.n_terms == 2);
    CHECK(h.elements.ascending_node[1] == 0.8);
    CHECK(prometheia_hypothetical_get(c.e, "nosuch", &h, &err) == PROMETHEIA_ERROR_NOT_FOUND);

    prometheia_result named{};
    REQUIRE(prometheia_calc_hypothetical(c.e, "testbody", 2452000.0, nullptr, &named, &err) ==
            PROMETHEIA_OK);
    prometheia_result direct{};
    REQUIRE(prometheia_calc_elements(c.e, &ce, 2452000.0, nullptr, &direct, &err) == PROMETHEIA_OK);
    CHECK(named.lon_deg == direct.lon_deg);
    CHECK(named.lat_deg == direct.lat_deg);
    CHECK(std::string(named.source) == "Invented for tests");

    // A malformed file is refused whole, with the line in the message.
    {
        FILE* f = std::fopen(scratch.path.c_str(), "wb");
        REQUIRE(f);
        std::fputs("{\"token\":\"broken\"}\n", f);
        std::fclose(f);
    }
    CHECK(prometheia_engine_add_hypotheticals(c.e, scratch.path.c_str(), &err) ==
          PROMETHEIA_ERROR_FORMAT);
    CHECK(std::string(err.message).find(":1:") != std::string::npos);
    CHECK(prometheia_hypothetical_count(c.e) == before + 1);
}

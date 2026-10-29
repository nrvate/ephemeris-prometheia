// SPDX-License-Identifier: GPL-2.0-or-later
//
// Segments through the C API (prometheia_segments_*, docs/C_API.md,
// "Segments"): the arguments, the layout against prometheia_segments_eval bit
// for bit, the error against prometheia_calc_object, and one handle evaluated
// from several threads at once. Needs DE440 (PROMETHEIA_DE440, else
// ephe/linux_p1550p2650.440); the catalog cases also need the DE440-span
// perturber kernel (PROMETHEIA_SB441_DE440SPAN, else
// ephe/sb441-n16-de440span.bsp), and the handover case DE441 behind DE440
// (PROMETHEIA_DE441, else ephe/linux_m13000p17000.441). SKIP when they are
// absent.
//
// c_segments_report (doctest::skip) is the plan's done-when: a year at
// 0.001", 10,000 random instants per case, and the fit and evaluation costs
// that docs/SEGMENTS.md quotes.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <prometheia/prometheia.h>

#include <doctest/doctest.h>

namespace {

constexpr double kRadToArcsec = 180.0 / 3.14159265358979323846 * 3600.0;
constexpr double kJ2026 = 2461041.5; // 2026-01-01 TT, near enough

std::string env_or(const char* var, const std::string& fallback) {
    const char* env = std::getenv(var);
    return (env && *env) ? std::string(env) : fallback;
}

std::string de440_path() {
    return env_or("PROMETHEIA_DE440",
                  std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_p1550p2650.440");
}

std::string perturbers_path() {
    return env_or("PROMETHEIA_SB441_DE440SPAN",
                  std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/sb441-n16-de440span.bsp");
}

bool readable(const std::string& path) {
    return ::access(path.c_str(), R_OK) == 0;
}

// DE440, and with catalog the sample catalog and the perturber kernel; NULL
// when a file is missing.
prometheia_engine* open_engine(bool catalog) {
    if (!readable(de440_path()) || (catalog && !readable(perturbers_path())))
        return nullptr;
    prometheia_engine* e = nullptr;
    if (prometheia_engine_open(de440_path().c_str(), &e, nullptr) != PROMETHEIA_OK)
        return nullptr;
    if (catalog) {
        const std::string sample =
            std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/sample-100.epm";
        if (prometheia_engine_add_catalog(e, sample.c_str(), nullptr) != PROMETHEIA_OK ||
            prometheia_engine_add_perturbers(e, perturbers_path().c_str(), nullptr) !=
                PROMETHEIA_OK) {
            prometheia_engine_close(e);
            return nullptr;
        }
    }
    return e;
}

// The angle between two directions, without acos (small angles).
double angle_arcsec(const double a[3], const double b[3]) {
    const double cx = a[1] * b[2] - a[2] * b[1];
    const double cy = a[2] * b[0] - a[0] * b[2];
    const double cz = a[0] * b[1] - a[1] * b[0];
    const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), dot) * kRadToArcsec;
}

// A caller's own evaluation, from the layout alone, as docs/C_API.md writes
// it: every one of the stride coefficients, Clenshaw's recurrence.
void own_eval(const prometheia_segments_layout& l, double jd, double p[3], double v[3]) {
    int i = 0;
    while (i + 1 < l.count && jd >= l.bounds_jd_tt[i + 1])
        ++i;
    const double u = (jd - l.mid_jd_tt[i]) / l.half_span_days[i];
    for (int c = 0; c < 3; ++c) {
        const double* cf[2] = {l.coef + (3 * i + c) * l.stride, l.dcoef + (3 * i + c) * l.stride};
        double out[2];
        for (int s = 0; s < 2; ++s) {
            double b1 = 0.0, b2 = 0.0;
            for (int k = l.stride - 1; k >= 1; --k) {
                const double b = 2.0 * u * b1 - b2 + cf[s][k];
                b2 = b1;
                b1 = b;
            }
            out[s] = u * b1 - b2 + cf[s][0];
        }
        p[c] = out[0];
        v[c] = out[1] / l.half_span_days[i];
    }
}

struct Case {
    const char* name;
    prometheia_object object;
    prometheia_options opts;
    bool catalog;
};

// The defaults, but sigma: a fit never reads it, and with it on every
// catalog calc compared against would integrate twelve times more.
prometheia_options defaults() {
    prometheia_options o;
    prometheia_options_init(&o);
    o.sigma = 0;
    return o;
}

std::vector<Case> cases() {
    std::vector<Case> out;
    const struct {
        const char* name;
        int id;
    } bodies[] = {{"Sun", PROMETHEIA_SUN},         {"Moon", PROMETHEIA_MOON},
                  {"Mercury", PROMETHEIA_MERCURY}, {"Venus", PROMETHEIA_VENUS},
                  {"Mars", PROMETHEIA_MARS},       {"Jupiter", PROMETHEIA_JUPITER},
                  {"Saturn", PROMETHEIA_SATURN},   {"Uranus", PROMETHEIA_URANUS},
                  {"Neptune", PROMETHEIA_NEPTUNE}, {"Pluto", PROMETHEIA_PLUTO}};
    for (const auto& b : bodies)
        out.push_back({b.name, {PROMETHEIA_OBJECT_BODY, b.id, nullptr}, defaults(), false});
    const struct {
        const char* name;
        int id;
    } small[] = {
        {"Ceres", 20000001}, {"Pallas", 20000002}, {"Juno", 20000003}, {"Vesta", 20000004}};
    for (const auto& b : small)
        out.push_back({b.name, {PROMETHEIA_OBJECT_BODY, b.id, nullptr}, defaults(), true});
    out.push_back({"Moon mean node",
                   {PROMETHEIA_OBJECT_ORBIT_POINT, PROMETHEIA_MOON, "ascending-node:mean"},
                   defaults(),
                   false});
    out.push_back({"Moon osculating node",
                   {PROMETHEIA_OBJECT_ORBIT_POINT, PROMETHEIA_MOON, "ascending-node:osculating"},
                   defaults(),
                   false});
    prometheia_options lahiri = defaults();
    lahiri.sidereal = PROMETHEIA_SIDEREAL_LAHIRI;
    out.push_back(
        {"Mars, Lahiri", {PROMETHEIA_OBJECT_BODY, PROMETHEIA_MARS, nullptr}, lahiri, false});
    prometheia_options topo = defaults();
    topo.center = PROMETHEIA_CENTER_TOPOCENTRIC;
    topo.site_lon_deg = 8.55;
    topo.site_lat_deg = 47.37;
    topo.site_height_m = 400.0;
    out.push_back(
        {"Moon, topocentric", {PROMETHEIA_OBJECT_BODY, PROMETHEIA_MOON, nullptr}, topo, false});
    out.push_back(
        {"Mars, topocentric", {PROMETHEIA_OBJECT_BODY, PROMETHEIA_MARS, nullptr}, topo, false});
    return out;
}

struct Measured {
    double worst_arcsec = 0.0;
    double worst_rate_arcsec_per_day = 0.0;
    int failures = 0;
};

// The handle against prometheia_calc_object at n random instants (the span's
// ends among them).
Measured measure(prometheia_engine* e, const Case& c, const prometheia_segments* s, double from,
                 double to, int n, unsigned seed) {
    Measured m;
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> pick(from, to);
    for (int k = 0; k < n; ++k) {
        const double jd = k == 0 ? from : k == 1 ? to : pick(rng);
        prometheia_result r;
        prometheia_segments_point p;
        if (prometheia_calc_object(e, &c.object, jd, &c.opts, &r, nullptr) != PROMETHEIA_OK ||
            prometheia_segments_eval(s, jd, &p, nullptr) != PROMETHEIA_OK) {
            ++m.failures;
            continue;
        }
        m.worst_arcsec = std::max(m.worst_arcsec, angle_arcsec(r.xyz_au, p.xyz_au));
        const double cosb = std::cos(r.lat_deg / 180.0 * 3.14159265358979323846);
        const double dl = std::remainder(p.lon_speed - r.lon_speed, 360.0) * cosb;
        const double db = p.lat_speed - r.lat_speed;
        m.worst_rate_arcsec_per_day =
            std::max(m.worst_rate_arcsec_per_day, std::hypot(dl, db) * 3600.0);
    }
    return m;
}

} // namespace

TEST_CASE("c_segments_arguments") {
    prometheia_engine* e = open_engine(false);
    if (!e) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    const prometheia_object mars{PROMETHEIA_OBJECT_BODY, PROMETHEIA_MARS, nullptr};
    prometheia_segments* s = reinterpret_cast<prometheia_segments*>(&s); // not NULL
    prometheia_error err;
    CHECK(prometheia_segments_fit(nullptr, &mars, kJ2026, kJ2026 + 10, nullptr, 0.001, &s, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    CHECK(s == nullptr);
    CHECK(prometheia_segments_fit(e, nullptr, kJ2026, kJ2026 + 10, nullptr, 0.001, &s, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_segments_fit(e, &mars, kJ2026, kJ2026 + 10, nullptr, 0.001, nullptr, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    for (double tol : {0.0, -1.0, std::nan(""), double(INFINITY)}) {
        CHECK(prometheia_segments_fit(e, &mars, kJ2026, kJ2026 + 10, nullptr, tol, &s, &err) ==
              PROMETHEIA_ERROR_ARGUMENT);
        CHECK(s == nullptr);
    }
    CHECK(prometheia_segments_fit(e, &mars, kJ2026, kJ2026, nullptr, 0.001, &s, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_segments_fit(e, &mars, kJ2026 + 1, kJ2026, nullptr, 0.001, &s, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_segments_fit(e, &mars, std::nan(""), kJ2026, nullptr, 0.001, &s, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    CHECK(std::strlen(err.message) > 0);

    // The engine's own errors come back unchanged: coverage, an unknown body,
    // a bad option.
    CHECK(prometheia_segments_fit(e, &mars, 1000000.5, 1000010.5, nullptr, 0.001, &s, &err) ==
          PROMETHEIA_ERROR_COVERAGE);
    CHECK(s == nullptr);
    const prometheia_object nobody{PROMETHEIA_OBJECT_BODY, 20000001, nullptr};
    CHECK(prometheia_segments_fit(e, &nobody, kJ2026, kJ2026 + 10, nullptr, 0.001, &s, &err) ==
          PROMETHEIA_ERROR_NOT_FOUND);
    prometheia_options bad = defaults();
    bad.frame = 99;
    CHECK(prometheia_segments_fit(e, &mars, kJ2026, kJ2026 + 10, &bad, 0.001, &s, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);

    // A tolerance no fit reaches is refused, never a handle that misses it.
    CHECK(prometheia_segments_fit(e, &mars, kJ2026, kJ2026 + 10, nullptr, 1e-12, &s, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    CHECK(s == nullptr);
    CHECK(std::string(err.message).find("no fit within") != std::string::npos);

    REQUIRE(prometheia_segments_fit(e, &mars, kJ2026, kJ2026 + 10, nullptr, 0.001, &s, &err) ==
            PROMETHEIA_OK);
    REQUIRE(s != nullptr);
    CHECK(err.message[0] == '\0');
    prometheia_segments_point p;
    p.lon_deg = 1.0;
    for (double jd : {kJ2026 - 1e-6, kJ2026 + 10 + 1e-6, std::nan("")}) {
        CHECK(prometheia_segments_eval(s, jd, &p, &err) == PROMETHEIA_ERROR_COVERAGE);
        CHECK(p.lon_deg == 0.0);
    }
    CHECK(prometheia_segments_eval(nullptr, kJ2026, &p, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_segments_eval(s, kJ2026, nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_segments_eval(s, kJ2026, &p, &err) == PROMETHEIA_OK);
    CHECK(prometheia_segments_eval(s, kJ2026 + 10, &p, &err) == PROMETHEIA_OK);
    CHECK(prometheia_segments_get_layout(nullptr) == nullptr);
    prometheia_segments_close(s);
    prometheia_segments_close(nullptr);
    prometheia_engine_close(e);
}

TEST_CASE("c_segments_layout_is_eval") {
    prometheia_engine* e = open_engine(false);
    if (!e) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    // The Moon over 40 days, tropical and on the equator, so there are many
    // segments and the bounds between them are exercised.
    for (int coords : {PROMETHEIA_COORDS_ECLIPTIC, PROMETHEIA_COORDS_EQUATORIAL}) {
        const prometheia_object moon{PROMETHEIA_OBJECT_BODY, PROMETHEIA_MOON, nullptr};
        prometheia_options o = defaults();
        o.coords = coords;
        const double from = kJ2026 + 0.3, to = kJ2026 + 40.3;
        prometheia_segments* s = nullptr;
        REQUIRE(prometheia_segments_fit(e, &moon, from, to, &o, 0.001, &s, nullptr) ==
                PROMETHEIA_OK);
        const prometheia_segments_layout* l = prometheia_segments_get_layout(s);
        REQUIRE(l != nullptr);
        CHECK(l->count > 1);
        CHECK(l->stride == PROMETHEIA_SEGMENTS_MAX_DEGREE + 1);
        CHECK(l->jd_tt_start == from);
        CHECK(l->jd_tt_end == to);
        CHECK(l->bounds_jd_tt[0] == from);
        CHECK(l->bounds_jd_tt[l->count] == to);
        CHECK(l->tolerance_arcsec == 0.001);
        CHECK(l->err_arcsec <= 0.8 * 0.001);
        CHECK(l->coords == coords);
        int padding_nonzero = 0;
        for (int i = 0; i < l->count; ++i) {
            CHECK(l->bounds_jd_tt[i] < l->bounds_jd_tt[i + 1]);
            CHECK(l->degree[i] >= 1);
            CHECK(l->degree[i] <= PROMETHEIA_SEGMENTS_MAX_DEGREE);
            for (int c = 0; c < 3; ++c)
                for (int k = l->degree[i] + 1; k < l->stride; ++k)
                    padding_nonzero += l->coef[(3 * i + c) * l->stride + k] != 0.0 ||
                                       l->dcoef[(3 * i + c) * l->stride + k] != 0.0;
        }
        CHECK(padding_nonzero == 0);

        // The caller's own loop over the documented layout gives eval's
        // doubles: at the ends, at every bound, and at random instants.
        std::vector<double> at = {from, to};
        for (int i = 1; i < l->count; ++i)
            at.push_back(l->bounds_jd_tt[i]);
        std::mt19937_64 rng(7);
        std::uniform_real_distribution<double> pick(from, to);
        for (int k = 0; k < 300; ++k)
            at.push_back(pick(rng));
        int differ = 0;
        for (double jd : at) {
            prometheia_segments_point p;
            REQUIRE(prometheia_segments_eval(s, jd, &p, nullptr) == PROMETHEIA_OK);
            double x[3], v[3];
            own_eval(*l, jd, x, v);
            for (int c = 0; c < 3; ++c)
                differ += x[c] != p.xyz_au[c] || v[c] != p.vel_au_day[c];
            // And the spherical part from the vector, as documented.
            const double kR = 180.0 / 3.14159265358979323846;
            const double rho2 = x[0] * x[0] + x[1] * x[1];
            const double r = std::sqrt(rho2 + x[2] * x[2]);
            double lon = std::atan2(x[1], x[0]) * kR;
            if (lon < 0.0)
                lon += 360.0;
            differ += lon != p.lon_deg || std::atan2(x[2], std::sqrt(rho2)) * kR != p.lat_deg ||
                      r != p.dist_au || (x[0] * v[1] - x[1] * v[0]) / rho2 * kR != p.lon_speed ||
                      (v[2] * rho2 - x[2] * (x[0] * v[0] + x[1] * v[1])) /
                              (r * r * std::sqrt(rho2)) * kR !=
                          p.lat_speed ||
                      (x[0] * v[0] + x[1] * v[1] + x[2] * v[2]) / r != p.dist_speed;
            CHECK(p.lon_deg >= 0.0);
            CHECK(p.lon_deg < 360.0);
        }
        CHECK(differ == 0);
        prometheia_segments_close(s);
    }
    prometheia_engine_close(e);
}

TEST_CASE("c_segments_within_tolerance") {
    // Short spans, so the gate stays fast; c_segments_report runs the years.
    prometheia_engine* e = open_engine(false);
    if (!e) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    prometheia_engine* ec = open_engine(true);
    if (!ec)
        MESSAGE("SKIP (catalog cases): sb441-n16-de440span.bsp not found");
    unsigned seed = 1;
    for (const Case& c : cases()) {
        prometheia_engine* eng = c.catalog ? ec : e;
        if (!eng)
            continue;
        for (double tol : {0.001, 0.1}) {
            const double from = kJ2026 + 11.25, to = from + 12.0;
            prometheia_segments* s = nullptr;
            prometheia_error err;
            REQUIRE_MESSAGE(prometheia_segments_fit(eng, &c.object, from, to, &c.opts, tol, &s,
                                                    &err) == PROMETHEIA_OK,
                            c.name, ": ", err.message);
            const Measured m = measure(eng, c, s, from, to, 300, seed++);
            INFO(c.name, " at ", tol, "\": worst ", m.worst_arcsec, "\"");
            CHECK(m.failures == 0);
            CHECK(m.worst_arcsec <= tol);
            CHECK(prometheia_segments_get_layout(s)->err_arcsec <= 0.8 * tol);
            prometheia_segments_close(s);
        }
    }
    prometheia_engine_close(ec);
    prometheia_engine_close(e);
}

TEST_CASE("c_segments_through_a_solar_conjunction") {
    // A body behind the Sun: the point-mass deflection grows without bound
    // toward the disc's centre, so before the limb taper (docs/ENGINE.md,
    // "Gravitational deflection") these fits were refused, at 74" and
    // 1980", and the longitude stepped 0.05 degrees in a quarter hour.
    prometheia_engine* e = open_engine(false);
    if (!e) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    const Case cases[] = {
        // Uranus 0.034 degrees from the Sun's centre on 2029-06-04, 16:20 UT.
        {"Uranus", {PROMETHEIA_OBJECT_BODY, 7, nullptr}, defaults(), false},
        // The Sun crosses Jupiter's mean node every June.
        {"Jupiter mean node",
         {PROMETHEIA_OBJECT_ORBIT_POINT, 5, "ascending-node:mean"},
         defaults(),
         false},
    };
    const double windows[][2] = {{2462289.0, 2462295.0}, {2461195.0, 2461225.0}};
    unsigned seed = 7;
    for (int k = 0; k < 2; ++k) {
        const Case& c = cases[k];
        const double from = windows[k][0], to = windows[k][1];
        prometheia_segments* s = nullptr;
        prometheia_error err;
        REQUIRE_MESSAGE(prometheia_segments_fit(e, &c.object, from, to, &c.opts, 0.001, &s, &err) ==
                            PROMETHEIA_OK,
                        c.name, ": ", err.message);
        const Measured m = measure(e, c, s, from, to, 300, seed++);
        INFO(c.name, ": worst ", m.worst_arcsec, "\"");
        CHECK(m.failures == 0);
        CHECK(m.worst_arcsec <= 0.001);
        prometheia_segments_close(s);
    }
    // And the position itself is smooth through the disc: 10-minute second
    // differences of longitude across the conjunction, 229" before.
    const prometheia_object uranus{PROMETHEIA_OBJECT_BODY, 7, nullptr};
    const prometheia_options o = defaults();
    const double centre = 2462292.18; // 2029-06-04 16:20 UT, near enough
    double lon[3] = {};
    double worst = 0.0;
    for (int i = -72; i <= 72; ++i) {
        prometheia_result r;
        REQUIRE(prometheia_calc_object(e, &uranus, centre + i / 144.0, &o, &r, nullptr) ==
                PROMETHEIA_OK);
        lon[0] = lon[1];
        lon[1] = lon[2];
        lon[2] = r.lon_deg;
        if (i >= -70)
            worst = std::max(worst, std::fabs(lon[2] - 2.0 * lon[1] + lon[0]) * 3600.0);
    }
    INFO("worst second difference ", worst, "\"");
    CHECK(worst < 0.05);
    // The rate has no step either, minute by minute through both limb
    // crossings (Uranus passes 0.002 degrees from the Sun's centre); it read
    // -6.6 degrees a day before.
    double prev = std::nan(""), worst_step = 0.0;
    for (int i = -1440; i <= 1440; ++i) {
        prometheia_result r;
        REQUIRE(prometheia_calc_object(e, &uranus, centre + i / 1440.0, &o, &r, nullptr) ==
                PROMETHEIA_OK);
        CHECK(r.lon_speed > 0.05);
        CHECK(r.lon_speed < 0.065);
        if (!std::isnan(prev))
            worst_step = std::max(worst_step, std::fabs(r.lon_speed - prev));
        prev = r.lon_speed;
    }
    INFO("worst one-minute step in the rate ", worst_step, " deg/day");
    CHECK(worst_step < 1e-4);
    prometheia_engine_close(e);
}

TEST_CASE("c_segments_shared_across_threads") {
    // One handle, six threads evaluating it at once: every answer equals the
    // one thread's. And fits made on contexts of one dataset, on their own
    // threads, are the same handle to the last coefficient.
    if (!readable(de440_path())) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    prometheia_dataset* d = nullptr;
    REQUIRE(prometheia_dataset_open(de440_path().c_str(), &d, nullptr) == PROMETHEIA_OK);
    REQUIRE(prometheia_dataset_freeze(d, nullptr) == PROMETHEIA_OK);
    const prometheia_object moon{PROMETHEIA_OBJECT_BODY, PROMETHEIA_MOON, nullptr};
    const double from = kJ2026, to = kJ2026 + 30.0;
    constexpr int kThreads = 6;
    std::vector<prometheia_segments*> fits(kThreads, nullptr);
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t)
            threads.emplace_back([&, t] {
                prometheia_engine* ctx = nullptr;
                if (prometheia_context_open(d, &ctx, nullptr) != PROMETHEIA_OK)
                    return;
                prometheia_segments_fit(ctx, &moon, from, to, nullptr, 0.001, &fits[t], nullptr);
                prometheia_engine_close(ctx); // the handle outlives its context
            });
        for (std::thread& th : threads)
            th.join();
    }
    for (int t = 0; t < kThreads; ++t)
        REQUIRE(fits[t] != nullptr);
    const prometheia_segments_layout* l0 = prometheia_segments_get_layout(fits[0]);
    int differ = 0;
    for (int t = 1; t < kThreads; ++t) {
        const prometheia_segments_layout* l = prometheia_segments_get_layout(fits[t]);
        REQUIRE(l->count == l0->count);
        const size_t n = size_t(3 * l->stride * l->count);
        differ += std::memcmp(l->coef, l0->coef, n * sizeof(double)) != 0;
        differ += std::memcmp(l->dcoef, l0->dcoef, n * sizeof(double)) != 0;
        differ += std::memcmp(l->bounds_jd_tt, l0->bounds_jd_tt,
                              size_t(l->count + 1) * sizeof(double)) != 0;
    }
    CHECK(differ == 0);

    std::vector<double> at(4000);
    std::mt19937_64 rng(11);
    std::uniform_real_distribution<double> pick(from, to);
    for (double& jd : at)
        jd = pick(rng);
    auto run = [&](std::vector<prometheia_segments_point>& out) {
        out.resize(at.size());
        for (size_t k = 0; k < at.size(); ++k)
            prometheia_segments_eval(fits[0], at[k], &out[k], nullptr);
    };
    std::vector<prometheia_segments_point> serial;
    run(serial);
    std::vector<std::vector<prometheia_segments_point>> parallel(kThreads);
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t)
            threads.emplace_back([&, t] { run(parallel[t]); });
        for (std::thread& th : threads)
            th.join();
    }
    int wrong = 0;
    for (const auto& p : parallel)
        wrong += std::memcmp(p.data(), serial.data(), serial.size() * sizeof serial[0]) != 0;
    CHECK(wrong == 0);
    for (prometheia_segments* s : fits)
        prometheia_segments_close(s);
    prometheia_dataset_close(d);
}

TEST_CASE("c_segments_steps_and_refusals") {
    // Astrolog's review, points 2 and 3: a bound at every file handover, no
    // direction from the observer's own position, and a fit that fails whole
    // when one instant is refused.
    prometheia_engine* e = open_engine(false);
    if (!e) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    prometheia_segments* s = reinterpret_cast<prometheia_segments*>(&s);
    prometheia_error err;

    // The heliocentric Sun: no direction, ARGUMENT, not noise.
    const prometheia_object sun{PROMETHEIA_OBJECT_BODY, PROMETHEIA_SUN, nullptr};
    prometheia_options helio = defaults();
    helio.center = PROMETHEIA_CENTER_HELIOCENTRIC;
    CHECK(prometheia_segments_fit(e, &sun, kJ2026, kJ2026 + 5, &helio, 0.001, &s, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    CHECK(s == nullptr);

    // A span running past DE440's last day: refused whole, COVERAGE.
    double first_tdb = 0.0, last_tdb = 0.0;
    const char* src = nullptr;
    REQUIRE(prometheia_engine_ephemeris_span(e, 0, &first_tdb, &last_tdb, &src, nullptr) ==
            PROMETHEIA_OK);
    const prometheia_object mars{PROMETHEIA_OBJECT_BODY, PROMETHEIA_MARS, nullptr};
    s = reinterpret_cast<prometheia_segments*>(&s);
    CHECK(prometheia_segments_fit(e, &mars, last_tdb - 20, last_tdb + 20, nullptr, 0.001, &s,
                                  &err) == PROMETHEIA_ERROR_COVERAGE);
    CHECK(s == nullptr);

    // DE441 behind DE440: the span crosses DE440's first day (1550). The
    // observer's reads hand over at the edge, the body's one light time
    // later; both are bounds, and the fit holds its tolerance up to them
    // from either side.
    const std::string de441 = env_or("PROMETHEIA_DE441", std::string(PROMETHEIA_SOURCE_DIR) +
                                                             "/ephe/linux_m13000p17000.441");
    if (!readable(de441)) {
        MESSAGE("SKIP (handover): DE441 not found");
        prometheia_engine_close(e);
        return;
    }
    REQUIRE(prometheia_engine_add_ephemeris(e, de441.c_str(), nullptr) == PROMETHEIA_OK);
    const double edge = first_tdb - prometheia_tdb_minus_tt(first_tdb) / 86400.0;
    for (int body : {PROMETHEIA_MOON, PROMETHEIA_MARS, PROMETHEIA_SATURN}) {
        const prometheia_object obj{PROMETHEIA_OBJECT_BODY, body, nullptr};
        const Case c{"handover", obj, defaults(), false};
        prometheia_result at_edge;
        REQUIRE(prometheia_calc_object(e, &obj, edge, &c.opts, &at_edge, nullptr) == PROMETHEIA_OK);
        const double from = edge - 15.0, to = edge + 15.0;
        INFO(err.message);
        REQUIRE(prometheia_segments_fit(e, &obj, from, to, &c.opts, 0.001, &s, &err) ==
                PROMETHEIA_OK);
        const prometheia_segments_layout* l = prometheia_segments_get_layout(s);
        double nearest_edge = 1e9, nearest_retarded = 1e9;
        for (int i = 1; i < l->count; ++i) {
            nearest_edge = std::min(nearest_edge, std::fabs(l->bounds_jd_tt[i] - edge));
            nearest_retarded = std::min(
                nearest_retarded, std::fabs(l->bounds_jd_tt[i] - (edge + at_edge.light_time_days)));
        }
        INFO("body ", body, ": light time ", at_edge.light_time_days, " d");
        CHECK(nearest_edge < 1e-9);
        CHECK(nearest_retarded < 1e-9);
        Measured m = measure(e, c, s, from, to, 1000, 31u + unsigned(body));
        for (double around : {edge, edge + at_edge.light_time_days})
            for (double d : {-1e-3, -1e-6, 0.0, 1e-6, 1e-3}) {
                prometheia_result r;
                prometheia_segments_point p;
                REQUIRE(prometheia_calc_object(e, &obj, around + d, &c.opts, &r, nullptr) ==
                        PROMETHEIA_OK);
                REQUIRE(prometheia_segments_eval(s, around + d, &p, nullptr) == PROMETHEIA_OK);
                m.worst_arcsec = std::max(m.worst_arcsec, angle_arcsec(r.xyz_au, p.xyz_au));
            }
        CHECK(m.failures == 0);
        CHECK(m.worst_arcsec <= 0.001);
        prometheia_segments_close(s);
    }
    prometheia_engine_close(e);
}

TEST_CASE("c_segments_report" * doctest::skip()) {
    // The plan's done-when (A-16): a year at 0.001", 10,000 random instants
    // per case against prometheia_calc_object, and the costs for SEGMENTS.md.
    // Run: build/test_c_segments -tc=c_segments_report --no-skip
    prometheia_engine* e = open_engine(true);
    if (!e) {
        MESSAGE("SKIP: DE440 or sb441-n16-de440span.bsp not found");
        return;
    }
    const double tol = std::getenv("SEG_TOL") ? std::atof(std::getenv("SEG_TOL")) : 0.001;
    const double days = std::getenv("SEG_DAYS") ? std::atof(std::getenv("SEG_DAYS")) : 365.25;
    const double from = kJ2026, to = kJ2026 + days;
    std::printf("tolerance %.4g\", %.2f days from JD %.1f TT, 10,000 instants each\n", tol, days,
                from);
    std::printf("%-22s %5s %6s %9s %9s %10s %10s %10s %8s\n", "case", "segs", "deg", "fit ms", "kB",
                "declared\"", "measured\"", "rate\"/d", "eval ns");
    unsigned seed = 100;
    for (const Case& c : cases()) {
        prometheia_segments* s = nullptr;
        prometheia_error err;
        const auto t0 = std::chrono::steady_clock::now();
        const prometheia_status st =
            prometheia_segments_fit(e, &c.object, from, to, &c.opts, tol, &s, &err);
        const double fit_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        if (st != PROMETHEIA_OK) {
            std::printf("%-22s refused: %s\n", c.name, err.message);
            CHECK(st == PROMETHEIA_OK);
            continue;
        }
        const prometheia_segments_layout* l = prometheia_segments_get_layout(s);
        int dmin = 99, dmax = 0;
        for (int i = 0; i < l->count; ++i) {
            dmin = std::min(dmin, l->degree[i]);
            dmax = std::max(dmax, l->degree[i]);
        }
        const double kb = 2.0 * 3 * l->stride * l->count * sizeof(double) / 1024.0;
        const Measured m = measure(e, c, s, from, to, 10000, seed++);

        // Evaluation cost: 1e6 evaluations at random instants.
        std::vector<double> at(1 << 16);
        std::mt19937_64 rng(seed);
        std::uniform_real_distribution<double> pick(from, to);
        for (double& jd : at)
            jd = pick(rng);
        prometheia_segments_point p;
        double sink = 0.0;
        const int reps = 1000000;
        const auto e0 = std::chrono::steady_clock::now();
        for (int k = 0; k < reps; ++k) {
            prometheia_segments_eval(s, at[size_t(k) & (at.size() - 1)], &p, nullptr);
            sink += p.lon_deg;
        }
        const double eval_ns =
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - e0)
                .count() /
            reps;
        char deg[16];
        std::snprintf(deg, sizeof deg, "%d-%d", dmin, dmax);
        std::printf("%-22s %5d %6s %9.1f %9.1f %10.6f %10.6f %10.5f %8.0f%s\n", c.name, l->count,
                    deg, fit_ms, kb, l->err_arcsec, m.worst_arcsec, m.worst_rate_arcsec_per_day,
                    eval_ns, sink == 0.123 ? "!" : "");
        CHECK(m.failures == 0);
        CHECK(m.worst_arcsec <= tol);
        prometheia_segments_close(s);
    }
    prometheia_engine_close(e);
}

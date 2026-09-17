// SPDX-License-Identifier: GPL-2.0-or-later
//
// Chebyshev segment fits: the coefficients reproduce the sampler, the
// declared residuals bound what an independent check measures, and the
// segments tile the span. The DE440 cases SKIP without the ephemeris.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unistd.h>

#include <prometheia/engine.hpp>
#include <prometheia/segments.hpp>

#include "synthetic_spk.hpp"

#include <doctest/doctest.h>

using namespace prometheia;
using synth::TempFile;

namespace {

segments::Sampler body_sampler(Engine& e, int id, const CalcOptions& o) {
    return [&e, id, o](double jd, double p[3], double v[3]) -> Result<void> {
        auto r = e.calc(id, jd, o);
        if (!r) {
            return r.error();
        }
        for (int i = 0; i < 3; ++i) {
            p[i] = r.value().pos.xyz_au[i];
            v[i] = r.value().pos.vel_au_day[i];
        }
        return {};
    };
}

double arcsec_between(const double a[3], const double b[3]) {
    const double c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                         a[0] * b[1] - a[1] * b[0]};
    const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    const double cross = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    return std::atan2(cross, dot) * 180.0 / 3.14159265358979323846 * 3600.0;
}

// Every instant of the span lies in exactly one segment, and neighbours meet.
void check_tiling(const std::vector<segments::Segment>& segs, double from, double to) {
    REQUIRE(!segs.empty());
    CHECK(std::fabs((segs.front().mid_jd_tt - segs.front().half_span_days) - from) < 1e-9);
    CHECK(std::fabs((segs.back().mid_jd_tt + segs.back().half_span_days) - to) < 1e-9);
    for (size_t i = 1; i < segs.size(); ++i) {
        const double end = segs[i - 1].mid_jd_tt + segs[i - 1].half_span_days;
        const double start = segs[i].mid_jd_tt - segs[i].half_span_days;
        CHECK(std::fabs(end - start) < 1e-9);
        CHECK(segs[i].half_span_days > 0.0);
    }
}

} // namespace

TEST_CASE("segments_fit_synthetic_exactly") {
    // The synthetic kernel moves linearly, so a low degree is exact and the
    // fit needs no splitting at all.
    TempFile tf("segments");
    Engine e = synth::open_synthetic(tf);
    CalcOptions o = CalcOptions::geometric();
    o.frame = Frame::ICRF;
    o.sigma = false;
    const double from = 2451545.0, to = from + 40.0;
    auto r = segments::fit(body_sampler(e, body::kJupiter, o), from, to);
    REQUIRE_MESSAGE(r.ok(), r.error().message);
    const segments::FitReport& rep = r.value();
    CHECK(rep.segments.size() == 1);
    CHECK(rep.met_target);
    CHECK(rep.worst_err_arcsec < 1e-6);
    check_tiling(rep.segments, from, to);

    // Evaluating the segment reproduces the engine, position and velocity.
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> pick(from, to);
    for (int i = 0; i < 50; ++i) {
        const double jd = pick(rng);
        double p[3], v[3];
        rep.segments[0].position(jd, p);
        rep.segments[0].velocity(jd, v);
        const Position truth = e.calc(body::kJupiter, jd, o).value().pos;
        CHECK(arcsec_between(p, truth.xyz_au) < 1e-6);
        // The derivative recurrence amplifies the coefficients by the square
        // of the degree, so exactness here means roundoff, not zero: a part in
        // a billion of the body's own speed.
        for (int k = 0; k < 3; ++k) {
            CHECK(std::fabs(v[k] - truth.vel_au_day[k]) < 1e-10);
        }
    }
}

TEST_CASE("segments_reject_bad_arguments") {
    TempFile tf("segments-args");
    Engine e = synth::open_synthetic(tf);
    const auto s = body_sampler(e, body::kJupiter, CalcOptions::geometric());
    CHECK(segments::fit(s, 2451545.0, 2451545.0).error().code == ErrorCode::ArgumentError);
    CHECK(segments::fit(s, 2451600.0, 2451545.0).error().code == ErrorCode::ArgumentError);
    segments::FitOptions bad;
    bad.target_err_arcsec = 0.0;
    CHECK(segments::fit(s, 2451545.0, 2451575.0, bad).error().code == ErrorCode::ArgumentError);
    bad = {};
    bad.max_degree = 99;
    CHECK(segments::fit(s, 2451545.0, 2451575.0, bad).error().code == ErrorCode::ArgumentError);
    // A span the fitter cannot cover within its segment budget is an error,
    // not a silent truncation.
    segments::FitOptions tight;
    tight.target_err_arcsec = 1e-12;
    tight.min_half_span_days = 1e-6;
    tight.max_segments = 4;
    CHECK(!segments::fit(s, 2451545.0, 2451575.0, tight).ok());
    // The sampler's own failure comes back unchanged.
    segments::Sampler failing = [](double, double[3], double[3]) -> Result<void> {
        return make_error(ErrorCode::NotFound, "no such body");
    };
    CHECK(segments::fit(failing, 2451545.0, 2451575.0).error().code == ErrorCode::NotFound);
}

TEST_CASE("segments_de440_declared_residuals_bound_the_truth") {
    const char* env = std::getenv("PROMETHEIA_DE440");
    const std::string de =
        (env && *env) ? env : std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_p1550p2650.440";
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    auto opened = Engine::open(de);
    REQUIRE(opened.ok());
    Engine e = std::move(opened).value();
    CalcOptions o; // apparent place, geocentric, true ecliptic of date
    o.sigma = false;
    const double from = 2461300.5, to = from + 30.0;

    struct Case {
        int id;
        const char* name;
        double target;
    };
    for (const Case& c :
         {Case{301, "Moon", 0.1}, Case{10, "Sun", 0.1}, Case{5, "Jupiter", 0.001}}) {
        segments::FitOptions fo;
        fo.target_err_arcsec = c.target;
        auto r = segments::fit(body_sampler(e, c.id, o), from, to, fo);
        REQUIRE_MESSAGE(r.ok(), r.error().message);
        const segments::FitReport& rep = r.value();
        CHECK(rep.met_target);
        check_tiling(rep.segments, from, to);

        double declared = 0.0, declared_rate = 0.0;
        int lo_degree = 99, hi_degree = 0;
        for (const segments::Segment& s : rep.segments) {
            lo_degree = std::min(lo_degree, s.degree);
            hi_degree = std::max(hi_degree, s.degree);
            declared = std::max(declared, s.err_arcsec);
            declared_rate = std::max(declared_rate, s.err_rate_arcsec_per_day);
            CHECK(s.degree <= fo.max_degree);
            CHECK(s.x.size() == size_t(s.degree) + 1);
        }
        CHECK(declared <= c.target);

        // An independent check at random instants: what the segments declare
        // must bound what an outsider measures. The check set the fitter uses
        // includes the interval's endpoints, where the derivative's error
        // peaks; without them the rate residual is under-reported severalfold.
        std::mt19937 rng(23);
        std::uniform_real_distribution<double> pick(from, to);
        double worst = 0.0, worst_rate = 0.0;
        for (int i = 0; i < 200; ++i) {
            const double jd = pick(rng);
            const segments::Segment* seg = nullptr;
            for (const segments::Segment& s : rep.segments) {
                if (jd >= s.mid_jd_tt - s.half_span_days - 1e-9 &&
                    jd <= s.mid_jd_tt + s.half_span_days + 1e-9) {
                    seg = &s;
                    break;
                }
            }
            REQUIRE(seg != nullptr);
            double p[3], v[3];
            seg->position(jd, p);
            seg->velocity(jd, v);
            const Position truth = e.calc(c.id, jd, o).value().pos;
            const double d =
                std::sqrt(truth.xyz_au[0] * truth.xyz_au[0] + truth.xyz_au[1] * truth.xyz_au[1] +
                          truth.xyz_au[2] * truth.xyz_au[2]);
            worst = std::max(worst, arcsec_between(p, truth.xyz_au));
            double dv = 0.0;
            for (int k = 0; k < 3; ++k) {
                dv += (v[k] - truth.vel_au_day[k]) * (v[k] - truth.vel_au_day[k]);
            }
            worst_rate =
                std::max(worst_rate, std::sqrt(dv) / d * 180.0 / 3.14159265358979323846 * 3600.0);
        }
        std::printf("  %-8s %2zu segments, degree %d..%d, declared %.4g\" / %.3g\"/day, "
                    "independent %.4g\" / %.3g\"/day, %zu samples\n",
                    c.name, rep.segments.size(), lo_degree, hi_degree, declared, declared_rate,
                    worst, worst_rate, rep.sampler_calls);
        CHECK(worst <= declared * 1.05 + 1e-9);
        CHECK(worst_rate <= declared_rate * 1.05 + 1e-9);
    }
}

// What a scanning client actually needs: the roots of the fitted function
// where the sampled function's roots are. Astrolog finds events (ingresses,
// aspects, stations) by root-finding, and with segments it does that on the
// polynomial instead of interpolating linearly between half-hourly casts.
TEST_CASE("segments_de440_roots_land_where_the_engine_puts_them") {
    const char* env = std::getenv("PROMETHEIA_DE440");
    const std::string de =
        (env && *env) ? env : std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_p1550p2650.440";
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    auto opened = Engine::open(de);
    REQUIRE(opened.ok());
    Engine e = std::move(opened).value();
    CalcOptions o; // apparent longitude, the quantity an event search watches
    o.sigma = false;
    const double from = 2461300.5, to = from + 180.0;

    segments::FitOptions fo;
    fo.target_err_arcsec = 1.0; // a coarse fit, so the test bounds the worst case
    auto r = segments::fit(body_sampler(e, 301, o), from, to, fo);
    REQUIRE_MESSAGE(r.ok(), r.error().message);
    const segments::FitReport& rep = r.value();

    const auto seg_lon = [&](double jd) {
        for (const segments::Segment& s : rep.segments) {
            if (jd >= s.mid_jd_tt - s.half_span_days - 1e-9 &&
                jd <= s.mid_jd_tt + s.half_span_days + 1e-9) {
                double p[3];
                s.position(jd, p);
                const double l = std::atan2(p[1], p[0]) * 180.0 / 3.14159265358979323846;
                return l < 0.0 ? l + 360.0 : l;
            }
        }
        return -1.0;
    };
    const auto engine_lon = [&](double jd) {
        const Position p = e.calc(301, jd, o).value().pos;
        const double l = std::atan2(p.xyz_au[1], p.xyz_au[0]) * 180.0 / 3.14159265358979323846;
        return l < 0.0 ? l + 360.0 : l;
    };
    const auto root = [](double lo, double hi, double want, auto f) {
        for (int k = 0; k < 60; ++k) {
            const double m = 0.5 * (lo + hi);
            (f(m) < want ? lo : hi) = m;
        }
        return 0.5 * (lo + hi);
    };

    int found = 0;
    double worst_seconds = 0.0;
    for (double jd = from + 0.25; jd < to - 0.5; jd += 0.25) {
        const double a = seg_lon(jd), b = seg_lon(jd + 0.25);
        if (b < a || std::floor(a / 30.0) == std::floor(b / 30.0)) {
            continue; // no sign boundary in this step (or the wrap at 360)
        }
        const double want = std::floor(b / 30.0) * 30.0;
        const double from_segments = root(jd, jd + 0.25, want, seg_lon);
        const double from_engine = root(jd, jd + 0.25, want, engine_lon);
        worst_seconds = std::max(worst_seconds, std::fabs(from_segments - from_engine) * 86400.0);
        ++found;
    }
    std::printf("  Moon ingresses over 180 days: %d found, worst %.3f s of time (fit target 1\")\n",
                found, worst_seconds);
    CHECK(found >= 70); // the Moon crosses a sign boundary about every 2.3 days
    // One arcsecond of the Moon is about 2 seconds of time; the roots must not
    // be worse than the fit that produced them.
    CHECK(worst_seconds < 3.0);
}

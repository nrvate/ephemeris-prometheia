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
            if (v) {
                v[i] = r.value().pos.vel_au_day[i];
            }
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

TEST_CASE("segments_fit_recovers_known_chebyshev_coefficients") {
    // A sampler that is a Chebyshev series of known coefficients, fitted over
    // one interval: the interpolation must give them back, to the rounding of
    // the instants (a Julian date near 2.4e6 is quantized to 40 microseconds,
    // 2e-11 of this interval's half span). This pins the cosine table the three
    // components share: a wrong entry puts an error of the coefficient's size.
    const double from = 2451545.0, to = from + 20.0;
    const double mid = 0.5 * (from + to), half = 0.5 * (to - from);
    const double cx[6] = {0.3, 0.2, -0.1, 0.05, 0.02, -0.01};
    const double cy[3] = {1.0, 0.0, 0.1};
    const auto series = [&](const double* c, int n, double t) {
        double t0 = 1.0, t1 = t, sum = c[0];
        for (int k = 1; k < n; ++k) {
            sum += c[k] * t1;
            const double t2 = 2.0 * t * t1 - t0;
            t0 = t1;
            t1 = t2;
        }
        return sum;
    };
    const segments::Sampler sampler = [&](double jd, double p[3], double*) -> Result<void> {
        const double t = (jd - mid) / half;
        p[0] = series(cx, 6, t);
        p[1] = series(cy, 3, t);
        p[2] = 0.5;
        return {};
    };
    auto r = segments::fit(sampler, from, to);
    REQUIRE_MESSAGE(r.ok(), r.error().message);
    REQUIRE(r.value().segments.size() == 1);
    const segments::Segment& s = r.value().segments[0];
    for (size_t k = 0; k < s.x.size(); ++k) {
        CHECK(std::fabs(s.x[k] - (k < 6 ? cx[k] : 0.0)) < 5e-11);
        CHECK(std::fabs(s.y[k] - (k < 3 ? cy[k] : 0.0)) < 5e-11);
        CHECK(std::fabs(s.z[k] - (k == 0 ? 0.5 : 0.0)) < 5e-11);
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

// What a fit costs. The sampler is asked for rates only where the rate
// residual is measured, and the declared rate must still bound what an
// outsider measures at random instants: the derivative's error peaks at the
// interval's ends, so a rate subset without them under-reports it severalfold.
TEST_CASE("segments_rates_are_asked_only_where_the_derivative_peaks") {
    // Closed-form motion with closed-form rates, fast enough to need splits.
    const double j0 = 2451545.0, w = 0.2;
    size_t positions = 0, with_rates = 0;
    const auto at = [&](double jd, double p[3], double v[3]) {
        const double a = w * (jd - j0);
        p[0] = std::cos(a) + 0.05 * std::cos(2.3 * a);
        p[1] = std::sin(a) + 0.05 * std::sin(2.3 * a);
        p[2] = 0.1 * std::sin(0.5 * a);
        if (v) {
            v[0] = -w * std::sin(a) - 0.05 * 2.3 * w * std::sin(2.3 * a);
            v[1] = w * std::cos(a) + 0.05 * 2.3 * w * std::cos(2.3 * a);
            v[2] = 0.05 * w * std::cos(0.5 * a);
        }
    };
    const segments::Sampler sampler = [&](double jd, double p[3], double v[3]) -> Result<void> {
        ++positions;
        if (v) {
            ++with_rates;
        }
        at(jd, p, v);
        return {};
    };
    segments::FitOptions fo;
    fo.target_err_arcsec = 0.001;
    const double from = j0 + 10.0, to = from + 30.4375;
    auto r = segments::fit(sampler, from, to, fo);
    REQUIRE_MESSAGE(r.ok(), r.error().message);
    const segments::FitReport& rep = r.value();
    CHECK(rep.met_target);
    CHECK(rep.sampler_calls == positions);
    check_tiling(rep.segments, from, to);

    // Rates: both ends, the points beside them and the quarters between, at
    // most nine a segment (the ends shared by neighbours), a small share of
    // the calls.
    CHECK(with_rates <= 9 * rep.segments.size());
    CHECK(with_rates * 5 <= positions);

    std::mt19937 rng(5);
    for (const segments::Segment& s : rep.segments) {
        std::uniform_real_distribution<double> in(s.mid_jd_tt - s.half_span_days,
                                                  s.mid_jd_tt + s.half_span_days);
        double worst = 0.0, worst_rate = 0.0;
        for (int i = 0; i < 300; ++i) {
            const double jd = i == 0   ? s.mid_jd_tt - s.half_span_days
                              : i == 1 ? s.mid_jd_tt + s.half_span_days
                                       : in(rng);
            double p[3], v[3], tp[3], tv[3];
            s.position(jd, p);
            s.velocity(jd, v);
            at(jd, tp, tv);
            worst = std::max(worst, arcsec_between(p, tp));
            const double d = std::sqrt(tp[0] * tp[0] + tp[1] * tp[1] + tp[2] * tp[2]);
            const double dv =
                std::sqrt((v[0] - tv[0]) * (v[0] - tv[0]) + (v[1] - tv[1]) * (v[1] - tv[1]) +
                          (v[2] - tv[2]) * (v[2] - tv[2]));
            worst_rate = std::max(worst_rate, dv / d * 180.0 / 3.14159265358979323846 * 3600.0);
        }
        CHECK(worst <= s.err_arcsec * 1.03 + 1e-9);
        CHECK(worst_rate <= s.err_rate_arcsec_per_day * 1.03 + 1e-9);
    }
}

// The cost of a month against DE440 (docs/SEGMENTS.md, "Cost"). The bounds
// are measured with room; the fit used to take 223 sampler calls a month for
// the Sun at 0.001" and 174 for Pluto, every one of them with rates. The
// declared residuals of each segment must still bound a random instant in it.
TEST_CASE("segments_de440_a_month_costs_what_the_docs_say") {
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
    CalcOptions o;
    o.sigma = false;
    struct Case {
        int id;
        const char* name;
        double target;
        size_t before; // calls a month before the rework: the mean over 1800-1810
        size_t limit;  // allowed now
    };
    const double month = 30.4375, j2000 = 2451545.0;
    std::mt19937 rng(41);
    for (const Case& c : {Case{10, "Sun", 0.0008, 223, 110}, Case{9, "Pluto", 0.0008, 174, 105},
                          Case{4, "Mars", 0.0008, 226, 105}, Case{5, "Jupiter", 0.08, 150, 80},
                          Case{301, "Moon", 0.08, 175, 210}}) {
        size_t calls = 0, segs = 0;
        double worst = 0.0, worst_declared = 0.0, worst_rate = 0.0, worst_rate_declared = 0.0;
        const int months = 8;
        for (int m = 0; m < months; ++m) {
            const double from = j2000 + (400 + m) * month, to = from + month;
            segments::FitOptions fo;
            fo.target_err_arcsec = c.target;
            auto r = segments::fit(body_sampler(e, c.id, o), from, to, fo);
            REQUIRE_MESSAGE(r.ok(), r.error().message);
            CHECK(r.value().met_target);
            calls += r.value().sampler_calls;
            segs += r.value().segments.size();
            for (const segments::Segment& s : r.value().segments) {
                std::uniform_real_distribution<double> in(s.mid_jd_tt - s.half_span_days,
                                                          s.mid_jd_tt + s.half_span_days);
                double seg_worst = 0.0, seg_rate = 0.0;
                for (int i = 0; i < 60; ++i) {
                    const double jd = in(rng);
                    double p[3], v[3];
                    s.position(jd, p);
                    s.velocity(jd, v);
                    const Position t = e.calc(c.id, jd, o).value().pos;
                    seg_worst = std::max(seg_worst, arcsec_between(p, t.xyz_au));
                    const double d =
                        std::sqrt(t.xyz_au[0] * t.xyz_au[0] + t.xyz_au[1] * t.xyz_au[1] +
                                  t.xyz_au[2] * t.xyz_au[2]);
                    double dv = 0.0;
                    for (int k = 0; k < 3; ++k) {
                        dv += (v[k] - t.vel_au_day[k]) * (v[k] - t.vel_au_day[k]);
                    }
                    seg_rate = std::max(seg_rate, std::sqrt(dv) / d * 180.0 /
                                                      3.14159265358979323846 * 3600.0);
                }
                // Each segment on its own: the declared numbers bound it.
                CHECK(seg_worst <= s.err_arcsec * 1.05 + 1e-9);
                CHECK(seg_rate <= s.err_rate_arcsec_per_day * 1.05 + 1e-9);
                worst = std::max(worst, seg_worst / s.err_arcsec);
                worst_rate = std::max(worst_rate, seg_rate / s.err_rate_arcsec_per_day);
                worst_declared = std::max(worst_declared, s.err_arcsec);
                worst_rate_declared = std::max(worst_rate_declared, s.err_rate_arcsec_per_day);
            }
        }
        std::printf("  %-8s %.1f calls a month (was %zu), %.2f segments a month, direction %.3f "
                    "and rate %.3f of declared\n",
                    c.name, double(calls) / months, c.before, double(segs) / months, worst,
                    worst_rate);
        CHECK(calls <= c.limit * size_t(months));
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

// ---- scalar series -----------------------------------------------------------

TEST_CASE("segments_scalar_fits_a_known_function_within_its_target") {
    // An ayanamsa-like function: a large linear drift plus a slow periodic
    // term. The fit must meet the target where it says it does, on the check
    // set (interior and both endpoints), and evaluate back exactly.
    const auto sampler = [](double jd, double& v) -> Result<void> {
        const double t = jd - 2451545.0;
        v = 23.85 + 13.97e-3 * t + 0.02 * std::sin(2.0 * 3.14159265358979323846 * t / 365.25);
        return {};
    };
    segments::ScalarFitOptions o;
    o.target = 1e-9; // degrees: ~3.6e-6 arcsec, met easily by degree 4-5
    auto fit = segments::fit_scalar(sampler, 2451545.0, 2451545.0 + 320.0, o);
    REQUIRE_MESSAGE(fit.ok(), fit.error().message);
    const std::vector<segments::ScalarSegment>& segs = fit.value().segments;
    REQUIRE(!segs.empty());
    double worst = 0.0;
    for (int i = 0; i <= 3200; ++i) {
        const double jd = 2451545.0 + i * 0.1;
        double want = 0.0;
        REQUIRE(sampler(jd, want).ok());
        for (const segments::ScalarSegment& s : segs) {
            if (jd >= s.mid_jd_tt - s.half_span_days && jd <= s.mid_jd_tt + s.half_span_days) {
                worst = std::max(worst, std::fabs(s.value(jd) - want));
                break;
            }
        }
    }
    CHECK(worst <= o.target);
    // Every segment's declared residual is a measured bound on itself.
    for (const segments::ScalarSegment& s : segs) {
        CHECK(s.err_value <= o.target);
        CHECK(s.err_value > 0.0);
    }
    // The residuals are measured on both endpoints: the declared error of the
    // segment holding the span's start must be at least the error AT the
    // start.
    double at_start = 0.0;
    REQUIRE(sampler(2451545.0, at_start).ok());
    CHECK(segs.front().err_value >= std::fabs(segs.front().value(2451545.0) - at_start));
}

TEST_CASE("segments_scalar_splits_when_the_target_demands_it") {
    // A function with a sharp kink needs real work: high-frequency ripple
    // over a long span forces splits.
    const auto sampler = [](double jd, double& v) -> Result<void> {
        const double t = jd - 2451545.0;
        v = 24.0 + 0.5 * std::sin(40.0 * t);
        return {};
    };
    segments::ScalarFitOptions o;
    o.target = 1e-7;
    auto fit = segments::fit_scalar(sampler, 2451545.0, 2451545.0 + 64.0, o);
    REQUIRE_MESSAGE(fit.ok(), fit.error().message);
    CHECK(fit.value().segments.size() >= 4); // 64 days at this frequency splits
    double worst = 0.0;
    for (int i = 0; i <= 6400; ++i) {
        const double jd = 2451545.0 + i * 0.01;
        double want = 0.0;
        REQUIRE(sampler(jd, want).ok());
        for (const segments::ScalarSegment& s : fit.value().segments) {
            if (jd >= s.mid_jd_tt - s.half_span_days && jd <= s.mid_jd_tt + s.half_span_days) {
                worst = std::max(worst, std::fabs(s.value(jd) - want));
                break;
            }
        }
    }
    CHECK(worst <= o.target);
}

TEST_CASE("segments_scalar_refuses_bad_spans_and_reports_sampler_errors") {
    const auto sampler = [](double, double&) -> Result<void> { return {}; };
    segments::ScalarFitOptions o;
    CHECK(!segments::fit_scalar(sampler, 2451545.0, 2451545.0, o).ok());
    CHECK(!segments::fit_scalar(sampler, 2451546.0, 2451545.0, o).ok());
    o.target = 0.0;
    CHECK(!segments::fit_scalar(sampler, 2451545.0, 2451545.5, o).ok());

    const auto broken = [](double, double&) -> Result<void> {
        return make_error(ErrorCode::NotFound, "no such body");
    };
    segments::ScalarFitOptions ok_options;
    auto failed = segments::fit_scalar(broken, 2451545.0, 2451545.5, ok_options);
    REQUIRE(!failed.ok());
    CHECK(failed.error().code == ErrorCode::NotFound);
}

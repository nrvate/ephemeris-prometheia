// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia::segments — Chebyshev fits of a trajectory over a span, with
// the residuals measured rather than claimed (docs/SEGMENTS.md).
//
// A segment holds the coefficients of x, y and z over its own interval, so a
// consumer evaluates any instant inside the span itself, at polynomial cost
// and without the ephemeris. A window a client would otherwise receive as
// tens of thousands of sampled rows becomes a few dozen segments.
//
// This is engine-level and knows nothing about any wire format: the fitter
// takes a sampler and returns segments. prometheiad's transport translates
// at its boundary (docs/SERVER.md).
#ifndef PROMETHEIA_SEGMENTS_HPP
#define PROMETHEIA_SEGMENTS_HPP

#include <cstddef>
#include <functional>
#include <vector>

#include "prometheia/error.hpp"

namespace prometheia::segments {

// One fitted interval: [mid - half_span_days, mid + half_span_days], in TT.
struct Segment {
    double mid_jd_tt = 0.0;
    double half_span_days = 0.0;
    int degree = 0;
    // Chebyshev coefficients of the first kind, degree + 1 each, in AU.
    std::vector<double> x, y, z;
    // The largest residuals measured against the sampler over this segment,
    // on a check set that includes points between the fit's nodes:
    double err_arcsec = 0.0;              // direction
    double err_rel_dist = 0.0;            // |r| , relative
    double err_rate_arcsec_per_day = 0.0; // the analytic derivative against
                                          // the sampler's own rates
    // Position (AU) and velocity (AU/day) at a TT instant inside the segment.
    void position(double jd_tt, double out_au[3]) const;
    void velocity(double jd_tt, double out_au_per_day[3]) const;
};

// What the fitter samples: the rectangular position (AU) and velocity
// (AU/day) of the thing being fitted, at a TT instant, in whatever frame the
// caller wants the segments in. An error stops the fit.
using Sampler = std::function<Result<void>(double jd_tt, double pos_au[3], double vel_au_day[3])>;

struct FitOptions {
    // The direction error asked for. The fitter raises the degree, then
    // splits the interval, to meet it; where it cannot, the segment reports
    // what was measured.
    double target_err_arcsec = 0.1;
    int min_degree = 6;
    int max_degree = 16;              // a consumer's buffer bound
    double min_half_span_days = 0.05; // stop splitting here
    size_t max_segments = 4096;
    // Check points per segment, as a multiple of the coefficient count: the
    // residuals are measured on this set, which includes points between the
    // nodes the fit interpolated.
    int check_multiple = 4;
};

struct FitReport {
    std::vector<Segment> segments;
    size_t sampler_calls = 0; // what the fit cost, in engine evaluations
    double worst_err_arcsec = 0.0;
    bool met_target = true;
};

// Fits [jd_from, jd_to] (TT). Segments are contiguous and ordered: each one's
// end is the next one's start. ArgumentError on an empty span or unusable
// options; the sampler's own error is returned unchanged.
Result<FitReport> fit(const Sampler& sampler, double jd_from, double jd_to,
                      const FitOptions& options = {});

} // namespace prometheia::segments

#endif // PROMETHEIA_SEGMENTS_HPP

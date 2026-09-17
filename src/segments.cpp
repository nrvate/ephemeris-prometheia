// SPDX-License-Identifier: GPL-2.0-or-later
#include "prometheia/segments.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace prometheia::segments {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRadToArcsec = 180.0 / kPi * 3600.0;

// Clenshaw for a Chebyshev series of the first kind at t in [-1, 1].
double clenshaw(const std::vector<double>& c, double t) {
    const size_t n = c.size();
    if (n == 0) {
        return 0.0;
    }
    double b1 = 0.0, b2 = 0.0;
    for (size_t k = n - 1; k >= 1; --k) {
        const double b = 2.0 * t * b1 - b2 + c[k];
        b2 = b1;
        b1 = b;
    }
    return t * b1 - b2 + c[0];
}

// The derivative of that series, d/dt, by the standard recurrence on the
// coefficients (c'_{k-1} = c'_{k+1} + 2k c_k, c'_0 halved).
std::vector<double> derivative(const std::vector<double>& c) {
    const size_t n = c.size();
    if (n <= 1) {
        return {0.0};
    }
    std::vector<double> d(n - 1, 0.0);
    if (n >= 2) {
        d[n - 2] = 2.0 * double(n - 1) * c[n - 1];
    }
    for (size_t k = n - 2; k >= 1; --k) {
        d[k - 1] = (k + 1 < d.size() ? d[k + 1] : 0.0) + 2.0 * double(k) * c[k];
    }
    d[0] *= 0.5;
    return d;
}

// Interpolation at the Chebyshev nodes of the first kind: near-minimax, and
// it costs exactly degree + 1 samples.
void interpolate(const std::vector<double>& values_at_nodes, std::vector<double>& out) {
    const size_t n = values_at_nodes.size();
    out.assign(n, 0.0);
    for (size_t j = 0; j < n; ++j) {
        double sum = 0.0;
        for (size_t k = 0; k < n; ++k) {
            const double theta = kPi * (double(k) + 0.5) / double(n);
            sum += values_at_nodes[k] * std::cos(double(j) * theta);
        }
        out[j] = 2.0 / double(n) * sum;
    }
    out[0] *= 0.5;
}

double norm(const double v[3]) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

} // namespace

void Segment::position(double jd_tt, double out_au[3]) const {
    const double t = (jd_tt - mid_jd_tt) / half_span_days;
    out_au[0] = clenshaw(x, t);
    out_au[1] = clenshaw(y, t);
    out_au[2] = clenshaw(z, t);
}

void Segment::velocity(double jd_tt, double out_au_per_day[3]) const {
    const double t = (jd_tt - mid_jd_tt) / half_span_days;
    const std::vector<double> dx = derivative(x), dy = derivative(y), dz = derivative(z);
    out_au_per_day[0] = clenshaw(dx, t) / half_span_days;
    out_au_per_day[1] = clenshaw(dy, t) / half_span_days;
    out_au_per_day[2] = clenshaw(dz, t) / half_span_days;
}

namespace {

// Samples one interval at the Chebyshev nodes and builds the coefficients.
// The residuals are not measured here: that costs several times as much, and
// the degree is chosen before paying for it.
Result<Segment> fit_coeffs(const Sampler& sampler, double mid, double half, int degree,
                           size_t& calls) {
    const size_t n = size_t(degree) + 1;
    std::vector<double> vx(n), vy(n), vz(n);
    for (size_t k = 0; k < n; ++k) {
        const double t = std::cos(kPi * (double(k) + 0.5) / double(n));
        double p[3], v[3];
        auto r = sampler(mid + t * half, p, v);
        ++calls;
        if (!r) {
            return r.error();
        }
        vx[k] = p[0];
        vy[k] = p[1];
        vz[k] = p[2];
    }
    Segment s;
    s.mid_jd_tt = mid;
    s.half_span_days = half;
    s.degree = degree;
    interpolate(vx, s.x);
    interpolate(vy, s.y);
    interpolate(vz, s.z);
    return s;
}

// The magnitude of the k-th coefficient of the vector series.
double coeff_norm(const Segment& s, size_t k) {
    const double v[3] = {s.x[k], s.y[k], s.z[k]};
    return norm(v);
}

// The degree this interval needs, read off the coefficients already in hand.
//
// A Chebyshev series of a smooth function decays geometrically, so the tail
// beyond degree d — which is what truncating there costs — can be summed from
// the coefficients present and extrapolated past the last one. This is an
// estimate and is never trusted: it only chooses where to spend the check
// set, and the residuals the segment publishes are always measured. Its worth
// is in what it avoids, namely fitting and checking an interval at four or
// five rising degrees, and checking at all an interval that has to be split.
//
// Returns a degree that may exceed max_degree, which is the caller's signal
// to split instead.
int needed_degree(const Segment& s, double target_arcsec, int min_degree, int max_degree) {
    const size_t n = s.x.size();
    if (n < 3) {
        return max_degree;
    }
    double mid_pos[3];
    s.position(s.mid_jd_tt, mid_pos);
    const double dist = norm(mid_pos);
    if (!(dist > 0.0)) {
        return max_degree;
    }
    // Ask the tail for half the target: interpolation aliases, so the true
    // error runs a small factor above the truncation this estimates.
    const double budget_au = 0.5 * target_arcsec / kRadToArcsec * dist;

    const size_t last = n - 1;
    const double a_last = coeff_norm(s, last);
    if (!(a_last > 0.0)) {
        return min_degree; // exact at this degree; nothing more to buy
    }
    // The decay ratio over the last few coefficients, which is where the
    // series is asymptotic. A ratio at or above one means it is not decaying:
    // no degree within reach will do, so ask for a split.
    const size_t back = std::min<size_t>(4, last);
    const double a_back = coeff_norm(s, last - back);
    if (!(a_back > 0.0)) {
        return min_degree;
    }
    const double r = std::pow(a_last / a_back, 1.0 / double(back));
    if (!(r < 0.98)) {
        return max_degree + 8;
    }
    // What truncating at degree d would cost: the coefficients above d that
    // are in hand, plus the geometric continuation beyond the last one. Past
    // the probe's own degree only the continuation is left, and it decays.
    const auto tail_at = [&](int d) {
        double tail = 0.0;
        for (size_t k = size_t(std::max(d, 0)) + 1; k < n; ++k) {
            tail += coeff_norm(s, k);
        }
        const int beyond = std::max(0, d - int(last));
        return tail + a_last * std::pow(r, double(beyond) + 1.0) / (1.0 - r);
    };
    for (int d = min_degree; d <= max_degree; ++d) {
        if (tail_at(d) <= budget_au) {
            return d;
        }
    }
    // Beyond the largest degree allowed: how far beyond decides whether the
    // caller should try that degree anyway or go straight to splitting.
    const double over = tail_at(max_degree);
    const int steps = int(
        std::ceil(std::log(std::max(budget_au, 1e-300) / std::max(over, 1e-300)) / std::log(r)));
    return max_degree + std::max(1, steps);
}

// Measures the residuals of a fitted segment against the sampler.
Result<void> measure(Segment& s, const Sampler& sampler, int check_multiple, size_t& calls) {
    const size_t n = s.x.size();
    const double mid = s.mid_jd_tt, half = s.half_span_days;
    s.err_arcsec = 0.0;
    s.err_rel_dist = 0.0;
    s.err_rate_arcsec_per_day = 0.0;

    // Residuals on a denser set than the fit used, deliberately offset from
    // the nodes so the check sees where the fit is worst rather than where it
    // is exact by construction, and including both endpoints: the derivative's
    // error peaks there, and a check set of interior points alone
    // under-reports the rate residual by a factor of two to three (measured on
    // the Moon).
    const int interior = std::max(4, check_multiple) * int(n) + 1;
    for (int i = 0; i < interior + 2; ++i) {
        const double t = i == interior       ? -1.0
                         : i == interior + 1 ? 1.0
                                             : -1.0 + 2.0 * (double(i) + 0.5) / double(interior);
        const double jd = mid + t * half;
        double p[3], v[3];
        auto r = sampler(jd, p, v);
        ++calls;
        if (!r) {
            return r.error();
        }
        double fp[3], fv[3];
        s.position(jd, fp);
        s.velocity(jd, fv);
        const double d = norm(p);
        if (!(d > 0.0)) {
            return make_error(ErrorCode::ArgumentError, "segment fit: zero position vector");
        }
        const double dv[3] = {fv[0] - v[0], fv[1] - v[1], fv[2] - v[2]};
        // The direction residual is the angle between the fitted and sampled
        // vectors; the distance residual is relative; the rate residual is
        // the velocity difference seen from the observer, per day.
        const double cross[3] = {p[1] * fp[2] - p[2] * fp[1], p[2] * fp[0] - p[0] * fp[2],
                                 p[0] * fp[1] - p[1] * fp[0]};
        const double dot = p[0] * fp[0] + p[1] * fp[1] + p[2] * fp[2];
        s.err_arcsec = std::max(s.err_arcsec, std::atan2(norm(cross), dot) * kRadToArcsec);
        s.err_rel_dist = std::max(s.err_rel_dist, std::fabs(norm(fp) - d) / d);
        s.err_rate_arcsec_per_day =
            std::max(s.err_rate_arcsec_per_day, norm(dv) / d * kRadToArcsec);
    }
    return {};
}

} // namespace

Result<FitReport> fit(const Sampler& sampler, double jd_from, double jd_to,
                      const FitOptions& options) {
    if (!(jd_to > jd_from)) {
        return make_error(ErrorCode::ArgumentError, "segment fit: the span is empty");
    }
    if (!(options.target_err_arcsec > 0.0) || options.min_degree < 1 ||
        options.max_degree < options.min_degree || options.max_degree > 31 ||
        !(options.min_half_span_days > 0.0) || options.max_segments == 0) {
        return make_error(ErrorCode::ArgumentError, "segment fit: unusable options");
    }

    FitReport report;
    // Depth-first over the span, so the segments come out in order: fit an
    // interval at rising degrees; if the target is still unmet, halve it.
    struct Interval {
        double from, to;
    };
    std::vector<Interval> stack;
    stack.push_back({jd_from, jd_to});
    // Neighbouring intervals of the same length want much the same degree, so
    // the degree that worked last time is where the next probe starts.
    int hint = options.min_degree;
    while (!stack.empty()) {
        const Interval in = stack.back();
        stack.pop_back();
        const double half = 0.5 * (in.to - in.from);
        const double mid = 0.5 * (in.to + in.from);
        const bool can_split = half > options.min_half_span_days;

        const int probe = std::clamp(hint, options.min_degree, options.max_degree);
        auto probed = fit_coeffs(sampler, mid, half, probe, report.sampler_calls);
        if (!probed) {
            return probed.error();
        }
        Segment best = std::move(probed).value();
        const int want =
            needed_degree(best, options.target_err_arcsec, options.min_degree, options.max_degree);
        if (want > options.max_degree + 2 && can_split) {
            // Far out of reach at any degree allowed: split without paying for
            // a check set that can only confirm it.
            stack.push_back({mid, in.to});
            stack.push_back({in.from, mid});
            continue;
        }
        int use = std::clamp(want, options.min_degree, options.max_degree);
        // The estimate is read again off the refitted coefficients, which are
        // a better view of the series than the probe's: a correction here
        // costs degree + 1 samples, a correction after the check set costs
        // five times that.
        for (int pass = 0; pass < 2 && use != best.degree; ++pass) {
            auto refit = fit_coeffs(sampler, mid, half, use, report.sampler_calls);
            if (!refit) {
                return refit.error();
            }
            best = std::move(refit).value();
            const int again = needed_degree(best, options.target_err_arcsec, options.min_degree,
                                            options.max_degree);
            use = std::clamp(std::max(again, best.degree), options.min_degree, options.max_degree);
        }
        if (auto m = measure(best, sampler, options.check_multiple, report.sampler_calls); !m) {
            return m.error();
        }
        // The estimate chose the degree; the measurement decides. Where it
        // fell short, raise the degree until it holds or the cap is reached.
        while (best.err_arcsec > options.target_err_arcsec &&
               best.degree + 2 <= options.max_degree) {
            auto more = fit_coeffs(sampler, mid, half, best.degree + 2, report.sampler_calls);
            if (!more) {
                return more.error();
            }
            best = std::move(more).value();
            if (auto m = measure(best, sampler, options.check_multiple, report.sampler_calls); !m) {
                return m.error();
            }
        }
        if (best.err_arcsec > options.target_err_arcsec && can_split) {
            // Split, and keep the halves in time order on the stack.
            stack.push_back({mid, in.to});
            stack.push_back({in.from, mid});
            continue;
        }
        if (report.segments.size() >= options.max_segments) {
            return make_error(ErrorCode::ArgumentError, "segment fit: the span needs more than " +
                                                            std::to_string(options.max_segments) +
                                                            " segments");
        }
        hint = best.degree;
        report.worst_err_arcsec = std::max(report.worst_err_arcsec, best.err_arcsec);
        report.met_target = report.met_target && best.err_arcsec <= options.target_err_arcsec;
        report.segments.push_back(std::move(best));
    }
    return report;
}

} // namespace prometheia::segments

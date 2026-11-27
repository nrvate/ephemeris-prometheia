// SPDX-License-Identifier: GPL-2.0-or-later
#include "prometheia/segments.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace prometheia::segments {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRadToArcsec = 180.0 / kPi * 3600.0;

// Clenshaw for the first n coefficients of a Chebyshev series of the first
// kind at t in [-1, 1].
double clenshaw(const double* c, size_t n, double t) {
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

double clenshaw(const std::vector<double>& c, double t) {
    return clenshaw(c.data(), c.size(), t);
}

// Interpolation at the Chebyshev nodes of the first kind: near-minimax, and
// it costs exactly degree + 1 samples. (The scalar fit's; the vector fit
// shares one table of cosines between its three components, below.)
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

// The nodes of degree n - 1 and cos(j * theta_k) at them, computed once per
// degree for a whole fit and shared by the three components and every refit
// (3 n^2 cosines a fit before). With theta_k = pi (k + 1/2) / n the angle
// j theta_k is a multiple m of pi / 2n, m = j (2k + 1), and a cosine of that
// takes one of 2n + 1 values up to sign, so the table is read off n + 1
// cosines instead of being evaluated n^2 times. The nodes themselves are
// computed as they always were. The coefficients differ from evaluating each
// cosine at j * theta_k only by rounding, about 1e-16 of the series' size.
struct NodeTable {
    std::vector<double> t;   // t[k] = cos(theta_k)
    std::vector<double> cos; // cos[j * n + k] = cos(j * theta_k)
};

class Nodes {
public:
    const NodeTable& of(size_t n) {
        if (tables_.size() <= n) {
            tables_.resize(n + 1);
        }
        NodeTable& tab = tables_[n];
        if (tab.t.empty()) {
            tab.t.resize(n);
            tab.cos.resize(n * n);
            std::vector<double> quarter(n + 1); // cos(pi i / 2n), i = 0..n
            for (size_t i = 0; i <= n; ++i) {
                quarter[i] = i == n ? 0.0 : std::cos(kPi * double(i) / double(2 * n));
            }
            for (size_t k = 0; k < n; ++k) {
                tab.t[k] = std::cos(kPi * (double(k) + 0.5) / double(n));
                for (size_t j = 0; j < n; ++j) {
                    // cos(pi m / 2n) for m = j (2k + 1), by its period 4n,
                    // its evenness and its antisymmetry about pi / 2.
                    size_t m = (j * (2 * k + 1)) % (4 * n);
                    if (m > 2 * n) {
                        m = 4 * n - m;
                    }
                    tab.cos[j * n + k] = m <= n ? quarter[m] : -quarter[2 * n - m];
                }
            }
        }
        return tables_[n];
    }

private:
    std::vector<NodeTable> tables_;
};

void interpolate(const NodeTable& tab, const double* values, std::vector<double>& out) {
    const size_t n = tab.t.size();
    out.assign(n, 0.0);
    for (size_t j = 0; j < n; ++j) {
        double sum = 0.0;
        for (size_t k = 0; k < n; ++k) {
            sum += values[k] * tab.cos[j * n + k];
        }
        out[j] = 2.0 / double(n) * sum;
    }
    out[0] *= 0.5;
}

double norm(const double v[3]) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

} // namespace

// The standard recurrence on the coefficients (c'_{k-1} = c'_{k+1} + 2k c_k,
// c'_0 halved).
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

// One instant the sampler answered: the position, and the rate where it was
// asked for. Every instant of an interval is sampled once, whatever the
// degree it is later judged against.
struct Sample {
    double jd = 0.0;
    double p[3] = {0.0, 0.0, 0.0};
    double v[3] = {0.0, 0.0, 0.0};
    bool has_v = false;
};

// What has been sampled in one interval, by instant. Fit nodes, check points
// and the neighbours' shared ends all land here, so an instant is never paid
// for twice however many degrees it is judged at. One pool serves the whole
// fit and is emptied between intervals, so its storage is allocated once.
// Samples are named by index: the storage may move as it grows.
class Pool {
public:
    Pool(const Sampler& sampler, size_t& calls) : sampler_(sampler), calls_(calls) {
        samples_.reserve(256);
        jds_.reserve(256);
    }

    void clear() {
        samples_.clear();
        jds_.clear();
    }
    void seed(const Sample& s) {
        samples_.push_back(s);
        jds_.push_back(s.jd);
    }
    size_t size() const { return samples_.size(); }
    const Sample& operator[](size_t i) const { return samples_[i]; }

    // The sample at jd, with its rate if want_v. A position-only sample is
    // asked again when its rate is wanted later.
    Result<size_t> get(double jd, bool want_v) {
        const size_t i = index_of(jd);
        if (i < jds_.size()) {
            if (samples_[i].has_v || !want_v) {
                return i;
            }
            return take(i, jd, want_v);
        }
        samples_.emplace_back();
        jds_.push_back(jd);
        return take(samples_.size() - 1, jd, want_v);
    }

    // The sample at jd, or nullptr; valid until the next get().
    const Sample* find(double jd) const {
        const size_t i = index_of(jd);
        return i < jds_.size() ? &samples_[i] : nullptr;
    }

private:
    Result<size_t> take(size_t i, double jd, bool want_v) {
        Sample& s = samples_[i];
        s.jd = jd;
        jds_[i] = jd;
        auto r = sampler_(jd, s.p, want_v ? s.v : nullptr);
        ++calls_;
        if (!r) {
            return r.error();
        }
        s.has_v = want_v;
        return i;
    }

    // The instants alone, packed: the search for one runs over a hundred of
    // them for every point of a check set.
    size_t index_of(double jd) const {
        const size_t n = jds_.size();
        const double* j = jds_.data();
        for (size_t i = 0; i < n; ++i) {
            if (j[i] == jd) {
                return i;
            }
        }
        return n;
    }

    const Sampler& sampler_;
    size_t& calls_;
    std::vector<Sample> samples_;
    std::vector<double> jds_;
};

// The coefficients of the vector series interpolated from `degree + 1`
// samples at the Chebyshev nodes.
Result<Segment> fit_coeffs(Pool& pool, Nodes& nodes, double mid, double half, int degree) {
    const size_t n = size_t(degree) + 1;
    const NodeTable& tab = nodes.of(n);
    double vx[32], vy[32], vz[32]; // max_degree is at most 31
    for (size_t k = 0; k < n; ++k) {
        auto r = pool.get(mid + tab.t[k] * half, false);
        if (!r) {
            return r.error();
        }
        const Sample& smp = pool[r.value()];
        vx[k] = smp.p[0];
        vy[k] = smp.p[1];
        vz[k] = smp.p[2];
    }
    Segment s;
    s.mid_jd_tt = mid;
    s.half_span_days = half;
    s.degree = degree;
    interpolate(tab, vx, s.x);
    interpolate(tab, vy, s.y);
    interpolate(tab, vz, s.z);
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
    // series is asymptotic. A ratio at or above one means it is not decaying;
    // that is a reason to split only if the coefficients are still large. Once
    // they are far under the budget they sit at the rounding floor, where
    // their ratio is noise (a slow body at degree 16): there the tail is the
    // coefficients in hand and a few more of their size, and nothing is out of
    // reach.
    const size_t back = std::min<size_t>(4, last);
    const double a_back = coeff_norm(s, last - back);
    if (!(a_back > 0.0)) {
        return min_degree;
    }
    double r = std::pow(a_last / a_back, 1.0 / double(back));
    if (!(r < 0.98)) {
        if (!(a_last * 64.0 <= budget_au)) {
            return max_degree + 8;
        }
        r = 0.5; // the floor: the continuation is of the order of a_last itself
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

// The check set: N + 1 points at t = -cos(pi i / N), both ends among them and
// the middle too (N is even). They are the extrema of the Chebyshev
// polynomial T_N, so the error of a series, which swings between extrema of
// the same kind, is sampled four times a swing near the interval's ends as
// much as in its middle; evenly spaced points sample the middle twice as
// finely as the ends and under-report the peaks there by a few percent. The
// set belongs to the degree the segment is judged at: a degree raised from the
// same samples gets a denser one.
struct Grid {
    int n = 0;
    double mid = 0.0, half = 0.0, from = 0.0, to = 0.0;

    // The instant of point i, 0..n. The ends are the interval's own: mid +-
    // half can round a double past them, and an interval that ends where the
    // sampler's coverage does would be refused (29% of span lengths,
    // 2026-11-01).
    double jd(int i) const {
        if (i == 0) {
            return from;
        }
        if (i == n) {
            return to;
        }
        if (2 * i == n) {
            return mid; // exactly: a split shares it between the halves
        }
        const int k = i < n - i ? i : n - i;
        const double t = -std::cos(kPi * double(k) / double(n));
        return mid + (i < n - i ? t : -t) * half;
    }
    // The order the points are sampled in: the ends, the middle (which a split
    // hands to both halves), then the rest in time order.
    std::vector<int> order() const {
        std::vector<int> o = {0, n, n / 2};
        for (int i = 1; i < n; ++i) {
            if (i != n / 2) {
                o.push_back(i);
            }
        }
        return o;
    }
};

constexpr int kHintMargin = 2; // the next probe starts this far above the last degree

// The points whose rates are measured: both ends, the two points next to each
// (the grid is dense there), and every quarter of the interval between. The
// derivative's error peaks at the ends (docs/SEGMENTS.md), so they carry the
// bound; the rest guard against a residual that is not the interpolant's smooth
// error, such as noise in the sampler's own rates, which no end reaches.
bool rate_point(int i, int n) {
    if (i <= 2 || i >= n - 2)
        return true;
    for (int k = 1; k <= 3; ++k) // the quarters, the nearest point to each
        if (i == (k * n + 2) / 4)
            return true;
    return false;
}

// The largest value a sequence of samples of a smooth oscillation reaches,
// read off the samples: at each local maximum, the top of the parabola
// through it and its neighbours. The check points are evenly spaced in the
// angle whose cosine is t, where a Chebyshev series swings at a steady pace,
// four points to a half swing; the highest point then under-reports a swing's
// peak by up to 8% (estimate, for a pure sinusoid), and the parabola by much
// less. Never less than the highest sample, and never more than 15% above it.
double peak_of(const std::vector<double>& y) {
    double best = 0.0;
    for (double v : y) {
        best = std::max(best, v);
    }
    for (size_t i = 1; i + 1 < y.size(); ++i) {
        if (y[i] >= y[i - 1] && y[i] >= y[i + 1]) {
            const double curve = y[i - 1] - 2.0 * y[i] + y[i + 1];
            if (curve < 0.0) {
                const double slope = y[i + 1] - y[i - 1];
                best = std::max(best, std::min(y[i] - slope * slope / (8.0 * curve), 1.15 * y[i]));
            }
        }
    }
    return best;
}

// The residuals of the position series (x, y, z cut to `n` coefficients)
// against one sample: the angle between the directions in arcseconds, and the
// relative difference of the distances. False for a zero vector.
bool position_errors(const Segment& s, size_t n, const Sample& smp, double& angle_arcsec,
                     double& rel_dist) {
    const double t = (smp.jd - s.mid_jd_tt) / s.half_span_days;
    const double fp[3] = {clenshaw(s.x.data(), n, t), clenshaw(s.y.data(), n, t),
                          clenshaw(s.z.data(), n, t)};
    const double d = norm(smp.p);
    if (!(d > 0.0)) {
        return false;
    }
    // The direction residual is the angle between the fitted and sampled
    // vectors; the distance residual is relative.
    const double* p = smp.p;
    const double cross[3] = {p[1] * fp[2] - p[2] * fp[1], p[2] * fp[0] - p[0] * fp[2],
                             p[0] * fp[1] - p[1] * fp[0]};
    const double dot = p[0] * fp[0] + p[1] * fp[1] + p[2] * fp[2];
    // atan2 is a fifth of what a check point costs, and the angle is nearly
    // always tiny: its series is exact to rounding below 1e-4 rad.
    const double c = norm(cross);
    const double x = c / dot;
    angle_arcsec =
        (dot > 0.0 && x < 1e-4 ? x * (1.0 - x * x / 3.0) : std::atan2(c, dot)) * kRadToArcsec;
    rel_dist = std::fabs(norm(fp) - d) / d;
    return true;
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
    Nodes nodes;
    Pool pool(sampler, report.sampler_calls);
    std::vector<size_t> seen;
    std::vector<double> grid_angle, grid_rel; // the residuals at the grid's points
    // Depth-first over the span, so the segments come out in order: probe an
    // interval, choose its degree, check it; if the target is still unmet,
    // halve it. An interval may arrive with the samples at its ends already
    // taken (its neighbour's, or its parent's).
    struct Interval {
        double from, to;
        Sample at_from, at_to;
        bool has_from = false, has_to = false;
    };
    std::vector<Interval> stack;
    stack.push_back({jd_from, jd_to, {}, {}, false, false});
    // Neighbouring intervals of the same length want much the same degree, so
    // the degree that worked last time is where the next probe starts.
    int hint = options.probe_degree > 0 ? options.probe_degree : options.max_degree;
    const int check_multiple = std::max(4, options.check_multiple);
    // The most recent interval's far end, for its successor.
    Sample carry;
    bool have_carry = false;
    while (!stack.empty()) {
        Interval in = std::move(stack.back());
        stack.pop_back();
        const double half = 0.5 * (in.to - in.from);
        const double mid = 0.5 * (in.to + in.from);
        const bool can_split = half > options.min_half_span_days;
        if (!in.has_from && have_carry && carry.jd == in.from) {
            in.at_from = carry;
            in.has_from = true;
        }

        pool.clear();
        if (in.has_from) {
            pool.seed(in.at_from);
        }
        if (in.has_to) {
            pool.seed(in.at_to);
        }
        // The ends carry their rates: the rate residual is measured there,
        // and a neighbour or a half starts from them.
        for (double end : {in.from, in.to}) {
            auto r = pool.get(end, true);
            if (!r) {
                return r.error();
            }
        }

        int p = std::clamp(hint, options.min_degree, options.max_degree);
        Segment full;
        int want = 0;
        // Probe at p. If the estimate wants more than the probe has, or
        // cannot tell, fit again at the degree it names (or the largest
        // allowed) with the probe's samples kept as check points; only an
        // interval that is out of reach at the largest degree is split with
        // no check set bought for it.
        for (int pass = 0;; ++pass) {
            auto probed = fit_coeffs(pool, nodes, mid, half, p);
            if (!probed) {
                return probed.error();
            }
            full = std::move(probed).value();
            want = needed_degree(full, options.target_err_arcsec, options.min_degree,
                                 options.max_degree);
            if (want <= p || p >= options.max_degree || pass >= 2) {
                break;
            }
            if (want > options.max_degree + 12 && can_split) {
                break; // hopeless by far: splitting is cheaper than another probe
            }
            p = std::clamp(std::max(want, p + 2), options.min_degree, options.max_degree);
        }
        // The halves share the middle, taken once with its rate, and each
        // keeps its outer end from here.
        const auto split = [&]() -> Result<void> {
            auto m = pool.get(mid, true);
            if (!m) {
                return m.error();
            }
            Interval left{in.from, mid, {}, pool[m.value()], false, true};
            Interval right{mid, in.to, pool[m.value()], {}, true, false};
            if (const Sample* f = pool.find(in.from)) {
                left.at_from = *f;
                left.has_from = true;
            }
            if (const Sample* t = pool.find(in.to)) {
                right.at_to = *t;
                right.has_to = true;
            }
            stack.push_back(std::move(right));
            stack.push_back(std::move(left));
            return {};
        };
        if (want > options.max_degree + 8 && can_split) {
            if (auto r = split(); !r) {
                return r.error();
            }
            continue;
        }

        // The estimate chose the degree; the measurement decides. The series
        // is cut to d coefficients from the probe's p, and d is raised one at
        // a time (from the same samples, at no cost in the sampler) until the
        // check set agrees. Where it fails at p, the probe is repeated higher.
        int d = std::clamp(want, options.min_degree, p);
        bool accepted = false;
        double err_arcsec = 0.0, err_rel = 0.0;
        Grid grid;
        grid.mid = mid;
        grid.half = half;
        grid.from = in.from;
        grid.to = in.to;
        while (true) {
            // Where the target cannot be missed twice over -- the largest
            // degree on an interval that cannot be split -- the segment is
            // reported as measured, so the whole set is taken.
            const bool may_abort = can_split || p < options.max_degree;
            // Everything sampled so far is a check point: the probe's nodes
            // are, off the grid as they are.
            seen.clear();
            for (size_t i = 0; i < pool.size(); ++i) {
                seen.push_back(i);
            }
            double worst = 0.0, worst_rel = 0.0;
            bool zero = false;
            const auto refold = [&] {
                worst = worst_rel = 0.0;
                for (size_t i : seen) {
                    double a, rd;
                    if (!position_errors(full, size_t(d) + 1, pool[i], a, rd)) {
                        zero = true;
                        return;
                    }
                    worst = std::max(worst, a);
                    worst_rel = std::max(worst_rel, rd);
                }
            };
            // Raise d one at a time, from the same samples, while over target.
            const auto raise = [&] {
                refold();
                while (!zero && worst > options.target_err_arcsec && d < p) {
                    ++d;
                    refold();
                }
            };
            raise();
            bool failed = !zero && worst > options.target_err_arcsec && may_abort;
            for (bool regrid = !failed && !zero; regrid;) {
                regrid = false;
                grid.n = 2 * ((check_multiple * (d + 1) + 1) / 2);
                grid_angle.assign(size_t(grid.n) + 1, 0.0);
                grid_rel.assign(size_t(grid.n) + 1, 0.0);
                const int d_at_start = d;
                for (int i : grid.order()) {
                    const size_t before = pool.size();
                    auto got = pool.get(grid.jd(i), false);
                    if (!got) {
                        return got.error();
                    }
                    if (pool.size() != before) {
                        seen.push_back(got.value()); // else it is in `seen` already
                    }
                    double a, rd;
                    if (!position_errors(full, size_t(d) + 1, pool[got.value()], a, rd)) {
                        zero = true;
                        break;
                    }
                    grid_angle[size_t(i)] = a;
                    grid_rel[size_t(i)] = rd;
                    worst = std::max(worst, a);
                    worst_rel = std::max(worst_rel, rd);
                    if (worst > options.target_err_arcsec && d < p) {
                        raise();
                    }
                    if (zero) {
                        break;
                    }
                    if (worst > options.target_err_arcsec && may_abort) {
                        failed = true;
                        break;
                    }
                    if (d != d_at_start) {
                        regrid = true; // a denser set for the higher degree
                        break;
                    }
                }
                if (failed || zero || regrid) {
                    continue;
                }
                // The set is complete at this degree. The peaks between its
                // points are read off the points around them, and the segment
                // is judged on what they say.
                worst = std::max(worst, peak_of(grid_angle));
                worst_rel = std::max(worst_rel, peak_of(grid_rel));
                if (worst > options.target_err_arcsec) {
                    if (d < p) {
                        ++d;
                        raise();
                        regrid = !zero;
                    } else if (may_abort) {
                        failed = true;
                    }
                }
            }
            if (zero) {
                return make_error(ErrorCode::ArgumentError, "segment fit: zero position vector");
            }
            if (!failed) {
                accepted = true;
                err_arcsec = worst;
                err_rel = worst_rel;
                break;
            }
            if (p >= options.max_degree) {
                break;
            }
            // Short at p: probe higher, keeping what was sampled.
            const int from_d = std::min(d + 1, options.max_degree);
            p = std::clamp(std::max(from_d, p + 2), options.min_degree, options.max_degree);
            auto more = fit_coeffs(pool, nodes, mid, half, p);
            if (!more) {
                return more.error();
            }
            full = std::move(more).value();
            d = std::min(from_d, p);
        }
        if (!accepted) {
            if (auto r = split(); !r) {
                return r.error();
            }
            continue;
        }

        Segment best = std::move(full);
        best.degree = d;
        best.x.resize(size_t(d) + 1);
        best.y.resize(size_t(d) + 1);
        best.z.resize(size_t(d) + 1);
        best.err_arcsec = err_arcsec;
        best.err_rel_dist = err_rel;
        // The rate residual, on the subset of the check set where it is
        // measured, against the sampler's own rates.
        const std::vector<double> dx = derivative(best.x), dy = derivative(best.y),
                                  dz = derivative(best.z);
        for (int i = 0; i <= grid.n; ++i) {
            if (!rate_point(i, grid.n)) {
                continue;
            }
            auto got = pool.get(grid.jd(i), true);
            if (!got) {
                return got.error();
            }
            const Sample& s = pool[got.value()];
            const double t = (s.jd - best.mid_jd_tt) / best.half_span_days;
            const double fv[3] = {clenshaw(dx, t) / best.half_span_days,
                                  clenshaw(dy, t) / best.half_span_days,
                                  clenshaw(dz, t) / best.half_span_days};
            const double dv[3] = {fv[0] - s.v[0], fv[1] - s.v[1], fv[2] - s.v[2]};
            best.err_rate_arcsec_per_day =
                std::max(best.err_rate_arcsec_per_day, norm(dv) / norm(s.p) * kRadToArcsec);
        }
        if (report.segments.size() >= options.max_segments) {
            return make_error(ErrorCode::ArgumentError, "segment fit: the span needs more than " +
                                                            std::to_string(options.max_segments) +
                                                            " segments");
        }
        hint = std::min(best.degree + kHintMargin, options.max_degree);
        carry = *pool.find(in.to);
        have_carry = true;
        report.worst_err_arcsec = std::max(report.worst_err_arcsec, best.err_arcsec);
        report.met_target = report.met_target && best.err_arcsec <= options.target_err_arcsec;
        report.segments.push_back(std::move(best));
    }
    return report;
}

// ---- scalar series -----------------------------------------------------------

namespace {

// The degree the coefficient tail says suffices for `target`, by the same
// contract as needed_degree: never trusted, only spent on choosing where the
// check set goes. A 1-D series' tail is the suffix sum of |c_k|, continued
// geometrically past the last coefficient in hand.
int needed_degree_1d(const std::vector<double>& c, double target, int lo, int hi) {
    const int last = int(c.size()) - 1;
    if (last < 2) {
        return hi;
    }
    std::vector<double> tail(size_t(last) + 2, 0.0); // tail[k] = sum_{j>=k} |c_j|
    for (int k = last; k >= 0; --k) {
        tail[size_t(k)] = tail[size_t(k) + 1] + std::fabs(c[size_t(k)]);
    }
    const double a_last = tail[size_t(last)];
    if (!(a_last > 0.0)) {
        return lo; // exact at this degree
    }
    const int back = std::min(4, last);
    const double a_back = tail[size_t(last - back)];
    if (!(a_back > 0.0)) {
        return lo;
    }
    const double r = std::pow(a_last / a_back, 1.0 / double(back));
    if (!(r < 0.98)) {
        return hi + 8; // not decaying: no degree within reach
    }
    for (int d = lo; d <= hi; ++d) {
        double sum = d + 1 <= last ? tail[size_t(d + 1)] : 0.0;
        if (d >= last) {
            sum += a_last * std::pow(r, double(d - last) + 1.0) / (1.0 - r);
        }
        if (sum <= 0.5 * target) {
            return d;
        }
    }
    return hi + 1;
}

Result<ScalarSegment> fit_scalar_coeffs(const ScalarSampler& sampler, double mid, double half,
                                        int degree, size_t& calls) {
    const size_t n = size_t(degree) + 1;
    std::vector<double> v(n);
    for (size_t k = 0; k < n; ++k) {
        const double t = std::cos(kPi * (double(k) + 0.5) / double(n));
        double x = 0.0;
        auto r = sampler(mid + t * half, x);
        ++calls;
        if (!r) {
            return r.error();
        }
        v[k] = x;
    }
    ScalarSegment s;
    s.mid_jd_tt = mid;
    s.half_span_days = half;
    s.degree = degree;
    interpolate(v, s.c);
    return s;
}

// Measured on interior points offset from the nodes and both endpoints: the
// same rule as the vector fit's measure(), for a series whose error is not an
// angle but a difference of values.
Result<void> measure_scalar(ScalarSegment& s, const ScalarSampler& sampler, double from, double to,
                            int check_multiple, size_t& calls) {
    const size_t n = s.c.size();
    const double mid = s.mid_jd_tt, half = s.half_span_days;
    s.err_value = 0.0;
    const int interior = std::max(4, check_multiple) * int(n) + 1;
    for (int i = 0; i < interior + 2; ++i) {
        const double t = i == interior       ? -1.0
                         : i == interior + 1 ? 1.0
                                             : -1.0 + 2.0 * (double(i) + 0.5) / double(interior);
        double x = 0.0;
        auto r = sampler(i == interior ? from : i == interior + 1 ? to : mid + t * half, x);
        ++calls;
        if (!r) {
            return r.error();
        }
        s.err_value = std::max(s.err_value, std::fabs(clenshaw(s.c, t) - x));
    }
    return {};
}

} // namespace

double ScalarSegment::value(double jd_tt) const {
    return clenshaw(c, (jd_tt - mid_jd_tt) / half_span_days);
}

Result<ScalarFitReport> fit_scalar(const ScalarSampler& sampler, double jd_from, double jd_to,
                                   const ScalarFitOptions& options) {
    if (!(jd_to > jd_from)) {
        return make_error(ErrorCode::ArgumentError, "scalar fit: the span is empty");
    }
    if (!(options.target > 0.0) || options.min_degree < 1 ||
        options.max_degree < options.min_degree || options.max_degree > 31 ||
        !(options.min_half_span_days > 0.0) || options.max_segments == 0) {
        return make_error(ErrorCode::ArgumentError, "scalar fit: unusable options");
    }
    ScalarFitReport report;
    struct Interval {
        double from, to;
    };
    std::vector<Interval> stack{{jd_from, jd_to}};
    int hint = options.min_degree;
    while (!stack.empty()) {
        const Interval in = stack.back();
        stack.pop_back();
        const double half = 0.5 * (in.to - in.from);
        const double mid = 0.5 * (in.to + in.from);
        const bool can_split = half > options.min_half_span_days;
        const int probe = std::clamp(hint, options.min_degree, options.max_degree);
        auto probed = fit_scalar_coeffs(sampler, mid, half, probe, report.sampler_calls);
        if (!probed) {
            return probed.error();
        }
        ScalarSegment best = std::move(probed).value();
        const int want =
            needed_degree_1d(best.c, options.target, options.min_degree, options.max_degree);
        int use = std::clamp(want, options.min_degree, options.max_degree);
        if (use != best.degree) {
            auto refit = fit_scalar_coeffs(sampler, mid, half, use, report.sampler_calls);
            if (!refit) {
                return refit.error();
            }
            best = std::move(refit).value();
        }
        if (auto m = measure_scalar(best, sampler, in.from, in.to, options.check_multiple,
                                    report.sampler_calls);
            !m) {
            return m.error();
        }
        while (best.err_value > options.target && best.degree + 2 <= options.max_degree) {
            auto more =
                fit_scalar_coeffs(sampler, mid, half, best.degree + 2, report.sampler_calls);
            if (!more) {
                return more.error();
            }
            best = std::move(more).value();
            if (auto m = measure_scalar(best, sampler, in.from, in.to, options.check_multiple,
                                        report.sampler_calls);
                !m) {
                return m.error();
            }
        }
        if (best.err_value > options.target && can_split) {
            stack.push_back({mid, in.to});
            stack.push_back({in.from, mid});
            continue;
        }
        if (report.segments.size() >= options.max_segments) {
            return make_error(ErrorCode::ArgumentError, "scalar fit: the span needs more than " +
                                                            std::to_string(options.max_segments) +
                                                            " segments");
        }
        hint = best.degree;
        report.segments.push_back(std::move(best));
    }
    return report;
}

} // namespace prometheia::segments

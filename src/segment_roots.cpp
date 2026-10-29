// SPDX-License-Identifier: GPL-2.0-or-later
//
// Roots on segment handles (docs/SEGMENTS.md, "Roots").
//
// The window is cut into pieces, each where one segment of a and one of b
// overlap, and every piece is searched whole whatever the window, so a root
// is the same double from any window that holds it. On a piece f is smooth:
// it is interpolated at Chebyshev points (degree kN, halving until the
// series' tail is negligible), and Chebyshev bounds on the interpolant's
// first and second derivatives cut it into runs on which f is monotone,
// with at most one extremum between two runs. A run's roots are counted
// from the values at its two ends, and each is found by a bracketed solve
// on f itself, never on the interpolant.
#include "segment_roots.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace prometheia::segment_roots {
namespace {

constexpr double kPi = 3.14159265358979323846; // M_PI is not ISO C++
constexpr double kR = 180.0 / kPi;
constexpr int kN = 16;                  // interpolation degree on a cell
constexpr int kMaxSolveSteps = 200;     // per root; bisection alone needs ~60
constexpr int64_t kMaxLevels = 1 << 20; // levels one run may cross
// Roots are solved at least this finely, so that slope, err_days and the
// tangent flag, read at the root, do not depend on a coarse tol_days.
constexpr double kSolveDays = 1e-6;
constexpr double kRounding = 1e-13; // a series' tail that is only rounding, relative

// Clenshaw's recurrence over n coefficients, as prometheia_segments_eval.
double clenshaw(const double* c, int n, double u) {
    double b1 = 0.0, b2 = 0.0;
    for (int k = n - 1; k >= 1; --k) {
        const double b = 2.0 * u * b1 - b2 + c[k];
        b2 = b1;
        b1 = b;
    }
    return u * b1 - b2 + c[0];
}

// d/du of a Chebyshev series of kN + 1 terms.
void derivative(const double* c, double* d) {
    d[kN] = 0.0;
    d[kN - 1] = 2.0 * kN * c[kN];
    for (int k = kN - 1; k >= 2; --k)
        d[k - 1] = d[k + 1] + 2.0 * k * c[k];
    d[0] = 0.5 * (kN >= 2 ? d[2] + 2.0 * c[1] : 2.0 * c[1]);
}

// How far a series stays from 0 over [-1, 1] at the least: |c0| less the
// rest. Positive only if it never vanishes there.
double floor_bound(const double* c) {
    double rest = 0.0;
    for (int k = 1; k <= kN; ++k)
        rest += std::fabs(c[k]);
    return std::fabs(c[0]) - rest;
}

double wrap(double x, double m) {
    return x - m * std::floor(x / m + 0.5);
}

struct Value {
    double v; // the component
    double d; // its derivative per day; NaN for a speed
};

// A component of handle segment i at t, from the fitted vector, with the
// spherical conversion of prometheia_segments_eval.
Value raw(const prometheia_segments_layout& l, int i, int comp, double t) {
    const double u = (t - l.mid_jd_tt[i]) / l.half_span_days[i];
    const int n = l.degree[i] + 1;
    double p[3], v[3];
    for (int c = 0; c < 3; ++c) {
        const size_t at = size_t(3 * i + c) * size_t(l.stride);
        p[c] = clenshaw(l.coef + at, n, u);
        v[c] = clenshaw(l.dcoef + at, n, u) / l.half_span_days[i];
    }
    const double x = p[0], y = p[1], z = p[2], vx = v[0], vy = v[1], vz = v[2];
    const double rho2 = x * x + y * y;
    const double r2 = rho2 + z * z;
    const double rho = std::sqrt(rho2);
    const double lon_speed = (x * vy - y * vx) / rho2 * kR;
    const double lat_speed = (vz * rho2 - z * (x * vx + y * vy)) / (r2 * rho) * kR;
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    switch (comp) {
    case PROMETHEIA_SEGMENTS_LON: {
        double lon = std::atan2(y, x) * kR;
        if (lon < 0.0)
            lon += 360.0;
        return {lon, lon_speed};
    }
    case PROMETHEIA_SEGMENTS_LAT:
        return {std::atan2(z, rho) * kR, lat_speed};
    case PROMETHEIA_SEGMENTS_LON_SPEED:
        return {lon_speed, kNaN};
    default:
        return {lat_speed, kNaN};
    }
}

// A segment of a term: handle h, segment i.
struct Seg {
    int h = 0, i = 0;
};

// One term: its handles back to back, its coefficient and component.
struct Term {
    const prometheia_segments* const* hs = nullptr;
    int n = 0;
    double c = 0.0;
    int comp = 0;

    const prometheia_segments_layout& L(int h) const {
        return *prometheia_segments_get_layout(hs[h]);
    }
    double lo(Seg s) const { return L(s.h).bounds_jd_tt[s.i]; }
    double hi(Seg s) const { return L(s.h).bounds_jd_tt[s.i + 1]; }
    bool has_prev(Seg s) const { return s.i > 0 || s.h > 0; }
    bool has_next(Seg s) const { return s.i + 1 < L(s.h).count || s.h + 1 < n; }
    Seg prev(Seg s) const {
        return s.i > 0 ? Seg{s.h, s.i - 1} : Seg{s.h - 1, L(s.h - 1).count - 1};
    }
    Seg next(Seg s) const { return s.i + 1 < L(s.h).count ? Seg{s.h, s.i + 1} : Seg{s.h + 1, 0}; }
    Value at(Seg s, double t) const { return raw(L(s.h), s.i, comp, t); }

    // The segment a search from t starts in: lo < t <= hi, or the first.
    Seg locate(double t) const {
        int h = 0;
        while (h + 1 < n && L(h).jd_tt_end < t)
            ++h;
        const prometheia_segments_layout& l = L(h);
        const double* first = l.bounds_jd_tt + 1;
        const double* last = l.bounds_jd_tt + l.count;
        return {h, int(std::lower_bound(first, last, t) - first)};
    }

    // The continuous value at the bound between p and the segment after it:
    // halfway between the two sides. One function of the two, so both
    // segments meet at the same double.
    double join(Seg p, Seg q, double t) const {
        const double vp = at(p, t).v, vq = at(q, t).v;
        const double step = comp == PROMETHEIA_SEGMENTS_LON ? wrap(vp - vq, 360.0) : vp - vq;
        return vq + 0.5 * step;
    }
};

// A term on one segment, made continuous: half the step at each bound is
// spread linearly over the segment, so the two sides meet at join().
struct Cursor {
    const Term* term = nullptr;
    Seg seg;
    double b0 = 0.0, b1 = 0.0;
    double join0 = 0.0, join1 = 0.0;
    double corr0 = 0.0, corr1 = 0.0;
    bool has0 = false, has1 = false;

    void enter(const Term& t, Seg s) {
        term = &t;
        seg = s;
        b0 = t.lo(s);
        b1 = t.hi(s);
        has0 = t.has_prev(s);
        has1 = t.has_next(s);
        corr0 = corr1 = 0.0;
        const bool lon = t.comp == PROMETHEIA_SEGMENTS_LON;
        if (has0) {
            join0 = t.join(t.prev(s), s, b0);
            const double d = join0 - t.at(s, b0).v;
            corr0 = lon ? wrap(d, 360.0) : d;
        }
        if (has1) {
            join1 = t.join(s, t.next(s), b1);
            const double d = join1 - t.at(s, b1).v;
            corr1 = lon ? wrap(d, 360.0) : d;
        }
    }

    Value value(double t) const {
        const double slope = (corr1 - corr0) / (b1 - b0);
        if (t == b0 && has0)
            return {join0, term->at(seg, t).d + slope};
        if (t == b1 && has1)
            return {join1, term->at(seg, t).d + slope};
        const Value r = term->at(seg, t);
        const double w = (t - b0) / (b1 - b0);
        return {r.v + corr0 + (corr1 - corr0) * w, r.d + slope};
    }
};

// An end of a run: f there, as gw (f reduced to [-M/2, M/2), or f itself
// without a modulus) and k, the turns of M the cell's continuous branch adds.
struct End {
    double t = 0.0;
    double gw = 0.0;
    int64_t k = 0;
};

class Search {
public:
    Search(const Function& f, double from, double to, double tol, prometheia_segments_root* out,
           int capacity)
        : f_(f), from_(from), to_(to), tol_(std::min(tol, kSolveDays)), out_(out),
          capacity_(capacity) {
        a_ = {f.a, f.na, f.ca, f.component};
        if (f.nb > 0)
            b_ = {f.b, f.nb, f.cb, f.component};
        m_ = f.modulus;
        speed_ = f.component == PROMETHEIA_SEGMENTS_LON_SPEED ||
                 f.component == PROMETHEIA_SEGMENTS_LAT_SPEED;
        auto tol_of = [&](const Term& t) {
            double w = 0.0;
            for (int h = 0; h < t.n; ++h) {
                const prometheia_segments_layout& l = t.L(h);
                w = std::max(w, speed_ ? l.err_rate_arcsec_per_day : l.tolerance_arcsec);
            }
            return std::fabs(t.c) * w / 3600.0;
        };
        band_ = tol_of(a_) + (f.nb > 0 ? tol_of(b_) : 0.0);
        band_ = std::max(band_, 1e-15);
        eps_ = 0.01 * band_;
        for (int m = 0; m < 2 * kN; ++m)
            cos_[m] = std::cos(kPi * m / kN);
    }

    void run() {
        Seg sa = a_.locate(from_);
        Seg sb = f_.nb > 0 ? b_.locate(from_) : Seg{};
        for (;;) {
            ca_.enter(a_, sa);
            double p0 = ca_.b0, p1 = ca_.b1;
            if (f_.nb > 0) {
                cb_.enter(b_, sb);
                p0 = std::max(p0, cb_.b0);
                p1 = std::min(p1, cb_.b1);
            }
            cells_ = 0;
            last_ = -std::numeric_limits<double>::infinity();
            cell(p0, p1, 0);
            if (stop_ || p1 >= to_)
                return;
            if (ca_.b1 == p1) {
                if (!a_.has_next(sa))
                    return;
                sa = a_.next(sa);
            }
            if (f_.nb > 0 && cb_.b1 == p1) {
                if (!b_.has_next(sb))
                    return;
                sb = b_.next(sb);
            }
        }
    }

    int count() const { return count_; }
    int unresolved_cells() const { return unresolved_; }
    double resume() const { return resume_; }

private:
    // f reduced (gw) and its derivative per day (NaN for a speed).
    Value eval(double t) const {
        const Value va = ca_.value(t);
        double v = a_.c * va.v, d = a_.c * va.d;
        if (f_.nb > 0) {
            const Value vb = cb_.value(t);
            v += b_.c * vb.v;
            d += b_.c * vb.d;
        }
        v -= f_.target;
        if (f_.target_rate != 0.0) {
            v -= f_.target_rate * (t - f_.target_jd_tt);
            d -= f_.target_rate;
        }
        if (m_ > 0.0)
            v = wrap(v, m_);
        return {v, d};
    }

    // The interpolant of one cell: f on its continuous branch, and its first
    // two derivatives, all in u on [-1, 1].
    struct Interp {
        double mid = 0.0, half = 0.0;
        double c[kN + 1]{}, d1[kN + 1]{}, d2[kN + 1]{};
        double u(double t) const { return std::clamp((t - mid) / half, -1.0, 1.0); }
        double p(double t) const { return clenshaw(c, kN + 1, u(t)); }
        double dp(double t) const { return clenshaw(d1, kN + 1, u(t)) / half; }
        double ddp(double t) const { return clenshaw(d2, kN + 1, u(t)) / (half * half); }
    };

    End end_at(double t, double gw, double branch) const {
        End e{t, gw, 0};
        if (m_ > 0.0)
            e.k = std::llround((branch - gw) / m_);
        return e;
    }

    void cell(double t0, double t1, int depth) {
        if (stop_)
            return;
        ++cells_;
        Interp ip;
        ip.mid = 0.5 * (t0 + t1);
        ip.half = 0.5 * (t1 - t0);
        double g[kN + 1], gw[kN + 1];
        // Nodes cos(pi j / kN), j = 0 at t1; taken in time order, from j = kN.
        Value prev{};
        double tprev = t0;
        for (int j = kN; j >= 0; --j) {
            const double t = j == kN ? t0 : j == 0 ? t1 : ip.mid + ip.half * cos_[j];
            const Value v = eval(t);
            gw[j] = v.v;
            if (m_ > 0.0 && j < kN) {
                // The continuous branch: the step predicted by the rate.
                const double pred = g[j + 1] + 0.5 * (prev.d + v.d) * (t - tprev);
                g[j] = pred + wrap(v.v - pred, m_);
            } else {
                g[j] = v.v;
            }
            prev = v;
            tprev = t;
        }
        for (int k = 0; k <= kN; ++k) {
            double s = 0.5 * (g[0] + ((k & 1) ? -g[kN] : g[kN]));
            for (int j = 1; j < kN; ++j)
                s += g[j] * cos_[(j * k) % (2 * kN)];
            ip.c[k] = s * (2.0 / kN);
        }
        ip.c[0] *= 0.5;
        ip.c[kN] *= 0.5;
        const bool can_split = depth < f_.limits.max_depth && cells_ < f_.limits.max_cells &&
                               t1 - t0 > 2.0 * f_.limits.min_cell_days;
        const double tail = std::fabs(ip.c[kN - 1]) + std::fabs(ip.c[kN]);
        // Below this the tail is rounding, however small the band.
        double size = 0.0;
        for (int k = 0; k <= kN; ++k)
            size += std::fabs(ip.c[k]);
        if (tail > std::max(eps_, kRounding * size)) {
            if (can_split) {
                cell(t0, ip.mid, depth + 1);
                cell(ip.mid, t1, depth + 1);
                return;
            }
            unresolved(t0, t1); // the interpolant, and so its bounds, are not to be trusted
        }
        derivative(ip.c, ip.d1);
        derivative(ip.d1, ip.d2);
        const End e0 = end_at(t0, gw[kN], g[kN]);
        const End e1 = end_at(t1, gw[0], g[0]);
        const double margin1 = 2.0 * (std::fabs(ip.d1[kN - 1]) + std::fabs(ip.d1[kN - 2]));
        if (floor_bound(ip.d1) > margin1) {
            span(e0, e1, ip);
            return;
        }
        const double margin2 = 2.0 * (std::fabs(ip.d2[kN - 2]) + std::fabs(ip.d2[kN - 3]));
        if (floor_bound(ip.d2) > margin2) {
            // f' is monotone: one extremum at most, where it changes sign.
            double ua = -1.0, ub = 1.0;
            double fa = clenshaw(ip.d1, kN + 1, ua), fb = clenshaw(ip.d1, kN + 1, ub);
            if (!(fa * fb < 0.0)) {
                span(e0, e1, ip);
                return;
            }
            for (int it = 0; it < 80 && ub - ua > 1e-15; ++it) {
                const double um = 0.5 * (ua + ub);
                const double fm = clenshaw(ip.d1, kN + 1, um);
                if ((fm < 0.0) == (fa < 0.0)) {
                    ua = um;
                    fa = fm;
                } else {
                    ub = um;
                }
            }
            const double ts = std::clamp(ip.mid + ip.half * 0.5 * (ua + ub), t0, t1);
            if (ts <= t0 || ts >= t1) {
                span(e0, e1, ip);
                return;
            }
            const Value vs = eval(ts);
            const End es = end_at(ts, vs.v, ip.p(ts));
            span(e0, es, ip);
            touch(es, vs, ip, ip.d2[0] < 0.0);
            span(es, e1, ip);
            return;
        }
        if (can_split) {
            cell(t0, ip.mid, depth + 1);
            cell(ip.mid, t1, depth + 1);
            return;
        }
        unresolved(t0, t1);
        span(e0, e1, ip); // structure given up: the ends decide
    }

    // The levels n M crossed from a to b, each solved for in time order.
    // f on a run is monotone; a root exactly at b belongs to this run, one
    // exactly at a to the run before.
    void span(const End& a, const End& b, const Interp& ip) {
        const double rise = double(b.k - a.k) * m_ + (b.gw - a.gw);
        if (rise > 0.0) {
            const int64_t first = a.k + (a.gw >= 0.0 ? 0 : -1) + 1;
            const int64_t last = b.k + (b.gw >= 0.0 ? 0 : -1);
            for (int64_t n = first; n <= last && n - first < kMaxLevels && !stop_; ++n)
                solve(a, b, n, ip);
        } else if (rise < 0.0) {
            const int64_t first = a.k + (a.gw > 0.0 ? 1 : 0) - 1;
            const int64_t last = b.k + (b.gw > 0.0 ? 1 : 0);
            for (int64_t n = first; n >= last && first - n < kMaxLevels && !stop_; --n)
                solve(a, b, n, ip);
        }
    }

    // f less the level n M, on the cell's branch.
    double level_gap(double t, double gw, int64_t n, const Interp& ip) const {
        if (m_ <= 0.0)
            return gw;
        const int64_t k = std::llround((ip.p(t) - gw) / m_);
        return double(k - n) * m_ + gw;
    }

    void solve(const End& ea, const End& eb, int64_t n, const Interp& ip) {
        double a = ea.t, b = eb.t;
        double fa = double(ea.k - n) * m_ + ea.gw;
        double fb = double(eb.k - n) * m_ + eb.gw;
        double t = b;
        if (fb != 0.0) {
            int side = 0;
            bool bisect = false;
            double width = b - a;
            for (int it = 0; it < kMaxSolveSteps && b - a > tol_; ++it) {
                double x = bisect ? 0.5 * (a + b) : (a * fb - b * fa) / (fb - fa);
                if (!(x > a && x < b))
                    x = 0.5 * (a + b);
                const double fx = level_gap(x, eval(x).v, n, ip);
                if (fx == 0.0) {
                    a = b = x;
                    break;
                }
                if ((fx < 0.0) == (fa < 0.0)) {
                    a = x;
                    fa = fx;
                    if (side == -1)
                        fb *= 0.5; // Illinois
                    side = -1;
                } else {
                    b = x;
                    fb = fx;
                    if (side == 1)
                        fa *= 0.5;
                    side = 1;
                }
                bisect = b - a > 0.5 * width;
                width = b - a;
            }
            t = 0.5 * (a + b);
        }
        const Value v = eval(t);
        const double slope = speed_ ? ip.dp(t) : v.d;
        const double curv = ip.ddp(t);
        const bool tangent = slope * slope <= 2.0 * std::fabs(curv) * band_;
        const double err =
            tangent ? std::sqrt(2.0 * band_ / std::fabs(curv)) : band_ / std::fabs(slope);
        put({t, slope, err + tol_, tangent ? 1 : 0});
    }

    // An extremum at s that comes within the band of a level without
    // reaching it: a root of its own, tangent.
    void touch(const End& s, const Value& v, const Interp& ip, bool maximum) {
        constexpr double kInf = std::numeric_limits<double>::infinity();
        double gap;
        if (maximum)
            gap = s.gw < 0.0 ? -s.gw : m_ > 0.0 ? m_ - s.gw : kInf;
        else
            gap = s.gw > 0.0 ? s.gw : m_ > 0.0 ? m_ + s.gw : kInf;
        if (!(gap <= band_))
            return;
        const double slope = speed_ ? ip.dp(s.t) : v.d;
        const double curv = std::fabs(ip.ddp(s.t));
        const double err = curv > 0.0 ? std::sqrt(2.0 * band_ / curv) : kInf;
        put({s.t, slope, err + tol_, 1});
    }

    // A cell that ended at a limit: its roots are counted from its ends, so
    // a cancelling pair inside it may be lost. Counted when it meets the
    // window.
    void unresolved(double t0, double t1) {
        if (t1 > from_ && t0 < to_)
            ++unresolved_;
    }

    void put(prometheia_segments_root r) {
        // Roots strictly increase: two levels solved to a coarse tol_days
        // could otherwise share an instant. Only this piece's roots count,
        // so the answer still does not depend on the window.
        if (!(r.jd_tt > last_))
            r.jd_tt = std::nextafter(last_, std::numeric_limits<double>::infinity());
        last_ = r.jd_tt;
        if (r.jd_tt >= to_) {
            stop_ = true; // roots come in time order: none after this one counts
            return;
        }
        if (r.jd_tt < from_)
            return;
        if (capacity_ == 0) {
            ++count_;
            return;
        }
        if (count_ == capacity_) {
            resume_ = r.jd_tt;
            stop_ = true;
            return;
        }
        out_[count_++] = r;
    }

    const Function& f_;
    double from_, to_, tol_;
    prometheia_segments_root* out_;
    int capacity_;
    Term a_, b_;
    Cursor ca_, cb_;
    double m_ = 0.0, band_ = 0.0, eps_ = 0.0;
    bool speed_ = false;
    double cos_[2 * kN]{};
    int cells_ = 0;
    double last_ = 0.0; // the piece's last root
    int count_ = 0;
    int unresolved_ = 0;
    double resume_ = std::numeric_limits<double>::quiet_NaN();
    bool stop_ = false;
};

} // namespace

int find(const Function& f, double from, double to, double tol_days, prometheia_segments_root* out,
         int capacity, double* resume, int* unresolved) noexcept {
    Search s(f, from, to, tol_days, out, capacity);
    s.run();
    *resume = s.resume();
    *unresolved = s.unresolved_cells();
    return s.count();
}

} // namespace prometheia::segment_roots

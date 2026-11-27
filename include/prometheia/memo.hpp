// SPDX-License-Identifier: GPL-2.0-or-later
//
// Windowed per-body memo. One integration from the seed produces a
// sampled trajectory (cubic-Hermite evaluation) covering a window; every
// later query inside the window costs a spline evaluation. Coverage grows
// by marching window-by-window from the seed (forward or backward), so
// repeated queries amortize to one integration pass over the arc actually
// visited. Deliberately single-threaded — the engine (M4) owns per-context
// memo instances, mirroring the no-shared-mutable-state rule. What contexts
// share is the windows themselves, through a WindowShare: a window is a
// function of the body and the windows before it, so one integrated by any
// memo is the same doubles for all of them.
#ifndef PROMETHEIA_MEMO_HPP
#define PROMETHEIA_MEMO_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <prometheia/forces.hpp>
#include <prometheia/integrator.hpp>
#include <prometheia/trajectory.hpp>

namespace prometheia {

// Where a memo finds the windows another memo integrated, and leaves its
// own. A window is named by a track (key), its direction and its count from
// the seed, and its samples are those a memo appends to that run. Any thread
// may call these at once; a miss is the memo integrating, as without a share.
class WindowShare {
public:
    using Window = std::vector<TrajSample>;
    virtual ~WindowShare() = default;
    virtual std::shared_ptr<const Window> find(uint64_t key, bool forward, size_t index) = 0;
    virtual void keep(uint64_t key, bool forward, size_t index, Window samples) = 0;
};

// Force-generic: the engine instantiates it with BarycentricForce; the
// default keeps the M2-era HeliocentricForce call sites unchanged.
template <typename Force = HeliocentricForce>
class WindowMemo {
public:
    struct Config {
        double window_days = 365.25; // cache-line width
        // Hermite samples per window. The evaluation error is ~(n*dt)^4/384*r,
        // so density is the accuracy knob: 128/year keeps a 1 AU, 1 yr window
        // near 2e-9 AU; raise for long windows or high-eccentricity bodies.
        int min_samples = 128;
        // A close encounter happens inside one of those segments, and a cubic
        // through its ends misses it (Apophis in 2029, by 2,300 km: docs/
        // ENGINE.md, "Small bodies"). So each segment's error is estimated,
        // from the accelerations already in hand, and one past refine_au
        // gets samples of its own, down to refined_au, at most 2^refine_depth
        // to the segment. Its end state stays as integrated, so nothing
        // outside it moves.
        double refine_au = 1e-8;
        double refined_au = 1e-12;
        int refine_depth = 12;
    };

    WindowMemo(Force* force, IntegrateOptions opts) : WindowMemo(force, opts, Config{}) {}

    WindowMemo(Force* force, IntegrateOptions opts, Config cfg)
        : force_(force), opts_(std::move(opts)), cfg_(cfg) {}

    // Windows are looked for in `share` under `key` before integrating, and
    // left there after. The key must name one seed, force and set of limits.
    void set_share(WindowShare* share, uint64_t key) {
        share_ = share;
        share_key_ = key;
    }

    // The reference state the memo propagates from. Resets all coverage.
    void set_seed(const State& s, double t) {
        seed_ = s;
        seed_t_ = t;
        lo_ = hi_ = t;
        fwd_.clear();
        bwd_.clear();
        fwd_windows_ = bwd_windows_ = 0;
        fwd_.push_back(make_sample(t, s));
        bwd_.push_back(make_sample(t, s));
    }

    // The span the force model can be evaluated over (the planetary and
    // perturber files'). A window that would cross a limit ends at it, so
    // an instant inside the span is reached however the window grid falls;
    // past a limit at() integrates nothing and coverage stops there.
    void set_limits(double lo, double hi) {
        limit_lo_ = lo;
        limit_hi_ = hi;
    }

    // Position/velocity at time t; integrates only when coverage doesn't
    // already answer it.
    State at(double t) {
        ++stats_.evals;
        if (t >= lo_ && t <= hi_) {
            ++stats_.cache_hits;
            return eval_from_samples(t);
        }
        ++stats_.cache_misses;
        if (t > hi_) {
            while (t > hi_) {
                if (!extend_forward()) {
                    return {}; // integration failed; stats_ tell the story
                }
            }
        } else {
            while (t < lo_) {
                if (!extend_backward()) {
                    return {};
                }
            }
        }
        return eval_from_samples(t);
    }

    struct Stats {
        uint64_t evals = 0;
        uint64_t cache_hits = 0;
        uint64_t cache_misses = 0;
        uint64_t windows_built = 0;  // integrated by this memo
        uint64_t windows_shared = 0; // taken from the share
        uint64_t segments_refined = 0;
        uint64_t accel_evals = 0;
    };

    const Stats& stats() const { return stats_; }
    double coverage_lo() const { return lo_; }
    double coverage_hi() const { return hi_; }

private:
    static TrajSample make_sample(double t, const State& s) {
        TrajSample p;
        p.t = t;
        p.px = s.pos.x;
        p.py = s.pos.y;
        p.pz = s.pos.z;
        p.vx = s.vel.x;
        p.vy = s.vel.y;
        p.vz = s.vel.z;
        return p;
    }

    // Samples live in two runs that meet at the seed, each only ever
    // appended to: fwd_ ascending from it, bwd_ descending from it. (One
    // ascending vector had every backward window copy all of it, which made
    // a backward walk quadratic: 36% of a 1,000-instant query, 2026-09-19.)
    // The segment around t is the same pair of samples either way, so the
    // values are too.
    State eval_from_samples(double t) const {
        if (t >= seed_t_ || bwd_.size() < 2) {
            size_t lo = 0, hi = fwd_.size();
            while (lo + 1 < hi) {
                const size_t mid = (lo + hi) / 2;
                if (fwd_[mid].t <= t) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            if (lo + 1 >= fwd_.size())
                return sample_to_state(fwd_.back());
            return hermite(fwd_[lo], fwd_[lo + 1], t);
        }
        // bwd_[0] is the seed, later than t: the last sample later than t
        // and the one after it bracket t.
        size_t lo = 0, hi = bwd_.size() - 1;
        while (lo + 1 < hi) {
            const size_t mid = (lo + hi) / 2;
            if (bwd_[mid].t > t) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        return hermite(bwd_[lo + 1], bwd_[lo], t);
    }

    // Cubic Hermite between samples a and b (a earlier).
    static State hermite(const TrajSample& a, const TrajSample& b, double t) {
        const double dt = b.t - a.t;
        if (dt == 0.0)
            return sample_to_state(a);
        const double u = (t - a.t) / dt;
        const double u2 = u * u, u3 = u2 * u;
        const double h00 = 2 * u3 - 3 * u2 + 1;
        const double h10 = u3 - 2 * u2 + u;
        const double h01 = -2 * u3 + 3 * u2;
        const double h11 = u3 - u2;
        State out;
        out.pos = Vec3(h00 * a.px + h10 * dt * a.vx + h01 * b.px + h11 * dt * b.vx,
                       h00 * a.py + h10 * dt * a.vy + h01 * b.py + h11 * dt * b.vy,
                       h00 * a.pz + h10 * dt * a.vz + h01 * b.pz + h11 * dt * b.vz);
        const double d00 = (6 * u2 - 6 * u) / dt;
        const double d10 = (3 * u2 - 4 * u + 1);
        const double d01 = (-6 * u2 + 6 * u) / dt;
        const double d11 = (3 * u2 - 2 * u);
        out.vel = Vec3(d00 * a.px + d10 * a.vx + d01 * b.px + d11 * b.vx,
                       d00 * a.py + d10 * a.vy + d01 * b.py + d11 * b.vy,
                       d00 * a.pz + d10 * a.vz + d01 * b.pz + d11 * b.vz);
        return out;
    }

    static State sample_to_state(const TrajSample& p) {
        State s;
        s.pos = Vec3(p.px, p.py, p.pz);
        s.vel = Vec3(p.vx, p.vy, p.vz);
        return s;
    }

    // Integrates one window from `from` to `to` (either direction), replacing
    // the sample list with a uniformly densified version over [from, to].
    // The cubic's position error over a segment, estimated from how far its
    // second derivative at each end is from the acceleration there. Over a
    // span h with fourth derivative k the two are k h^2 / 12 and k h^4 / 384
    // apart, so one is the other times h^2 / 32.
    static double cubic_error(const TrajSample& a, const double acc_a[3], const TrajSample& b,
                              const double acc_b[3]) {
        const double h = b.t - a.t;
        const double dp[3] = {b.px - a.px, b.py - a.py, b.pz - a.pz};
        const double va[3] = {a.vx, a.vy, a.vz}, vb[3] = {b.vx, b.vy, b.vz};
        double worst = 0.0;
        for (int i = 0; i < 3; ++i) {
            const double at_a = 6.0 * dp[i] / (h * h) - (4.0 * va[i] + 2.0 * vb[i]) / h;
            const double at_b = -6.0 * dp[i] / (h * h) + (2.0 * va[i] + 4.0 * vb[i]) / h;
            worst = std::max({worst, std::fabs(at_a - acc_a[i]), std::fabs(at_b - acc_b[i])});
        }
        return worst * h * h / 32.0;
    }

    // Samples from a to `to`, until the cubics through them are good to
    // refined_au, appended to `out` in order, the last one left to the caller
    // unless it is inside the segment. They lie on one continuous trajectory:
    // y and acc carry the state from each sample to the next, for two
    // integrations that met only at their ends would disagree by the
    // integrator's own tolerance, a few decimetres, and at 50,000 km that is a
    // milliarcsecond of wiggle between samples.
    bool refine(double* y, double* acc, TrajSample& a, double to, bool last, int depth,
                std::vector<TrajSample>& out, IntegrateStats& istats) {
        double trial[6];
        std::copy(y, y + 6, trial);
        IntegrateOptions opts = opts_;
        opts.first_step = to - a.t;
        if (!integrate_dp54(trial, a.t, to, *force_, opts, &istats).ok())
            return false;
        double d[6];
        (*force_)(trial, to, d);
        const TrajSample b =
            make_sample(to, State{Vec3(trial[0], trial[1], trial[2]), Vec3(d[0], d[1], d[2])});
        if (depth >= cfg_.refine_depth || !(cubic_error(a, acc, b, d + 3) > cfg_.refined_au)) {
            if (!last)
                out.push_back(b);
            std::copy(trial, trial + 6, y);
            std::copy(d + 3, d + 6, acc);
            a = b;
            return true;
        }
        const double tm = 0.5 * (a.t + to);
        return refine(y, acc, a, tm, false, depth + 1, out, istats) &&
               refine(y, acc, a, to, last, depth + 1, out, istats);
    }

    bool integrate_window(double from, double to, bool forward) {
        std::vector<TrajSample>& run = forward ? fwd_ : bwd_;
        size_t& windows = forward ? fwd_windows_ : bwd_windows_;
        if (share_) {
            if (auto w = share_->find(share_key_, forward, windows)) {
                run.insert(run.end(), w->begin(), w->end());
                (forward ? hi_ : lo_) = to;
                ++windows;
                ++stats_.windows_shared;
                return true;
            }
        }
        const int nseg = std::max(cfg_.min_samples, 1);
        double y[6];
        State s = eval_from_samples(forward ? hi_ : lo_);
        y[0] = s.pos.x;
        y[1] = s.pos.y;
        y[2] = s.pos.z;
        y[3] = s.vel.x;
        y[4] = s.vel.y;
        y[5] = s.vel.z;

        std::vector<TrajSample> fresh;
        fresh.reserve(size_t(nseg) + 1);
        // Seed sample for this window (velocity from the force at `from`).
        double acc[3];
        {
            double d[6];
            (*force_)(y, from, d);
            std::copy(d + 3, d + 6, acc);
            TrajSample p;
            p.t = from;
            p.px = y[0];
            p.py = y[1];
            p.pz = y[2];
            p.vx = d[0];
            p.vy = d[1];
            p.vz = d[2];
            fresh.push_back(p);
        }

        IntegrateStats istats;
        const double step = (to - from) / double(nseg);
        IntegrateOptions seg = opts_;
        if (seg.first_step == 0.0)
            seg.first_step = step;
        for (int i = 0; i < nseg; ++i) {
            const double ta = from + step * double(i);
            const double tb = from + step * double(i + 1);
            double y_start[6];
            std::copy(y, y + 6, y_start);
            auto r = integrate_dp54(y, ta, tb, *force_, seg, &istats);
            if (!r.ok())
                return false;
            double d[6];
            (*force_)(y, tb, d);
            TrajSample p;
            p.t = tb;
            p.px = y[0];
            p.py = y[1];
            p.pz = y[2];
            p.vx = d[0];
            p.vy = d[1];
            p.vz = d[2];
            if (cfg_.refine_au > 0.0 && cubic_error(fresh.back(), acc, p, d + 3) > cfg_.refine_au) {
                ++stats_.segments_refined;
                std::vector<TrajSample> inside;
                TrajSample start = fresh.back();
                double acc_start[3];
                std::copy(acc, acc + 3, acc_start);
                if (!refine(y_start, acc_start, start, tb, true, 0, inside, istats))
                    return false;
                fresh.insert(fresh.end(), inside.begin(), inside.end());
            }
            fresh.push_back(p);
            std::copy(d + 3, d + 6, acc);
        }

        // fresh[0] duplicates the run's last sample; the rest continue it
        // (ascending forward, descending backward).
        run.insert(run.end(), fresh.begin() + 1, fresh.end());
        if (share_)
            share_->keep(share_key_, forward, windows,
                         WindowShare::Window(fresh.begin() + 1, fresh.end()));
        (forward ? hi_ : lo_) = to;
        ++windows;
        ++stats_.windows_built;
        stats_.accel_evals += istats.accel_evals;
        return true;
    }

    bool extend_forward() {
        if (!(hi_ < limit_hi_))
            return false;
        const double to = std::min(hi_ + cfg_.window_days, limit_hi_);
        return integrate_window(hi_, to, true);
    }

    bool extend_backward() {
        if (!(lo_ > limit_lo_))
            return false;
        const double to = std::max(lo_ - cfg_.window_days, limit_lo_);
        return integrate_window(lo_, to, false);
    }

    Force* force_;
    IntegrateOptions opts_;
    Config cfg_;
    State seed_{};
    double seed_t_ = 0.0;
    double lo_ = 0.0, hi_ = 0.0;
    double limit_lo_ = -std::numeric_limits<double>::infinity();
    double limit_hi_ = std::numeric_limits<double>::infinity();
    std::vector<TrajSample> fwd_, bwd_;
    size_t fwd_windows_ = 0, bwd_windows_ = 0; // windows in each run
    WindowShare* share_ = nullptr;
    uint64_t share_key_ = 0;
    Stats stats_;
};

} // namespace prometheia

#endif // PROMETHEIA_MEMO_HPP

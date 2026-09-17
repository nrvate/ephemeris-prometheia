// SPDX-License-Identifier: GPL-2.0-or-later
//
// Windowed per-body memo. One integration from the seed produces a
// sampled trajectory (cubic-Hermite evaluation) covering a window; every
// later query inside the window costs a spline evaluation. Coverage grows
// by marching window-by-window from the seed (forward or backward), so
// repeated queries amortize to one integration pass over the arc actually
// visited. Deliberately single-threaded — the engine (M4) owns per-context
// memo instances, mirroring the no-shared-mutable-state rule.
#ifndef PROMETHEIA_MEMO_HPP
#define PROMETHEIA_MEMO_HPP

#include <cmath>
#include <cstdint>
#include <vector>

#include <prometheia/forces.hpp>
#include <prometheia/integrator.hpp>
#include <prometheia/trajectory.hpp>

namespace prometheia {

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
    };

    WindowMemo(Force* force, IntegrateOptions opts) : WindowMemo(force, opts, Config{}) {}

    WindowMemo(Force* force, IntegrateOptions opts, Config cfg)
        : force_(force), opts_(std::move(opts)), cfg_(cfg) {}

    // The reference state the memo propagates from. Resets all coverage.
    void set_seed(const State& s, double t) {
        seed_ = s;
        seed_t_ = t;
        lo_ = hi_ = t;
        samples_.clear();
        samples_.push_back(make_sample(t, s));
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
        uint64_t windows_built = 0;
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

    State eval_from_samples(double t) const {
        // Samples are in integration order (forward); a backward extension
        // prepends, keeping ascending order. Binary search the segment.
        size_t lo = 0, hi = samples_.size();
        while (lo + 1 < hi) {
            const size_t mid = (lo + hi) / 2;
            if (samples_[mid].t <= t) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        if (lo + 1 >= samples_.size())
            return sample_to_state(samples_.back());
        const TrajSample& a = samples_[lo];
        const TrajSample& b = samples_[lo + 1];
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
    bool integrate_window(double from, double to, bool forward) {
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
        {
            double d[6];
            (*force_)(y, from, d);
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
        for (int i = 0; i < nseg; ++i) {
            const double ta = from + step * double(i);
            const double tb = from + step * double(i + 1);
            auto r = integrate_dp54(y, ta, tb, *force_, opts_, &istats);
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
            fresh.push_back(p);
        }

        if (forward) {
            // fresh[0] duplicates the old tail sample's time; drop it and append
            // the rest (already ascending).
            for (size_t i = 1; i < fresh.size(); ++i)
                samples_.push_back(fresh[i]);
            hi_ = to;
        } else {
            // fresh runs descending in time (to -> lo_); drop its first (dups the
            // old head) and prepend the rest reversed so storage stays ascending.
            std::vector<TrajSample> merged;
            merged.reserve(samples_.size() + fresh.size());
            for (size_t i = fresh.size(); i-- > 1;)
                merged.push_back(fresh[i]);
            for (const TrajSample& p : samples_)
                merged.push_back(p);
            samples_ = std::move(merged);
            lo_ = to;
        }
        ++stats_.windows_built;
        stats_.accel_evals += istats.accel_evals;
        return true;
    }

    bool extend_forward() {
        const double w = cfg_.window_days;
        const double to = hi_ + w;
        return integrate_window(hi_, to, true);
    }

    bool extend_backward() {
        const double w = cfg_.window_days;
        const double to = lo_ - w;
        return integrate_window(lo_, to, false);
    }

    Force* force_;
    IntegrateOptions opts_;
    Config cfg_;
    State seed_{};
    double seed_t_ = 0.0;
    double lo_ = 0.0, hi_ = 0.0;
    std::vector<TrajSample> samples_;
    Stats stats_;
};

} // namespace prometheia

#endif // PROMETHEIA_MEMO_HPP

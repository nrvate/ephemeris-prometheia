// SPDX-License-Identifier: GPL-2.0-or-later
//
// Piecewise cubic-Hermite trajectory. The perturber table: planetary
// positions are sampled once (from any State source) and evaluated at
// O(1) cost during force evaluations, replacing per-eval analytic
// propagation. Cubic-Hermite error is O(dt^4); with a few hundred
// segments over decades the perturbing-acceleration error lands orders
// of magnitude below every accuracy tier this library promises.
#ifndef PROMETHEIA_TRAJECTORY_HPP
#define PROMETHEIA_TRAJECTORY_HPP

#include <algorithm>
#include <cmath>
#include <vector>

#include <prometheia/error.hpp>
#include <prometheia/kepler.hpp>

namespace prometheia {

struct TrajSample {
  double t = 0.0;
  double px = 0.0, py = 0.0, pz = 0.0;
  double vx = 0.0, vy = 0.0, vz = 0.0;
};

class Trajectory {
 public:
  Trajectory() = default;

  // Samples must be sorted by strictly ascending t (>= 2 samples).
  static Result<Trajectory> from_samples(std::vector<TrajSample> samples) {
    if (samples.size() < 2) {
      return make_error(ErrorCode::ArgumentError, "trajectory needs >= 2 samples");
    }
    for (size_t i = 1; i < samples.size(); ++i) {
      if (!(samples[i].t > samples[i - 1].t)) {
        return make_error(ErrorCode::ArgumentError, "sample times not ascending");
      }
      for (double v : {samples[i].px, samples[i].py, samples[i].pz, samples[i].vx,
                       samples[i].vy, samples[i].vz}) {
        if (!std::isfinite(v)) {
          return make_error(ErrorCode::ArgumentError, "non-finite sample");
        }
      }
    }
    Trajectory tr;
    tr.pts_ = std::move(samples);
    return tr;
  }

  // Uniform sampling from any callable State(double t) -> State.
  template <typename Source>
  static Result<Trajectory> sample_uniform(Source&& src, double t0, double t1,
                                           int segments) {
    if (segments < 1 || !(t1 > t0)) {
      return make_error(ErrorCode::ArgumentError, "bad sampling bounds");
    }
    std::vector<TrajSample> pts;
    pts.reserve(size_t(segments) + 1);
    for (int i = 0; i <= segments; ++i) {
      const double t = t0 + (t1 - t0) * double(i) / double(segments);
      const State s = src(t);
      TrajSample p;
      p.t = t;
      p.px = s.pos.x; p.py = s.pos.y; p.pz = s.pos.z;
      p.vx = s.vel.x; p.vy = s.vel.y; p.vz = s.vel.z;
      pts.push_back(p);
    }
    return from_samples(std::move(pts));
  }

  // Hermite cubic evaluation; clamps to the end samples outside the span.
  State eval(double t) const {
    const size_t n = pts_.size();
    if (n == 0) return {};
    if (t <= pts_.front().t) return sample_state(0);
    if (t >= pts_[n - 1].t) return sample_state(n - 1);
    // Rightmost sample with time <= t.
    size_t hi = size_t(std::upper_bound(pts_.begin(), pts_.end(), t,
                                        [](double v, const TrajSample& s) {
                                          return v < s.t;
                                        }) -
                       pts_.begin());
    const size_t seg = hi - 1;
    const TrajSample& a = pts_[seg];
    const TrajSample& b = pts_[hi];
    const double dt = b.t - a.t;
    const double u = (t - a.t) / dt;

    // Cubic Hermite basis.
    const double u2 = u * u, u3 = u2 * u;
    const double h00 = 2 * u3 - 3 * u2 + 1;
    const double h10 = u3 - 2 * u2 + u;
    const double h01 = -2 * u3 + 3 * u2;
    const double h11 = u3 - u2;
    State out;
    out.pos = Vec3(h00 * a.px + h10 * dt * a.vx + h01 * b.px + h11 * dt * b.vx,
                   h00 * a.py + h10 * dt * a.vy + h01 * b.py + h11 * dt * b.vy,
                   h00 * a.pz + h10 * dt * a.vz + h01 * b.pz + h11 * dt * b.vz);
    // Derivative of the same basis with respect to t (chain rule /dt).
    const double d00 = (6 * u2 - 6 * u) / dt;
    const double d10 = (3 * u2 - 4 * u + 1);
    const double d01 = (-6 * u2 + 6 * u) / dt;
    const double d11 = (3 * u2 - 2 * u);
    out.vel = Vec3(d00 * a.px + d10 * a.vx + d01 * b.px + d11 * b.vx,
                   d00 * a.py + d10 * a.vy + d01 * b.py + d11 * b.vy,
                   d00 * a.pz + d10 * a.vz + d01 * b.pz + d11 * b.vz);
    return out;
  }

  double t_min() const { return pts_.empty() ? 0.0 : pts_.front().t; }
  double t_max() const { return pts_.empty() ? 0.0 : pts_.back().t; }
  size_t sample_count() const { return pts_.size(); }

 private:
  State sample_state(size_t i) const {
    State s;
    s.pos = Vec3(pts_[i].px, pts_[i].py, pts_[i].pz);
    s.vel = Vec3(pts_[i].vx, pts_[i].vy, pts_[i].vz);
    return s;
  }

  std::vector<TrajSample> pts_;
};

}  // namespace prometheia

#endif  // PROMETHEIA_TRAJECTORY_HPP

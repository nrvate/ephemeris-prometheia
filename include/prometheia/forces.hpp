// SPDX-License-Identifier: GPL-2.0-or-later
//
// Force models for small-body integration. v1 is heliocentric: the central
// mass at the origin, perturbers on Trajectory tables. When the DE reader
// lands (M3) the same interface takes barycentric truth; nothing downstream
// changes.
#ifndef PROMETHEIA_FORCES_HPP
#define PROMETHEIA_FORCES_HPP

#include <vector>

#include <prometheia/trajectory.hpp>

namespace prometheia {

struct Perturber {
  double mu = 0.0;             // GM of the perturber (AU^3/day^2)
  const Trajectory* traj = nullptr;  // not owned
};

// Heliocentric N-body point-mass force: central mu at origin plus moving
// perturbers. dydt[0..2] = velocity, dydt[3..5] = acceleration.
struct HeliocentricForce {
  double mu_central = 0.0;
  const std::vector<Perturber>* perturbers = nullptr;

  void operator()(const double y[6], double t, double dydt[6]) const {
    const double r2 = y[0] * y[0] + y[1] * y[1] + y[2] * y[2];
    const double inv_r3 = 1.0 / (r2 * std::sqrt(r2));
    dydt[0] = y[3];
    dydt[1] = y[4];
    dydt[2] = y[5];
    const double f = -mu_central * inv_r3;
    double ax = f * y[0], ay = f * y[1], az = f * y[2];
    for (const Perturber& p : *perturbers) {
      const State ps = p.traj->eval(t);
      const double dx = ps.pos.x - y[0], dy = ps.pos.y - y[1],
                   dz = ps.pos.z - y[2];
      const double d2 = dx * dx + dy * dy + dz * dz;
      const double inv_d3 = p.mu / (d2 * std::sqrt(d2));
      ax += inv_d3 * dx;
      ay += inv_d3 * dy;
      az += inv_d3 * dz;
    }
    dydt[3] = ax;
    dydt[4] = ay;
    dydt[5] = az;
  }
};

}  // namespace prometheia

#endif  // PROMETHEIA_FORCES_HPP

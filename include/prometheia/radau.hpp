// SPDX-License-Identifier: GPL-2.0-or-later
//
// integrate_radau15 — an IAS15-class adaptive integrator: 15th-order
// implicit collocation on Gauss-Radau spacings (Everhart 1985; Rein &
// Spiegel 2015, arXiv:1409.4779).
//
// Cleanroom provenance of the constants: the seven free substep fractions
// are the roots of the Jacobi polynomial P_7^{(1,0)} mapped to [0,1] and
// reflected (1 - x), computed from the standard three-term recurrence —
// verified against the four most significant digits published in the
// IAS15 paper's footnote and Everhart's table. The collocation structure,
// error criterion (highest b-coefficient relative to the acceleration
// scale) and step controller (dt * (eps_b/ratio)^(1/7), iteration
// convergence at 1e-16 with an oscillation break) follow the published
// descriptions. Nothing is derived from any implementation's source.
#ifndef PROMETHEIA_RADAU_HPP
#define PROMETHEIA_RADAU_HPP

#include <prometheia/error.hpp>
#include <prometheia/integrator.hpp>

namespace prometheia {

struct Radau15Options {
  // Dimensionless smoothness target from the IAS15 paper; 1e-9 is the
  // published conservative default.
  double eps_b = 1e-9;
  double first_step = 0.0;    // 0 = automatic (fraction of the span)
  double max_step = 0.0;      // 0 = unlimited
  int max_iterations = 12;    // corrector cap (paper's value)
};

struct Radau15Stats {
  uint64_t steps = 0;
  uint64_t accel_evals = 0;
  uint64_t rejected_steps = 0;
  uint64_t corrector_iterations = 0;
  double final_ratio = 0.0;  // last accepted step's b8/a scale
};

// Same calling convention as integrate_dp54: y in/out over [t0, t1], a
// ForceModel callable, optional per-accepted-step sink.
template <typename Force, typename Sink = NoSink>
Result<void> integrate_radau15(double* y, double t0, double t1, Force&& accel,
                               const Radau15Options& opts, Radau15Stats* stats,
                               Sink&& sink = Sink{});

}  // namespace prometheia

#include <prometheia/radau.inl>

#endif  // PROMETHEIA_RADAU_HPP
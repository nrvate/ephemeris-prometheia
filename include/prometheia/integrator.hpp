// SPDX-License-Identifier: GPL-2.0-or-later
//
// Adaptive numerical integration for the mechanics core.
//
// Increment 1 ships Dormand-Prince 5(4) with FSAL (the standard embedded
// RK5(4) pair): well-understood coefficients, dense-enough accuracy for
// validation, and a ForceModel-template interface that keeps later
// integrators (IAS15-class, next increment) swappable without touching
// call sites. State is [x y z vx vy vz]; the force model fills the
// acceleration part of the derivative.
#ifndef PROMETHEIA_INTEGRATOR_HPP
#define PROMETHEIA_INTEGRATOR_HPP

#include <cmath>
#include <cstdint>

#include <prometheia/error.hpp>

namespace prometheia {

// A force model is any callable with signature
//   void accel(const double y[6], double t, double dydt[6])
// where dydt[0..2] = velocity, dydt[3..5] = acceleration(y, t).
// CONTRACT: y and dydt must not alias — implementations commonly write
// dydt[0..2] (the velocity copy) before evaluating the acceleration from
// y[0..2].

// Optional per-accepted-step sink (memo cache construction). Called with
// the accepted (t, y, dydt) after each successful step.
struct NoSink {
  void operator()(double, const double*, const double*) const {}
};

struct IntegrateStats {
  uint64_t steps = 0;
  uint64_t accel_evals = 0;
  uint64_t rejected_steps = 0;
  double final_error_estimate = 0.0;  // scaled RMS of the last accepted step
};

struct IntegrateOptions {
  double rtol = 1e-12;   // scaled relative tolerance
  double atol = 1e-30;   // absolute floor (positions in AU: negligible)
  double max_step = 0.0; // 0 = unlimited
  double first_step = 0.0;  // 0 = automatic
};

// Integrates y over [t0, t1] with the given force model. On success y holds
// the final state. Throws nothing; reports via Result.
template <typename Force, typename Sink = NoSink>
Result<void> integrate_dp54(double* y, double t0, double t1, Force&& accel,
                            const IntegrateOptions& opts, IntegrateStats* stats,
                            Sink&& sink = Sink{});

}  // namespace prometheia

#include <prometheia/integrator.inl>

#endif  // PROMETHEIA_INTEGRATOR_HPP

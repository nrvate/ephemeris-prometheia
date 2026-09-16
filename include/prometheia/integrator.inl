// SPDX-License-Identifier: GPL-2.0-or-later
// Template implementation of the DP5(4) integrator — included by
// integrator.hpp. Not for direct inclusion elsewhere.
#ifndef PROMETHEIA_INTEGRATOR_INL
#define PROMETHEIA_INTEGRATOR_INL

#include <algorithm>

namespace prometheia {

namespace dp54 {

// Dormand-Prince RK5(4) coefficients (Dormand & Prince 1980; the standard
// FSAL embedded pair used by MATLAB's ode45, SciPy's RK45, et al.).
constexpr double kC[7] = {0.0, 1.0 / 5.0, 3.0 / 10.0, 4.0 / 5.0, 8.0 / 9.0, 1.0, 1.0};

constexpr double kA[7][7] = {
    {0, 0, 0, 0, 0, 0, 0},
    {1.0 / 5.0, 0, 0, 0, 0, 0, 0},
    {3.0 / 40.0, 9.0 / 40.0, 0, 0, 0, 0, 0},
    {44.0 / 45.0, -56.0 / 15.0, 32.0 / 9.0, 0, 0, 0, 0},
    {19372.0 / 6561.0, -25360.0 / 2187.0, 64448.0 / 6561.0, -212.0 / 729.0, 0, 0, 0},
    {9017.0 / 3168.0, -355.0 / 33.0, 46732.0 / 5247.0, 49.0 / 176.0,
     -5103.0 / 18656.0, 0, 0},
    {35.0 / 384.0, 0, 500.0 / 1113.0, 125.0 / 192.0, -2187.0 / 6784.0, 11.0 / 84.0,
     0},
};

// 5th-order solution weights (= kA[6] with FSAL reuse).
constexpr double kB5[7] = {35.0 / 384.0, 0.0, 500.0 / 1113.0, 125.0 / 192.0,
                           -2187.0 / 6784.0, 11.0 / 84.0, 0.0};

// 4th-order embedded solution (for the error estimate).
constexpr double kB4[7] = {5179.0 / 57600.0, 0.0, 7571.0 / 16695.0, 393.0 / 640.0,
                           -92097.0 / 339200.0, 187.0 / 2100.0, 1.0 / 40.0};

}  // namespace dp54

template <typename Force>
Result<void> integrate_dp54(double* y, double t0, double t1, Force&& accel,
                            const IntegrateOptions& opts, IntegrateStats* stats) {
  if (t1 == t0) return {};
  if (opts.rtol <= 0.0 || !std::isfinite(t0) || !std::isfinite(t1)) {
    return make_error(ErrorCode::ArgumentError, "bad integration bounds/tolerance");
  }

  IntegrateStats local;
  double t = t0;
  const double direction = (t1 > t0) ? 1.0 : -1.0;
  const double span = t1 - t0;

  double k[7][6];
  double ytmp[6];
  double y5[6];

  accel(y, t, k[0]);  // FSAL seed
  ++local.accel_evals;

  // span is signed; the default step inherits its sign. A caller-provided
  // first_step is used verbatim (caller owns its sign).
  double h = opts.first_step != 0.0 ? opts.first_step : span / 100.0;

  while ((t1 - t) * direction > 0.0) {
    if ((t + h - t1) * direction > 0.0) h = t1 - t;
    if (opts.max_step > 0.0 && std::fabs(h) > opts.max_step) {
      h = direction * opts.max_step;
    }

    // Stages 2..7.
    for (int s = 1; s < 7; ++s) {
      for (int i = 0; i < 6; ++i) {
        double acc = 0.0;
        for (int j = 0; j < s; ++j) acc += dp54::kA[s][j] * k[j][i];
        ytmp[i] = y[i] + h * acc;
      }
      accel(ytmp, t + dp54::kC[s] * h, k[s]);
      ++local.accel_evals;
    }

    // 5th-order candidate, then the embedded scaled-RMS error estimate.
    for (int i = 0; i < 6; ++i) {
      double acc = 0.0;
      for (int j = 0; j < 7; ++j) acc += dp54::kB5[j] * k[j][i];
      y5[i] = y[i] + h * acc;
    }
    double err2 = 0.0;
    for (int i = 0; i < 6; ++i) {
      double ei = 0.0;
      for (int j = 0; j < 7; ++j) {
        ei += h * (dp54::kB5[j] - dp54::kB4[j]) * k[j][i];
      }
      const double scale =
          opts.rtol * std::max(std::fabs(y[i]), std::fabs(y5[i])) + opts.atol;
      const double ratio = ei / scale;
      err2 += ratio * ratio;
    }
    const double err = std::sqrt(err2 / 6.0);

    if (err <= 1.0 || std::fabs(h) <= 1e-14 * std::fabs(span)) {
      for (int i = 0; i < 6; ++i) y[i] = y5[i];
      t += h;
      ++local.steps;
      local.final_error_estimate = err;
      // FSAL: kB5 == kA[6], so k[6] is the derivative at the accepted
      // (t+h, y) — reuse it as the next step's first stage.
      for (int i = 0; i < 6; ++i) k[0][i] = k[6][i];

      double factor = (err == 0.0) ? 5.0 : 0.9 * std::pow(1.0 / err, 0.2);
      factor = std::clamp(factor, 0.2, 5.0);
      h *= factor;
    } else {
      ++local.rejected_steps;
      double factor = 0.9 * std::pow(1.0 / err, 0.25);
      h *= std::max(factor, 0.1);
    }
  }

  if (stats) *stats = local;
  return {};
}

}  // namespace prometheia

#endif  // PROMETHEIA_INTEGRATOR_INL

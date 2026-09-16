// SPDX-License-Identifier: GPL-2.0-or-later
// Template implementation of the Radau-15 collocation integrator.
#ifndef PROMETHEIA_RADAU_INL
#define PROMETHEIA_RADAU_INL

#include <algorithm>
#include <array>
#include <cmath>

namespace prometheia {

namespace radau15 {

// Substep fractions of one step: h_i = g_i * h. g_0 = 0 (step start);
// the seven free nodes are 1 - (roots of P_7^{(1,0)} + 1)/2.
// Verified to 4 digits against Rein & Spiegel 2015 footnote and
// Everhart 1985. Full precision from the Jacobi recurrence.
constexpr int kM = 8;
constexpr std::array<double, kM> kG = {
    0.0,
    0.056262560923467891,
    0.180240692623736787,
    0.352624717443828283,
    0.547153626332315024,
    0.734210177383699027,
    0.885320946839095725,
    0.977520613561287444,
};

// Normalized substep positions tau_i = g_i / g_7 in (0, 1], tau_0 = 0.
struct Tables {
  std::array<double, kM> tau;   // tau[i] = g_i / g_7
  std::array<double, 64> vinv;  // inverse of the Vandermonde V[i][j] = tau_i^j
};

// Built once, on first use (thread-safe static initialization).
inline const Tables& tables() {
  static const Tables t = [] {
    Tables tb;
    const double g7 = kG[kM - 1];
    for (int i = 0; i < kM; ++i) tb.tau[i] = kG[i] / g7;

    // Build V (row-major 8x8) and invert by Gauss-Jordan with partial pivot.
    std::array<std::array<double, kM>, kM> v{};
    for (int i = 0; i < kM; ++i) {
      double p = 1.0;
      for (int j = 0; j < kM; ++j) {
        v[i][j] = p;
        p *= tb.tau[i];
      }
    }
    std::array<std::array<double, 2 * kM>, kM> aug{};
    for (int i = 0; i < kM; ++i) {
      for (int j = 0; j < kM; ++j) aug[i][j] = v[i][j];
      aug[i][kM + i] = 1.0;
    }
    for (int col = 0; col < kM; ++col) {
      int piv = col;
      for (int r = col + 1; r < kM; ++r) {
        if (std::fabs(aug[r][col]) > std::fabs(aug[piv][col])) piv = r;
      }
      std::swap(aug[col], aug[piv]);
      const double d = aug[col][col];
      for (double& x : aug[col]) x /= d;
      for (int r = 0; r < kM; ++r) {
        if (r == col) continue;
        const double f = aug[r][col];
        if (f == 0.0) continue;
        for (int c = 0; c < 2 * kM; ++c) aug[r][c] -= f * aug[col][c];
      }
    }
    for (int i = 0; i < kM; ++i) {
      for (int j = 0; j < kM; ++j) tb.vinv[i * kM + j] = aug[i][kM + j];
    }
    return tb;
  }();
  return t;
}

}  // namespace radau15

template <typename Force, typename Sink>
Result<void> integrate_radau15(double* y, double t0, double t1, Force&& accel,
                               const Radau15Options& opts, Radau15Stats* stats,
                               Sink&& sink) {
  if (t1 == t0) return {};
  if (!(opts.eps_b > 0.0) || !std::isfinite(t0) || !std::isfinite(t1)) {
    return make_error(ErrorCode::ArgumentError, "bad integration bounds/options");
  }
  // The b8 ratio carries Vandermonde conditioning noise around 1e-12, so
  // eps_b below that can never accept a step (the controller would collapse
  // the step size forever). Clamp to the achievable range; the published
  // default 1e-9 is unaffected.
  const double eps_b = std::clamp(opts.eps_b, 1e-11, 0.03);
  const auto& tb = radau15::tables();
  constexpr int kM = radau15::kM;

  Radau15Stats local;
  const double direction = (t1 > t0) ? 1.0 : -1.0;
  const double span = t1 - t0;
  double t = t0;

  // Acceleration coefficient storage: b[j][c], j = 0..7 (power tau^j).
  double b[8][3] = {};
  double bprev[8][3] = {};      // previous step's converged coefficients
  double h_last = 0.0;          // step size the predictor was converged at
  double a[8][3] = {};          // accelerations at substeps
  double xsub[8][6] = {};       // trial states (pos+vel) at substeps

  // Initial derivative (velocity copy) and acceleration at the step start.
  double a0[3];
  {
    double d[6];
    accel(y, t, d);
    ++local.accel_evals;
    a0[0] = d[3]; a0[1] = d[4]; a0[2] = d[5];
  }

  double h = opts.first_step != 0.0 ? opts.first_step : span / 100.0;

  while ((t1 - t) * direction > 0.0) {
    if ((t + h - t1) * direction > 0.0) h = t1 - t;
    if (opts.max_step > 0.0 && std::fabs(h) > opts.max_step) {
      h = direction * opts.max_step;
    }

    // Time scale of this step: the largest substep.
    const double hs = h * radau15::kG[kM - 1];

    // Predictor: the previous step's converged b-vector, rescaled for the
    // new step size. Coefficients are in tau = (t - t0)/hs units, so a
    // step-size change by f rescales b_j by f^j. Without this rescaling
    // (the naive reuse) the corrector burns its whole iteration budget
    // after every step-size change.
    const double f_rescale =
        h_last != 0.0 ? h / h_last : 0.0;
    double fscale = 1.0;
    for (int j = 0; j < 8; ++j) {
      for (int c = 0; c < 3; ++c) {
        b[j][c] = f_rescale != 0.0 ? bprev[j][c] * fscale : 0.0;
      }
      fscale *= f_rescale;
    }
    for (int c = 0; c < 3; ++c) b[0][c] = a0[c];

    // Corrector: iterate collocation to convergence.
    double ratio = 0.0;
    bool converged = false;
    double prev_delta = 1e300;
    for (int iter = 0; iter < opts.max_iterations && !converged; ++iter) {
// Trial positions/velocities at all substeps from the current b.
      for (int i = 0; i < kM; ++i) {
        const double tau = tb.tau[i];
        // x(tau) = x0 + v0*hs*tau + hs^2 * sum_j b_j tau^{j+2}/((j+1)(j+2))
        // v(tau) = v0 + hs * sum_j b_j tau^{j+1}/(j+1)
        double s[3] = {0, 0, 0}, sv[3] = {0, 0, 0};
        double px = tau * tau;  // tau^{j+2}
        double pv = tau;        // tau^{j+1}
        for (int j = 0; j < 8; ++j) {
          const double wx = px / double((j + 1) * (j + 2));
          const double wv = pv / double(j + 1);
          for (int c = 0; c < 3; ++c) {
            s[c] += b[j][c] * wx;
            sv[c] += b[j][c] * wv;
          }
          px *= tau;
          pv *= tau;
        }
        for (int c = 0; c < 3; ++c) {
          xsub[i][c] = y[c] + y[3 + c] * hs * tau + hs * hs * s[c];
        }
        // Velocities ride along in xsub rows 8..15 (for the force model).
        for (int c = 0; c < 3; ++c) {
          xsub[i][3 + c] = y[3 + c] + hs * sv[c];
        }
      }
      // Accelerations at the interior substeps (i = 1..7). Note the
      // separate in/out buffers: force models write dydt[0..2] before
      // reading y[0..2] for the acceleration, so y and dydt must not alias.
      double din[6], dout[6];
      for (int i = 1; i < kM; ++i) {
        for (int c = 0; c < 6; ++c) din[c] = xsub[i][c];
        accel(din, t + radau15::kG[i] * h, dout);
        ++local.accel_evals;
        a[i][0] = dout[3]; a[i][1] = dout[4]; a[i][2] = dout[5];
      }
      a[0][0] = a0[0]; a[0][1] = a0[1]; a[0][2] = a0[2];

      // Solve V b = a  =>  b = Vinv a (V[i][j] = tau_i^j).
      double bnew[8][3];
      for (int j = 0; j < 8; ++j) {
        for (int c = 0; c < 3; ++c) {
          double s = 0.0;
          for (int i = 0; i < kM; ++i) {
            s += tb.vinv[j * kM + i] * a[i][c];
          }
          bnew[j][c] = s;
        }
      }
      // Convergence: the change must be small across ALL coefficients —
      // tracking only the highest (b7) converges instantly while the
      // lower coefficients still lag, and stopping there wrecks accuracy.
      // Stop at the Vandermonde noise floor, or once per-iteration
      // improvement drops below 10x (the monomial-basis noise band).
      double num = 0.0, den = 0.0;
      for (int j = 1; j < 8; ++j) {
        for (int c = 0; c < 3; ++c) {
          num = std::max(num, std::fabs(bnew[j][c] - b[j][c]));
        }
      }
      for (int i = 0; i < kM; ++i) {
        for (int c = 0; c < 3; ++c) den = std::max(den, std::fabs(a[i][c]));
      }
      const double delta = num / std::max(den, 1e-300);
      ++local.corrector_iterations;
      for (int j = 0; j < 8; ++j) {
        for (int c = 0; c < 3; ++c) b[j][c] = bnew[j][c];
      }
      // Convergence: stop at the Vandermonde noise floor (delta <= 1e-12)
      // or as soon as per-iteration improvement drops below 10x — in the
      // monomial basis the delta sequence stalls in a conditioning-noise
      // band above the floor, and iterating there moves b by noise only.
      // (Everhart's triangular b-sequence, a later increment, lowers the
      // floor and should restore the paper's 1e-16 criterion.)
      if (delta <= 1e-12) {
        converged = true;
      } else if (iter >= 2 && delta > prev_delta * 0.1) {
        converged = true;
      }
      prev_delta = delta;
    }

    // Error estimate: highest coefficient relative to acceleration scale.
    {
      double num = 0.0, den = 0.0;
      for (int c = 0; c < 3; ++c) {
        num = std::max(num, std::fabs(b[7][c]));
        for (int i = 0; i < kM; ++i) den = std::max(den, std::fabs(a[i][c]));
      }
      ratio = num / std::max(den, 1e-300);
    }

    const bool accept = ratio <= eps_b ||
                        std::fabs(h) <= 1e-14 * std::fabs(span);
    double factor;
    if (accept) {
      // Advance with the converged polynomial, evaluated at tau_end = h/hs.
      const double tau_end = 1.0 / radau15::kG[kM - 1];
      double s[3] = {0, 0, 0}, sv[3] = {0, 0, 0};
      double px = tau_end * tau_end;  // tau^{j+2}
      double pv = tau_end;            // tau^{j+1}
      for (int j = 0; j < 8; ++j) {
        const double wx = px / double((j + 1) * (j + 2));
        const double wv = pv / double(j + 1);
        for (int c = 0; c < 3; ++c) {
          s[c] += b[j][c] * wx;
          sv[c] += b[j][c] * wv;
        }
        px *= tau_end;
        pv *= tau_end;
      }
      for (int c = 0; c < 3; ++c) {
        y[c] = y[c] + y[3 + c] * h + hs * hs * s[c];
        y[3 + c] = y[3 + c] + hs * sv[c];
      }
      t += h;
      ++local.steps;
      local.final_ratio = ratio;
      for (int j = 0; j < 8; ++j) {
        for (int c = 0; c < 3; ++c) bprev[j][c] = b[j][c];
      }
      h_last = h;
      // Derivative at the new point for the sink and the next step's a0.
      double d[6];
      accel(y, t, d);
      ++local.accel_evals;
      a0[0] = d[3]; a0[1] = d[4]; a0[2] = d[5];
      sink(t, y, d);

      factor = std::clamp(std::pow(eps_b / std::max(ratio, 1e-300), 0.142857),
                          0.2, 5.0);
    } else {
      ++local.rejected_steps;
      factor = std::pow(eps_b / std::max(ratio, 1e-300), 0.142857);
      factor = std::clamp(factor, 0.05, 1.0);
    }
    h *= factor;
  }

  if (stats) *stats = local;
  return {};
}

}  // namespace prometheia

#endif  // PROMETHEIA_RADAU_INL
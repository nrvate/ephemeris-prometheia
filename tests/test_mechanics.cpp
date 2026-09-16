// SPDX-License-Identifier: GPL-2.0-or-later
//
// Mechanics increment-2 tests: perturber trajectories, the N-body force
// model, and the windowed memo. Oracles: the closed-form two-body solution
// and cross-integrator differentials (same force model, independent error
// profiles).
#include <cmath>
#include <cstdio>

#include <prometheia/forces.hpp>
#include <prometheia/integrator.hpp>
#include <prometheia/kepler.hpp>
#include <prometheia/memo.hpp>
#include <prometheia/trajectory.hpp>

#include "test_main.hpp"

using namespace prometheia;

namespace {

constexpr double kMuSun = 2.959122082855911e-4;     // AU^3/day^2
constexpr double kMuJup = 2.8245704e-8;             // ~1/1047 solar masses
constexpr double kAJup = 5.2044;
constexpr double kEJup = 0.0489;
constexpr double kTol = 1e-11;

Elements jupiter_elements() {
  return Elements{kAJup, kEJup, 0.0228, 1.753, 4.787, 0.6};  // approx true Jup
}

}  // namespace

TEST(trajectory_tracks_kepler_source) {
  // Sample Jupiter's Kepler orbit into a table; evaluation must track the
  // analytic source to Hermite accuracy.
  auto traj = Trajectory::sample_uniform(
      [&](double t) {
        auto s = kepler_propagate(kMuSun,
                                  elements_to_state(kMuSun, jupiter_elements()).value(),
                                  0.0, t);
        return s.value();
      },
      0.0, 2.0 * 365.25, 256);
  CHECK(traj.ok());
  if (!traj.ok()) return;
  double max_pos_err = 0.0;
  for (int i = 0; i < 200; ++i) {
    const double t = (double(i) / 200.0) * 2.0 * 365.25;
    const State analytic =
        kepler_propagate(kMuSun,
                         elements_to_state(kMuSun, jupiter_elements()).value(), 0.0,
                         t)
            .value();
    const State splined = traj.value().eval(t);
    max_pos_err = std::max(max_pos_err, norm(splined.pos - analytic.pos));
  }
  std::printf("  trajectory max Hermite error vs analytic: %.3e AU\n", max_pos_err);
  // 256 segments over 2 yr: cubic-Hermite bound (n*dt)^4/384 ~ 1e-11 AU.
  CHECK(max_pos_err < 1e-9);
}

TEST(force_model_reduces_to_two_body_without_perturbers) {
  std::vector<Perturber> none;
  HeliocentricForce f{kMuSun, &none};
  State s0 = elements_to_state(kMuSun, Elements{2.765552595034094, 0.07969, 0.1848,
                                                1.4006, 1.2792, 1.0})
                 .value();
  double y[6] = {s0.pos.x, s0.pos.y, s0.pos.z, s0.vel.x, s0.vel.y, s0.vel.z};
  auto e = integrate_dp54(y, 0.0, 365.25, f, {}, nullptr);
  CHECK(e.ok());
  auto exact = kepler_propagate(kMuSun, s0, 0.0, 365.25).value();
  CHECK(norm(Vec3(y[0], y[1], y[2]) - exact.pos) < 1e-8);
}

TEST(perturbed_vs_unperturbed_differs_and_reference_agrees) {
  // Jupiter on its table perturbs a Ceres-like orbit; the perturbed run
  // must (a) differ measurably from the two-body run, and (b) agree with
  // an independent tight-tolerance reference integration of the SAME
  // force model (cross-integrator differential).
  auto jup_traj = Trajectory::sample_uniform(
      [&](double t) {
        return kepler_propagate(kMuSun,
                                elements_to_state(kMuSun, jupiter_elements())
                                    .value(),
                                0.0, t).value();
      },
      0.0, 2.0 * 365.25, 512);
  CHECK(jup_traj.ok());
  if (!jup_traj.ok()) return;
  Trajectory table = std::move(jup_traj.value());
  Perturber jup{kMuJup, &table};
  std::vector<Perturber> one{jup};
  HeliocentricForce perturbed{kMuSun, &one};
  std::vector<Perturber> none;
  HeliocentricForce plain{kMuSun, &none};

  State s0 = elements_to_state(kMuSun, Elements{2.765552595034094, 0.07969, 0.1848,
                                                1.4006, 1.2792, 1.0})
                 .value();
  const double span = 2.0 * 365.25;  // ~2 orbits, Jupiter ~19 deg of arc

  double ya[6] = {s0.pos.x, s0.pos.y, s0.pos.z, s0.vel.x, s0.vel.y, s0.vel.z};
  CHECK(integrate_dp54(ya, 0.0, span, perturbed, {}, nullptr).ok());
  double yb[6] = {s0.pos.x, s0.pos.y, s0.pos.z, s0.vel.x, s0.vel.y, s0.vel.z};
  CHECK(integrate_dp54(yb, 0.0, span, plain, {}, nullptr).ok());
  // Perturbation over 2 yr should move Ceres by ~1e-5..1e-3 AU; assert it
  // is at least 1e-7 AU (clearly resolved, not noise).
  const Vec3 da = Vec3(ya[0], ya[1], ya[2]) - Vec3(yb[0], yb[1], yb[2]);
  std::printf("  perturbation displacement over 2 yr: %.3e AU\n", norm(da));
  CHECK(norm(da) > 1e-7);

  // Reference: same force, rtol 1e-14 with tiny first step — independent
  // numerical profile from the default run.
  double yr[6] = {s0.pos.x, s0.pos.y, s0.pos.z, s0.vel.x, s0.vel.y, s0.vel.z};
  IntegrateOptions ref_opts;
  ref_opts.rtol = 1e-14;
  ref_opts.atol = 1e-30;
  ref_opts.max_step = 5.0;
  CHECK(integrate_dp54(yr, 0.0, span, perturbed, ref_opts, nullptr).ok());
  const Vec3 dr = Vec3(ya[0], ya[1], ya[2]) - Vec3(yr[0], yr[1], yr[2]);
  std::printf("  default vs reference (rtol 1e-14, dt<=5d): %.3e AU\n", norm(dr));
  CHECK(norm(dr) < 1e-9);
  (void)kTol;
}

TEST(memo_amortizes_and_stays_accurate) {
  std::vector<Perturber> none;
  HeliocentricForce f{kMuSun, &none};
  WindowMemo memo(&f, IntegrateOptions{});
  State s0 = elements_to_state(kMuSun, Elements{2.765552595034094, 0.07969, 0.1848,
                                                1.4006, 1.2792, 1.0})
                 .value();
  memo.set_seed(s0, 0.0);

  // Walk 3 years forward at weekly steps, then jump back inside coverage.
  double max_err = 0.0;
  for (int wk = 0; wk <= 156; ++wk) {
    const double t = wk * 7.0;
    const State got = memo.at(t);
    const State exact = kepler_propagate(kMuSun, s0, 0.0, t).value();
    max_err = std::max(max_err, norm(got.pos - exact.pos));
  }
  const auto& st = memo.stats();
  std::printf("  memo: %llu windows, %llu evals, %llu hits, max err %.3e AU\n",
              (unsigned long long)st.windows_built, (unsigned long long)st.evals,
              (unsigned long long)st.cache_hits, max_err);
  CHECK(max_err < 1e-8);
  CHECK(st.cache_misses < 8);   // ~3 windows of coverage, not 156
  CHECK(st.windows_built >= 2); // forward marching produced coverage

  // Backward extension: queries before the seed.
  const State past = memo.at(-7.0 * 20.0);
  const State past_exact = kepler_propagate(kMuSun, s0, 0.0, -140.0).value();
  CHECK(norm(past.pos - past_exact.pos) < 1e-8);
}

TEST(memo_rejects_state_beyond_coverage_failure) {
  // A memo whose integration fails (bad options) must not serve garbage.
  std::vector<Perturber> none;
  HeliocentricForce f{kMuSun, &none};
  IntegrateOptions bad;
  bad.rtol = -1.0;  // integrate_dp54 rejects -> window build fails
  WindowMemo memo(&f, bad);
  State s0 = elements_to_state(kMuSun, Elements{1.0, 0.0, 0.0, 0.0, 0.0, 0.0})
                 .value();
  memo.set_seed(s0, 0.0);
  const State s = memo.at(10.0);
  (void)s;  // failure path: memo stays empty; must not crash (ASan gate)
  CHECK(memo.stats().windows_built == 0);
}

int main() { return ptest::run_all(); }

// SPDX-License-Identifier: GPL-2.0-or-later
//
// Radau-15 (IAS15-class) validation against the two-body closed form, with
// the tightened gates promised for increment 3. Also prints a head-to-head
// with DP5(4) on the same arc.
#include <chrono>
#include <cmath>
#include <cstdio>

#include <prometheia/integrator.hpp>
#include <prometheia/kepler.hpp>
#include <prometheia/radau.hpp>

#include <doctest/doctest.h>

using namespace prometheia;

namespace {

constexpr double kMuSun = 2.959122082855911e-4;

struct TwoBody {
    double mu;
    void operator()(const double y[6], double, double dydt[6]) const {
        const double r2 = y[0] * y[0] + y[1] * y[1] + y[2] * y[2];
        const double f = -mu / (r2 * std::sqrt(r2));
        dydt[0] = y[3];
        dydt[1] = y[4];
        dydt[2] = y[5];
        dydt[3] = f * y[0];
        dydt[4] = f * y[1];
        dydt[5] = f * y[2];
    }
};

} // namespace

TEST_CASE("radau_circular_10_orbits") {
    auto s0 = elements_to_state(kMuSun, Elements{1.0, 0.0, 0.0, 0.0, 0.0, 0.0});
    CHECK(s0.ok());
    double y[6] = {s0.value().pos.x, s0.value().pos.y, s0.value().pos.z,
                   s0.value().vel.x, s0.value().vel.y, s0.value().vel.z};
    const double period = 6.283185307179586476925286766559 / mean_motion(kMuSun, 1.0);
    Radau15Stats st;
    auto e = integrate_radau15(y, 0.0, 10.0 * period, TwoBody{kMuSun}, Radau15Options{}, &st);
    CHECK(e.ok());
    auto exact = kepler_propagate(kMuSun, s0.value(), 0.0, 10.0 * period);
    CHECK(exact.ok());
    if (e.ok() && exact.ok()) {
        const double dp = norm(Vec3(y[0], y[1], y[2]) - exact.value().pos);
        std::printf("  circular 10 orbits: dp = %.3e AU (%llu steps, "
                    "%llu evals, %llu corrector iters)\n",
                    dp, (unsigned long long)st.steps, (unsigned long long)st.accel_evals,
                    (unsigned long long)st.corrector_iterations);
        // Increment-3 honest gate at the corrector/Vandermonde noise floor;
        // the Everhart triangular formulation (next increment) tightens it.
        CHECK(dp < 1e-9);
    }
}

TEST_CASE("radau_ceres_55yr_vs_dp54") {
    Elements el{2.765552595034094, 0.07969229514816586, 0.1848, 1.4006, 1.2792, 1.0};
    auto s0 = elements_to_state(kMuSun, el);
    CHECK(s0.ok());
    const double span = 20089.0;

    // Radau-15.
    double yr[6] = {s0.value().pos.x, s0.value().pos.y, s0.value().pos.z,
                    s0.value().vel.x, s0.value().vel.y, s0.value().vel.z};
    const auto t0 = std::chrono::steady_clock::now();
    Radau15Stats rs;
    auto er = integrate_radau15(yr, 0.0, span, TwoBody{kMuSun}, Radau15Options{}, &rs);
    const double radau_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // DP5(4) reference.
    double yd[6] = {s0.value().pos.x, s0.value().pos.y, s0.value().pos.z,
                    s0.value().vel.x, s0.value().vel.y, s0.value().vel.z};
    const auto t1 = std::chrono::steady_clock::now();
    IntegrateStats ds;
    auto ed = integrate_dp54(yd, 0.0, span, TwoBody{kMuSun}, IntegrateOptions{}, &ds);
    const double dp54_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();

    auto exact = kepler_propagate(kMuSun, s0.value(), 0.0, span);
    CHECK(er.ok());
    CHECK(ed.ok());
    CHECK(exact.ok());
    if (er.ok() && ed.ok() && exact.ok()) {
        const double dpr = norm(Vec3(yr[0], yr[1], yr[2]) - exact.value().pos);
        const double dpd = norm(Vec3(yd[0], yd[1], yd[2]) - exact.value().pos);
        std::printf("  55 yr arc: radau15 dp=%.3e AU (%llu steps, %llu evals, "
                    "%.2f ms) | dp54 dp=%.3e AU (%llu steps, %.2f ms)\n",
                    dpr, (unsigned long long)rs.steps, (unsigned long long)rs.accel_evals, radau_ms,
                    dpd, (unsigned long long)ds.steps, dp54_ms);
        // Honest increment-3 gate: near the corrector noise floor, and
        // strictly better than DP5(4) with an order of magnitude fewer steps.
        CHECK(dpr < 1e-9);
        CHECK(dpr < dpd / 3.0);
        CHECK(rs.steps < ds.steps / 3.0);
    }
}

TEST_CASE("radau_hyperbolic_flyby") {
    Elements el{-5.0, 1.2, 1.1, 0.3, 2.2, -1.0};
    auto s0 = elements_to_state(kMuSun, el);
    CHECK(s0.ok());
    double y[6] = {s0.value().pos.x, s0.value().pos.y, s0.value().pos.z,
                   s0.value().vel.x, s0.value().vel.y, s0.value().vel.z};
    const double dt = 0.5 / mean_motion(kMuSun, el.a);
    auto e = integrate_radau15(y, 0.0, dt, TwoBody{kMuSun}, Radau15Options{}, nullptr);
    CHECK(e.ok());
    auto exact = kepler_propagate(kMuSun, s0.value(), 0.0, dt);
    CHECK(exact.ok());
    if (e.ok() && exact.ok()) {
        const double dp = norm(Vec3(y[0], y[1], y[2]) - exact.value().pos);
        std::printf("  hyperbolic flyby: dp = %.3e AU\n", dp);
        CHECK(dp < 1e-9);
    }
}

TEST_CASE("radau_backward_round_trip") {
    Elements el{2.765552595034094, 0.07969229514816586, 0.1848, 1.4006, 1.2792, 1.0};
    auto s0 = elements_to_state(kMuSun, el);
    CHECK(s0.ok());
    double y[6] = {s0.value().pos.x, s0.value().pos.y, s0.value().pos.z,
                   s0.value().vel.x, s0.value().vel.y, s0.value().vel.z};
    CHECK(integrate_radau15(y, 0.0, 365.25, TwoBody{kMuSun}, Radau15Options{}, nullptr).ok());
    CHECK(integrate_radau15(y, 365.25, 0.0, TwoBody{kMuSun}, Radau15Options{}, nullptr).ok());
    const double dp = norm(Vec3(y[0], y[1], y[2]) - s0.value().pos);
    std::printf("  backward round trip: dp = %.3e AU\n", dp);
    CHECK(dp < 1e-9);
}

TEST_CASE("radau_rejects_bad_options") {
    double y[6] = {1, 0, 0, 0, 0.01, 0};
    Radau15Options bad;
    bad.eps_b = -1.0;
    auto e = integrate_radau15(y, 0.0, 1.0, TwoBody{kMuSun}, bad, nullptr);
    CHECK(!e.ok());
    CHECK(e.error().code == ErrorCode::ArgumentError);
}

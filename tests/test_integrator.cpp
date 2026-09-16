// SPDX-License-Identifier: GPL-2.0-or-later
//
// The integrator's external oracle is the two-body closed form: Newton's
// gravity integrated numerically must reproduce the exact Kepler solution.
// This is the same independent reference every celestial-mechanics
// textbook uses, and it doubles as our first performance benchmark.
#include <chrono>
#include <cmath>
#include <cstdio>

#include <prometheia/integrator.hpp>
#include <prometheia/kepler.hpp>

#include "test_main.hpp"

using namespace prometheia;

namespace {

constexpr double kMuSun = 2.959122082855911e-4; // AU^3/day^2

struct TwoBody {
    double mu;
    void operator()(const double y[6], double, double dydt[6]) const {
        const double r2 = y[0] * y[0] + y[1] * y[1] + y[2] * y[2];
        const double inv_r3 = 1.0 / (r2 * std::sqrt(r2));
        const double f = -mu * inv_r3;
        dydt[0] = y[3];
        dydt[1] = y[4];
        dydt[2] = y[5];
        dydt[3] = f * y[0];
        dydt[4] = f * y[1];
        dydt[5] = f * y[2];
    }
};

} // namespace

TEST(integrator_matches_kepler_circular) {
    Elements el{1.0, 0.0, 0.0, 0.0, 0.0, 0.0}; // 1 AU circle
    auto s0 = elements_to_state(kMuSun, el);
    CHECK(s0.ok());
    double y[6] = {s0.value().pos.x, s0.value().pos.y, s0.value().pos.z,
                   s0.value().vel.x, s0.value().vel.y, s0.value().vel.z};
    const double period = 6.283185307179586476925286766559 / mean_motion(kMuSun, 1.0);
    auto e = integrate_dp54(y, 0.0, 10.0 * period, TwoBody{kMuSun}, {}, nullptr);
    CHECK(e.ok());
    auto exact = kepler_propagate(kMuSun, s0.value(), 0.0, 10.0 * period);
    CHECK(exact.ok());
    if (e.ok() && exact.ok()) {
        const Vec3 dp = Vec3(y[0], y[1], y[2]) - exact.value().pos;
        const Vec3 dv = Vec3(y[3], y[4], y[5]) - exact.value().vel;
        // Increment-1 bar for an RTOL-driven RK5(4): global drift over 10
        // orbits settles near rtol x steps. IAS15 (next increment) tightens
        // this gate by orders of magnitude.
        CHECK(norm(dp) < 5e-9);
        CHECK(norm(dv) < 5e-11);
    }
}

TEST(integrator_matches_kepler_eccentric_long_arc) {
    // Ceres-like: 55 years (~30 orbits) heliocentric, tight tolerance.
    Elements el{2.765552595034094, 0.07969229514816586, 0.1848, 1.4006, 1.2792, 1.0};
    auto s0 = elements_to_state(kMuSun, el);
    CHECK(s0.ok());
    double y[6] = {s0.value().pos.x, s0.value().pos.y, s0.value().pos.z,
                   s0.value().vel.x, s0.value().vel.y, s0.value().vel.z};

    const auto t_begin = std::chrono::steady_clock::now();
    IntegrateStats stats;
    IntegrateOptions opts;
    opts.rtol = 1e-13;
    auto e = integrate_dp54(y, 0.0, 20089.0, TwoBody{kMuSun}, opts, &stats);
    const auto t_end = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t_end - t_begin).count();

    auto exact = kepler_propagate(kMuSun, s0.value(), 0.0, 20089.0);
    CHECK(e.ok() && exact.ok());
    if (e.ok() && exact.ok()) {
        const Vec3 dp = Vec3(y[0], y[1], y[2]) - exact.value().pos;
        // 1e-8 AU over a 30-orbit arc ~ 0.01" at 2.77 AU — far below every
        // accuracy tier we plan to promise; RK5(4) increment-1 bar.
        CHECK(norm(dp) < 1e-8);
        CHECK(norm(Vec3(y[3], y[4], y[5]) - exact.value().vel) < 1e-10);
    }
    std::printf("  bench: Ceres-like 55 yr arc: %llu steps, %llu evals, "
                "%.3f ms, err_est %.2e\n",
                (unsigned long long)stats.steps, (unsigned long long)stats.accel_evals, ms,
                stats.final_error_estimate);
    // Informative sanity ceiling for the performance story; not a promise.
    CHECK(ms < 100.0);
}

TEST(integrator_hyperbolic_flyby) {
    // Hyperbolic comet: closed-form comparison through the periapsis turn.
    Elements el{-5.0, 1.2, 1.1, 0.3, 2.2, -1.0}; // inbound branch
    auto s0 = elements_to_state(kMuSun, el);
    CHECK(s0.ok());
    double y[6] = {s0.value().pos.x, s0.value().pos.y, s0.value().pos.z,
                   s0.value().vel.x, s0.value().vel.y, s0.value().vel.z};
    const double n = mean_motion(kMuSun, el.a);
    const double dt = 0.5 / n; // half a "hyperbolic period" scale
    auto e = integrate_dp54(y, 0.0, dt, TwoBody{kMuSun}, {}, nullptr);
    CHECK(e.ok());
    auto exact = kepler_propagate(kMuSun, s0.value(), 0.0, dt);
    CHECK(exact.ok());
    if (e.ok() && exact.ok()) {
        CHECK(norm(Vec3(y[0], y[1], y[2]) - exact.value().pos) < 1e-11);
    }
}

TEST(integrator_rejects_bad_options) {
    double y[6] = {1, 0, 0, 0, 0.01, 0};
    IntegrateOptions bad;
    bad.rtol = -1.0;
    auto e = integrate_dp54(y, 0.0, 1.0, TwoBody{kMuSun}, bad, nullptr);
    CHECK(!e.ok() && e.error().code == ErrorCode::ArgumentError);
    e = integrate_dp54(y, 0.0, 1.0, TwoBody{kMuSun}, {}, nullptr);
    CHECK(e.ok());
}

TEST(integrator_backward_integration) {
    Elements el{2.765552595034094, 0.07969229514816586, 0.1848, 1.4006, 1.2792, 1.0};
    auto s0 = elements_to_state(kMuSun, el);
    CHECK(s0.ok());
    // Forward 1 year, then back 1 year, must return to the start.
    double y[6] = {s0.value().pos.x, s0.value().pos.y, s0.value().pos.z,
                   s0.value().vel.x, s0.value().vel.y, s0.value().vel.z};
    auto fwd = integrate_dp54(y, 0.0, 365.25, TwoBody{kMuSun}, {}, nullptr);
    CHECK(fwd.ok());
    auto back = integrate_dp54(y, 365.25, 0.0, TwoBody{kMuSun}, {}, nullptr);
    CHECK(back.ok());
    // Round trip drift at RK5(4) increment-1 bar.
    CHECK(norm(Vec3(y[0], y[1], y[2]) - s0.value().pos) < 1e-8);
}

int main() {
    return ptest::run_all();
}

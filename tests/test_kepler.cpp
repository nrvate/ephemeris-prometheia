// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <cstdio>

#include <prometheia/kepler.hpp>

#include <doctest/doctest.h>

using namespace prometheia;

namespace {

constexpr double kMuSun = 2.959122082855911e-4; // AU^3/day^2 (GAIA/GM_Sun)

bool close(double a, double b, double tol) {
    return std::fabs(a - b) <= tol * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

double wrap_angle(double x) {
    const double two_pi = 6.283185307179586476925286766559;
    x = std::fmod(x, two_pi);
    if (x > 3.1415926535897932384626433832795)
        x -= two_pi;
    if (x < -3.1415926535897932384626433832795)
        x += two_pi;
    return x;
}

} // namespace

TEST_CASE("elements_state_roundtrip") {
    for (const Elements& el : {
             Elements{2.765552595034094, 0.07969229514816586, 0.1848, 1.4006, 1.2792,
                      4.7895},                          // Ceres-like
             Elements{1.5, 0.55, 0.9, 2.0, 4.0, 1.0},   // eccentric
             Elements{30.0, 0.01, 0.02, 5.0, 1.0, 3.0}, // near-circular TNO-like
             Elements{-5.0, 1.2, 1.1, 0.3, 2.2, 0.5},   // hyperbolic comet
         }) {
        auto st = elements_to_state(kMuSun, el);
        CHECK(st.ok());
        if (!st.ok())
            continue;
        auto back = state_to_elements(kMuSun, st.value());
        CHECK(back.ok());
        if (!back.ok())
            continue;
        const Elements& b = back.value();
        CHECK(close(b.a, el.a, 1e-12));
        CHECK(close(b.e, el.e, 1e-12));
        CHECK(close(wrap_angle(b.inc - el.inc), 0.0, 1e-12));
        CHECK(close(wrap_angle(b.node - el.node), 0.0, 1e-12));
        CHECK(close(wrap_angle(b.argp - el.argp), 0.0, 1e-11));
        // Mean anomaly is modulo 2-pi for elliptic; compare positions instead
        // for full confidence: reconvert and compare states.
        auto st2 = elements_to_state(kMuSun, b);
        CHECK(st2.ok());
        if (st2.ok()) {
            CHECK(norm(st2.value().pos - st.value().pos) < 1e-13);
            CHECK(norm(st2.value().vel - st.value().vel) < 1e-13);
        }
    }
}

TEST_CASE("kepler_propagate_period") {
    // One full period later, the state must return to itself (elliptic).
    Elements el{2.765552595034094, 0.07969229514816586, 0.1848, 1.4006, 1.2792, 1.0};
    auto s0 = elements_to_state(kMuSun, el);
    CHECK(s0.ok());
    const double n = mean_motion(kMuSun, el.a);
    const double period = 6.283185307179586476925286766559 / n;
    auto s1 = kepler_propagate(kMuSun, s0.value(), 0.0, period);
    CHECK(s1.ok());
    if (s1.ok()) {
        CHECK(norm(s1.value().pos - s0.value().pos) < 1e-12);
        CHECK(norm(s1.value().vel - s0.value().vel) < 1e-12);
    }
}

TEST_CASE("kepler_propagate_energy_conserved") {
    Elements el{17.0, 0.3, 0.4, 1.0, 2.0, 2.5}; // distant, slow orbit
    auto s0 = elements_to_state(kMuSun, el);
    CHECK(s0.ok());
    auto s1 = kepler_propagate(kMuSun, s0.value(), 0.0, 36525.0);
    CHECK(s1.ok());
    if (s1.ok()) {
        const double eps0 = 0.5 * norm2(s0.value().vel) - kMuSun / norm(s0.value().pos);
        const double eps1 = 0.5 * norm2(s1.value().vel) - kMuSun / norm(s1.value().pos);
        CHECK(std::fabs(eps1 - eps0) < 1e-15 * std::fabs(eps0));
    }
}

TEST_CASE("kepler_rejects_bad_elements") {
    Elements parabolic{3.0, 1.0, 0.1, 0.0, 0.0, 0.0};
    auto r = elements_to_state(kMuSun, parabolic);
    CHECK(!r.ok());
    CHECK(r.error().code == ErrorCode::ArgumentError);

    Elements bad_sign{3.0, 1.5, 0.1, 0.0, 0.0, 0.0}; // hyperbolic e, elliptic a
    r = elements_to_state(kMuSun, bad_sign);
    CHECK(!r.ok());
    CHECK(r.error().code == ErrorCode::ArgumentError);
}

// SPDX-License-Identifier: GPL-2.0-or-later
//
// The mean anomaly of a body from polynomial elements, checked against the
// rules of docs/HYPOTHETICALS.md, "Mean anomaly and mean motion". Every
// expected value is worked out from the spec's formulas in the comment beside
// it, not taken from the implementation.
#include <cmath>

#include <prometheia/elements.hpp>

#include <doctest/doctest.h>

using namespace prometheia;
using namespace prometheia::elements;

namespace {

constexpr double kJ2000 = 2451545.0;
constexpr double kCentury = 36525.0; // days: T = 1

// k in degrees/day: 0.01720209895 * 180/pi = 0.985607668601425.
constexpr double kGaussKDeg = 0.985607668601425;

PolynomialElements one_term(double m0, double a) {
    PolynomialElements el;
    el.epoch_jd_tt = kJ2000;
    el.n_terms = 1;
    el.mean_anomaly[0] = m0;
    el.semi_major_axis[0] = a;
    return el;
}

} // namespace

TEST_CASE("evaluate_horner") {
    // c = 1 + 2T + 3T^2 + 4T^3 + 5T^4 at T = 2, truncated after n_terms:
    // 1; 1+4 = 5; 5+12 = 17; 17+32 = 49; 49+80 = 129.
    const double c[5] = {1.0, 2.0, 3.0, 4.0, 5.0};
    CHECK(evaluate(c, 1, 2.0) == 1.0);
    CHECK(evaluate(c, 2, 2.0) == 5.0);
    CHECK(evaluate(c, 3, 2.0) == 17.0);
    CHECK(evaluate(c, 4, 2.0) == 49.0);
    CHECK(evaluate(c, 5, 2.0) == 129.0);
    // T = 0 leaves the constant term; negative T alternates the odd terms:
    // 1 - 2 + 3 - 4 + 5 = 3.
    CHECK(evaluate(c, 5, 0.0) == 1.0);
    CHECK(evaluate(c, 5, -1.0) == 3.0);
}

TEST_CASE("one_term_sun_centred_advances_at_gaussian_mean_motion") {
    // a = 1 AU: n = k / 1^1.5 = k. A century on, dt = 36525 d:
    // M = 10 + 0.985607668601425 * 36525 = 10 + 35999.32009566705.
    CHECK(mean_anomaly_deg(one_term(10.0, 1.0), kJ2000 + kCentury) ==
          doctest::Approx(36009.32009566705).epsilon(1e-14));
    // Not reduced to [0, 360), and backwards in time it runs backwards.
    CHECK(mean_anomaly_deg(one_term(10.0, 1.0), kJ2000 - kCentury) ==
          doctest::Approx(10.0 - 35999.32009566705).epsilon(1e-14));

    // a = 4 AU: a^1.5 = 8, n = k/8. M = 10 + 35999.32009566705 / 8
    // = 10 + 4499.915011958381.
    CHECK(mean_anomaly_deg(one_term(10.0, 4.0), kJ2000 + kCentury) ==
          doctest::Approx(4509.915011958381).epsilon(1e-14));

    // At the epoch, M is M0 exactly.
    CHECK(mean_anomaly_deg(one_term(10.0, 4.0), kJ2000) == 10.0);
}

TEST_CASE("one_term_earth_centred_is_slower_by_sqrt_mass_ratio") {
    // Same a = 4 AU orbit about the Earth: n = (k/8) / sqrt(332946.050895)
    // = (k/8) / 577.0147752830944, so the advance 4499.915011958381 becomes
    // 7.79861314599897 and M = 17.79861314599897.
    PolynomialElements sun = one_term(10.0, 4.0);
    PolynomialElements earth = sun;
    earth.origin = ElementOrigin::Earth;
    const double m_sun = mean_anomaly_deg(sun, kJ2000 + kCentury);
    const double m_earth = mean_anomaly_deg(earth, kJ2000 + kCentury);
    CHECK(m_earth == doctest::Approx(17.79861314599897).epsilon(1e-14));
    // The ratio of the advances is exactly the square root of the mass ratio:
    // the Earth alone, not the Earth-Moon system.
    CHECK((m_sun - 10.0) / (m_earth - 10.0) == doctest::Approx(577.0147752830944).epsilon(1e-13));
}

TEST_CASE("multi_term_mean_anomaly_is_the_bare_polynomial") {
    // M = 10 + 20T + 3T^2 at T = 2 (dt = 73050 d): 10 + 40 + 12 = 62, with no
    // mean motion added, whatever a and the centre say.
    PolynomialElements el;
    el.epoch_jd_tt = kJ2000;
    el.n_terms = 3;
    el.mean_anomaly[0] = 10.0;
    el.mean_anomaly[1] = 20.0;
    el.mean_anomaly[2] = 3.0;
    el.semi_major_axis[0] = 1.0;
    CHECK(mean_anomaly_deg(el, kJ2000 + 2.0 * kCentury) == doctest::Approx(62.0).epsilon(1e-15));
    el.origin = ElementOrigin::Earth;
    el.semi_major_axis[0] = 40.0;
    CHECK(mean_anomaly_deg(el, kJ2000 + 2.0 * kCentury) == doctest::Approx(62.0).epsilon(1e-15));
}

TEST_CASE("zero_padding_does_not_change_the_body") {
    // The same bodies carried with n_terms = 5, the extra coefficients zero:
    // M's meaning comes from which coefficients are nonzero, so the answers
    // are identical, not merely close.
    PolynomialElements classical = one_term(10.0, 4.0); // M at epoch
    PolynomialElements fitted;                          // M(T) = 10 + 20T + 3T^2
    fitted.epoch_jd_tt = kJ2000;
    fitted.n_terms = 3;
    fitted.mean_anomaly[0] = 10.0;
    fitted.mean_anomaly[1] = 20.0;
    fitted.mean_anomaly[2] = 3.0;
    fitted.semi_major_axis[0] = 1.0;
    for (const PolynomialElements& el : {classical, fitted}) {
        PolynomialElements padded = el;
        padded.n_terms = 5;
        for (double dt : {0.0, kCentury, -kCentury, 12345.6789, -0.5})
            CHECK(mean_anomaly_deg(padded, kJ2000 + dt) == mean_anomaly_deg(el, kJ2000 + dt));
    }
    // Spot-check the classical one against its derivation above: 4509.915...
    PolynomialElements padded = classical;
    padded.n_terms = 5;
    CHECK(mean_anomaly_deg(padded, kJ2000 + kCentury) ==
          doctest::Approx(4509.915011958381).epsilon(1e-14));
}

TEST_CASE("zero_m_rate_with_a_drifting_node_still_moves") {
    // The case the nonzero rule exists for: the node drifts (5 deg/century),
    // so the set needs n_terms = 2, but M is quoted at epoch with M[1] = 0.
    // The body still advances at n = k (a = 1): a century on,
    // M = 10 + 35999.32009566705 = 36009.32009566705, not the frozen 10.
    PolynomialElements el = one_term(10.0, 1.0);
    el.n_terms = 2;
    el.ascending_node[1] = 5.0;
    CHECK(mean_anomaly_deg(el, kJ2000 + kCentury) ==
          doctest::Approx(36009.32009566705).epsilon(1e-14));
    CHECK(mean_anomaly_deg(el, kJ2000) == 10.0);
}

TEST_CASE("mean_motion_uses_a_at_the_instant") {
    // n_terms = 2, M = 10 + 0T, a = 1 + 3T. A century on (T = 1), a(t) = 4,
    // a^1.5 = 8, n = k/8: M = 10 + 35999.32009566705 / 8 = 4509.915011958381.
    // Taking a at the epoch (a0 = 1) would instead give 36009.32009566705.
    PolynomialElements el = one_term(10.0, 1.0);
    el.n_terms = 2;
    el.semi_major_axis[1] = 3.0;
    CHECK(mean_anomaly_deg(el, kJ2000 + kCentury) ==
          doctest::Approx(4509.915011958381).epsilon(1e-14));
}

TEST_CASE("coefficients_beyond_n_terms_are_not_read") {
    // With n_terms = 1, a nonzero M[1] and a[1] stored past the count are
    // ignored: M is at epoch, a = 1, n = k, and a century on
    // M = 10 + 35999.32009566705.
    PolynomialElements el = one_term(10.0, 1.0);
    el.mean_anomaly[1] = 20.0;
    el.semi_major_axis[1] = 3.0;
    CHECK(mean_anomaly_deg(el, kJ2000 + kCentury) ==
          doctest::Approx(36009.32009566705).epsilon(1e-14));
}

TEST_CASE("any_nonzero_m_rate_takes_the_full_polynomial") {
    // M[1] = 1e-300 is nonzero, so M(T) = 10 + 1e-300 T is the mean anomaly
    // in full: a century on, 10 + 1e-300, which is 10 in double, with no
    // 35999-degree mean motion added.
    PolynomialElements el = one_term(10.0, 1.0);
    el.n_terms = 2;
    el.mean_anomaly[1] = 1e-300;
    CHECK(mean_anomaly_deg(el, kJ2000 + kCentury) == 10.0);
    // And exactly zero there is the at-epoch branch.
    el.mean_anomaly[1] = 0.0;
    CHECK(mean_anomaly_deg(el, kJ2000 + kCentury) ==
          doctest::Approx(36009.32009566705).epsilon(1e-14));
}

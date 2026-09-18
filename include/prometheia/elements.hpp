// SPDX-License-Identifier: GPL-2.0-or-later
//
// Bodies from polynomial orbital elements: the time dependence of the
// elements (ephemeris protocol v4, kind 4). The conventions and the
// specification this is written from are in docs/HYPOTHETICALS.md,
// "Mean anomaly and mean motion".
#ifndef PROMETHEIA_ELEMENTS_HPP
#define PROMETHEIA_ELEMENTS_HPP

#include <prometheia/engine.hpp>

namespace prometheia::elements {

// The Gaussian gravitational constant (IAU 1976), rad/day. The mean motion of
// a body from elements comes from this, not from the loaded ephemeris.
inline constexpr double kGaussK = 0.01720209895;

// The Sun-to-Earth mass ratio, the Earth alone, for Earth-centred orbits.
inline constexpr double kSunEarthMassRatio = 332946.050895;

// c[0] + c[1] T + ... + c[n_terms-1] T^(n_terms-1).
double evaluate(const double c[5], int n_terms, double T);

// The body's mean anomaly at jd_tt, in degrees (not reduced to [0, 360)).
double mean_anomaly_deg(const PolynomialElements& el, double jd_tt);

} // namespace prometheia::elements

#endif // PROMETHEIA_ELEMENTS_HPP

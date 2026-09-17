// SPDX-License-Identifier: GPL-2.0-or-later
//
// Apparent-place corrections: gravitational light deflection by the Sun
// and relativistic stellar aberration. Pure functions of rectangular
// barycentric (BCRS/ICRF) vectors; light-time iteration lives in the
// engine, which owns the ephemeris. Formulas from USNO Circular 179
// (Kaplan 2005) chapter 7, derivation notes in docs/ENGINE.md.
//
// Units: kilometres and km/day throughout.
#ifndef PROMETHEIA_APPARENT_HPP
#define PROMETHEIA_APPARENT_HPP

#include <cmath>

namespace prometheia::apparent {

// Speed of light, km/day (exact by definition of the metre).
inline constexpr double kLightKmPerDay = 299792.458 * 86400.0;

// Heliocentric gravitational constant (km^3/s^2, the DE440 value) and
// the Sun's gravitational radius GM/c^2 in km (~1.4766 km).
inline constexpr double kSunGmKm3S2 = 1.32712440041279419e11;
inline constexpr double kSunGmOverC2Km = kSunGmKm3S2 / (299792.458 * 299792.458);

// Deflects the observer->body vector p by the gravity of a body of
// gravitational radius gm_over_c2_km (the post-Newtonian single-body
// form, Circular 179 chapter 7):
//
//   p1 = p + (2 mu / (c^2 E)) / (1 + q.e) * ((p.q) e - (e.p) q)
//
// with p, q, e unit vectors along observer->body, deflector->body and
// deflector->observer, and E = |deflector->observer|. The result keeps
// |p|. Inputs may alias out.
inline void light_deflection(const double p[3], const double deflector_to_body[3],
                             const double deflector_to_observer[3], double gm_over_c2_km,
                             double out[3]) {
    const double pn = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    const double qn = std::sqrt(deflector_to_body[0] * deflector_to_body[0] +
                                deflector_to_body[1] * deflector_to_body[1] +
                                deflector_to_body[2] * deflector_to_body[2]);
    const double en = std::sqrt(deflector_to_observer[0] * deflector_to_observer[0] +
                                deflector_to_observer[1] * deflector_to_observer[1] +
                                deflector_to_observer[2] * deflector_to_observer[2]);
    if (pn == 0.0 || qn == 0.0 || en == 0.0) {
        for (int i = 0; i < 3; ++i)
            out[i] = p[i];
        return;
    }
    double u[3], q[3], e[3];
    for (int i = 0; i < 3; ++i) {
        u[i] = p[i] / pn;
        q[i] = deflector_to_body[i] / qn;
        e[i] = deflector_to_observer[i] / en;
    }
    const double pq = u[0] * q[0] + u[1] * q[1] + u[2] * q[2];
    const double ep = e[0] * u[0] + e[1] * u[1] + e[2] * u[2];
    const double qe = q[0] * e[0] + q[1] * e[1] + q[2] * e[2];
    const double f = 2.0 * gm_over_c2_km / en / (1.0 + qe);
    double w[3];
    for (int i = 0; i < 3; ++i)
        w[i] = u[i] + f * (pq * e[i] - ep * q[i]);
    const double wn = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    for (int i = 0; i < 3; ++i)
        out[i] = w[i] / wn * pn;
}

// Relativistic aberration of the observer->body vector p for an
// observer moving at barycentric velocity v (km/day), the Lorentz
// transformation of the direction (Circular 179 chapter 7):
//
//   p' = (p / gamma + (1 + (p.V) / (1 + 1/gamma)) V) / (1 + p.V)
//
// with p a unit vector, V = v/c and 1/gamma = sqrt(1 - V^2). The result
// keeps |p|. Inputs may alias out.
inline void aberration(const double p[3], const double v_km_day[3], double out[3]) {
    const double pn = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (pn == 0.0) {
        for (int i = 0; i < 3; ++i)
            out[i] = p[i];
        return;
    }
    double u[3], V[3];
    for (int i = 0; i < 3; ++i) {
        u[i] = p[i] / pn;
        V[i] = v_km_day[i] / kLightKmPerDay;
    }
    const double v2 = V[0] * V[0] + V[1] * V[1] + V[2] * V[2];
    const double inv_gamma = std::sqrt(1.0 - v2);
    const double pv = u[0] * V[0] + u[1] * V[1] + u[2] * V[2];
    const double k = 1.0 + pv / (1.0 + inv_gamma);
    double w[3];
    for (int i = 0; i < 3; ++i)
        w[i] = (inv_gamma * u[i] + k * V[i]) / (1.0 + pv);
    const double wn = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    for (int i = 0; i < 3; ++i)
        out[i] = w[i] / wn * pn;
}

} // namespace prometheia::apparent

#endif // PROMETHEIA_APPARENT_HPP

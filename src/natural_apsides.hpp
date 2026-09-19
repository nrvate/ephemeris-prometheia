// SPDX-License-Identifier: GPL-2.0-or-later
//
// The natural lunar apogee and perigee (docs/ORBIT-POINTS.md, "The natural
// apsides"): the model shared by the engine and its generator,
// tools/gen/gen_natural_apsides.cpp. The Moon's actual apsis passages
// deviate from the mean apse by up to ~6 degrees (apogee) and ~27 (perigee),
// mostly as a function of the Sun's elongation from the mean apse. The
// generator fits that function to every passage in a DE file; the engine
// adds it to the mean apse and interpolates only what remains between
// passages, so the curve is exact at each passage and shaped between them.
#ifndef PROMETHEIA_SRC_NATURAL_APSIDES_HPP
#define PROMETHEIA_SRC_NATURAL_APSIDES_HPP

#include <cmath>

#include "prometheia/frames.hpp"

namespace prometheia::natural_apsides {

// Harmonics of the elongation in the deviation model.
inline constexpr int kHarmonics = 12;
inline constexpr int kTerms = 2 * kHarmonics + 1;

// A smooth reference longitude of the mean perigee (apogee: + pi) in the mean
// ecliptic of J2000, radians: the Delaunay mean longitude of perigee of date
// less the IAU 2006 general precession in longitude. It only has to be the
// same function in the fit and the evaluation; the model absorbs the rest.
inline double mean_apse(double jd_tt, bool apogee) {
    double phi[14];
    frames::fundamental_arguments(jd_tt, phi);
    const double T = (jd_tt - 2451545.0) / 36525.0;
    const double p_a = (5028.796195 * T + 1.1054348 * T * T) / 3600.0 * M_PI / 180.0;
    const double varpi = phi[11] + phi[13] - phi[9] - p_a; // L - l
    return apogee ? varpi + M_PI : varpi;
}

// The model's value (radians) at elongation d = lon(Sun) - mean_apse, both in
// the mean ecliptic of J2000: c[0] + sum_k c[2k-1] sin(k d) + c[2k] cos(k d).
inline double model(const double c[kTerms], double d) {
    double v = c[0];
    for (int k = 1; k <= kHarmonics; ++k)
        v += c[2 * k - 1] * std::sin(k * d) + c[2 * k] * std::cos(k * d);
    return v;
}

} // namespace prometheia::natural_apsides

#endif // PROMETHEIA_SRC_NATURAL_APSIDES_HPP

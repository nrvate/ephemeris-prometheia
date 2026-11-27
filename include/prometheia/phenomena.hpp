// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia::phenomena — apparent magnitudes and the radii and poles they
// need (docs/PHENOMENA.md). Engine::phenomena() applies them to an object
// seen by an observer; the functions here are the published formulas alone,
// pure and thread-safe.
//
//   - Planets: Mallama & Hilton (2018), "Computing apparent planetary
//     magnitudes for The Astronomical Almanac", Astronomy and Computing 25,
//     10-24, equations 2-17.
//   - The Moon: Krisciunas & Schaefer (1991), PASP 103, 1033, eq. 9 (after
//     Allen 1976), at the Moon's mean distance and 1 au, scaled here by the
//     inverse squares of the actual distances.
//   - The Sun: Willmer (2018), ApJS 236, 47: V = -26.76 at 1 au.
//   - Small bodies: the H, G system of Bowell et al. (1989), Asteroids II.
//   - Radii and poles: the IAU WGCCRE 2015 report (Archinal et al. 2018,
//     Celest. Mech. Dyn. Astr. 130, 22), as NAIF's pck00011.tpc carries it.
#ifndef PROMETHEIA_PHENOMENA_HPP
#define PROMETHEIA_PHENOMENA_HPP

#include <string_view>

namespace prometheia::phenomena {

// A body's mean radius (km) by NAIF id (a planet's barycentre stands for the
// planet): the IAU's, the Sun's the IAU 2015 B3 nominal 695,700 km. 0 for a
// body the table does not hold.
double mean_radius_km(long naif);

// The unit vector of a planet's north pole in ICRF at a TDB Julian date, for
// the magnitudes that need it: Saturn's and Uranus's (by barycentre or
// centre). False for any other body.
bool pole_icrf(long naif, double jd_tdb, double out[3]);

// What a magnitude needs beyond the distances and the phase angle.
struct MagnitudeInputs {
    double r_au = 0.0; // Sun to body
    double d_au = 0.0; // observer to body
    double phase_angle_deg = 0.0;
    double jd_tt = 0.0; // Neptune's secular change
    // Saturn: the ring plane's planetocentric latitudes of the observer
    // and the Sun (deg). Uranus: the planetocentric latitudes of the
    // sub-observer and sub-solar points (deg).
    double sub_observer_lat_deg = 0.0, sub_solar_lat_deg = 0.0;
};

struct Magnitude {
    double v = 0.0;         // NaN outside every formula's range
    std::string_view model; // "Mallama & Hilton 2018 eq. 3", ...
};

// A planet (Mercury to Neptune, the Earth; by centre or barycentre), the
// Moon or the Sun. NaN, with the model naming why, for an unmodelled body
// (Pluto, the Earth-Moon barycentre) or a phase angle beyond every formula.
Magnitude body_magnitude(long naif, const MagnitudeInputs& in);

// H, G (Bowell et al. 1989).
Magnitude hg_magnitude(double h, double g, double r_au, double d_au, double phase_angle_deg);

} // namespace prometheia::phenomena

#endif // PROMETHEIA_PHENOMENA_HPP

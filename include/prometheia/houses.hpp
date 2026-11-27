// SPDX-License-Identifier: GPL-2.0-or-later
//
// House cusps and the chart's angles, from the sidereal time, the true
// obliquity and the latitude (docs/HOUSES.md). Each system is built from its
// published geometric definition; the arithmetic is our own and is checked
// against swetest's printed output only.
//
// Conventions: longitudes are on the ecliptic of date from the true equinox,
// degrees in [0, 360); the latitude is geodetic, north positive; the ARMC is
// the local apparent sidereal time as an angle. Cusps are numbered 1..12 and
// held at [0..11]; cusp 1 is the Ascendant and cusp 10 the MC in every
// system that places them there (all but Equal's cusp 10, Whole Sign,
// Morinus and Meridian's cusp 1).
//
// A system whose definition needs a semi-arc the sky does not have at that
// latitude (Placidus, Koch: a degree that never rises or sets) is refused
// with an ArgumentError naming the reason. It is never replaced by another
// system.

#ifndef PROMETHEIA_HOUSES_HPP
#define PROMETHEIA_HOUSES_HPP

#include <array>
#include <optional>
#include <string_view>

#include "prometheia/error.hpp"

namespace prometheia::houses {

enum class System {
    Placidus,      // P: semi-arcs trisected in time
    Koch,          // K: the MC degree's diurnal semi-arc trisected in time
    Porphyry,      // O: each quadrant's ecliptic arc trisected
    Regiomontanus, // R: the equator trisected, circles through the horizon's north and south points
    Campanus,      // C: the prime vertical trisected, circles through the same points
    Equal,         // A (also E): 30 degrees from the Ascendant
    WholeSign,     // W: the Ascendant's sign is the first house
    Alcabitius,    // B: the Ascendant's diurnal semi-arc trisected on the equator
    Morinus,       // M: the equator from the ARMC, projected along ecliptic latitude circles
    Meridian,      // X: the equator from the ARMC, projected along hour circles (axial rotation)
    Topocentric,   // T: Polich and Page's pole heights, tan(pole) = k/3 tan(latitude)
};

// The one-letter code astrology software uses ('E' is Equal as well as 'A'),
// and our lowercase token ("placidus", "whole-sign", ...). nullopt for any
// system not served.
std::optional<System> from_letter(char letter);
std::optional<System> from_token(std::string_view token);
char letter(System s);
std::string_view token(System s);
std::string_view name(System s); // "Placidus", "Whole Sign", ...

struct Angles {
    double asc_deg = 0.0;    // the ecliptic's intersection with the eastern horizon
    double mc_deg = 0.0;     // with the upper meridian
    double armc_deg = 0.0;   // local apparent sidereal time, as an angle
    double vertex_deg = 0.0; // with the prime vertical, in the west
    // The ecliptic degree rising at the equator: right ascension ARMC + 90.
    double equatorial_asc_deg = 0.0;
};

struct Houses {
    std::array<double, 12> cusp_deg{};
    Angles angles;
};

// The cusps and angles for an ARMC and true obliquity (radians) at a
// geodetic latitude strictly inside (-90, 90) degrees (radians).
Result<Houses> compute(System s, double armc_rad, double eps_true_rad, double lat_rad);

} // namespace prometheia::houses

#endif // PROMETHEIA_HOUSES_HPP

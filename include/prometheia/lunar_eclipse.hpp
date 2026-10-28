// SPDX-License-Identifier: GPL-2.0-or-later
//
// Lunar eclipses (docs/ECLIPSES.md, "Lunar eclipses"): the next or previous
// one, its contacts, magnitudes, gamma and saros, and how it looks from a
// site.
//
// Method: near each full moon the Moon's angular distance from the Earth's
// shadow axis (the direction opposite the geocentric apparent Sun) is
// minimised; that is the greatest eclipse. The shadow's angular radii are
// Danjon's, Rp = 1.01 pi + s + p (penumbra) and Ru = 1.01 pi - s + p
// (umbra), pi the Moon's horizontal parallax, s the Sun's semi-diameter and
// p the Sun's parallax. The contacts P1 U1 U2 U3 U4 P4 are the roots of the
// distance against Rp + m, Ru + m and Ru - m (m the Moon's semi-diameter).
// Everything is computed in TT from the Engine's geocentric apparent Sun and
// Moon; UT1 is TT less the Engine's Delta T.
//
// Threading: as the Engine's; one Engine per thread.
#ifndef PROMETHEIA_LUNAR_ECLIPSE_HPP
#define PROMETHEIA_LUNAR_ECLIPSE_HPP

#include "prometheia/engine.hpp"
#include "prometheia/error.hpp"
#include "prometheia/frames.hpp"

namespace prometheia::lunar_eclipse {

// The constants of the model, all stated in docs/ECLIPSES.md.
inline constexpr double kEarthEquatorialRadiusKm = 6378.1366; // the parallax's radius
inline constexpr double kMoonRadiusKm = 1737.4;
inline constexpr double kSunRadiusKm = 695700.0;
inline constexpr double kDanjon = 1.01; // enlargement of the Moon's parallax in both radii

// A bitmask of the kinds `next` accepts.
inline constexpr unsigned kTotal = 1, kPartial = 2, kPenumbral = 4, kAllKinds = 7;

enum class Kind { Penumbral = 0, Partial = 1, Total = 2 };

// Indexes of Eclipse::contact_tt.
enum Contact { P1 = 0, U1 = 1, U2 = 2, U3 = 3, U4 = 4, P4 = 5, kContacts = 6 };

struct Eclipse {
    Kind kind = Kind::Penumbral;
    int saros_series = 0;                     // van den Bergh's number (see saros_of)
    int saros_member = 0;                     // 1-based; 0 when the series' start is not known
    double jd_max_tt = 0.0, jd_max_ut1 = 0.0; // the greatest eclipse
    // Distance of the Moon's centre from the shadow axis at the greatest
    // eclipse in Earth equatorial radii, positive when the Moon is north of
    // the axis (ecliptic latitude).
    double gamma = 0.0;
    // Fractions of the Moon's diameter inside the umbra / penumbra; the
    // umbral one is negative when the Moon misses the umbra (the
    // convention of NASA's canon).
    double umbral_magnitude = 0.0, penumbral_magnitude = 0.0;
    // Angular radii at the greatest eclipse, degrees.
    double umbra_radius_deg = 0.0, penumbra_radius_deg = 0.0, moon_semidiameter_deg = 0.0;
    // P1 U1 U2 U3 U4 P4; NaN for a contact that does not occur.
    double contact_tt[kContacts] = {};
    double contact_ut1[kContacts] = {};
};

// The first eclipse of the kinds in the bitmask whose greatest eclipse is
// after (before, with backward) jd_start_tt. NotFound after ~160 years of
// none; the Engine's coverage errors pass through.
Result<Eclipse> next(Engine& engine, double jd_start_tt, unsigned kinds = kAllKinds,
                     bool backward = false);

// The eclipse whose greatest is within two days of jd_tt (NotFound if none).
Result<Eclipse> at(Engine& engine, double jd_tt);

// The Moon seen from a site at the instants of an eclipse: index Contact for
// the contacts, kMaximum for the greatest eclipse.
inline constexpr int kMaximum = 6;
struct Local {
    Eclipse eclipse;
    // Topocentric altitude of the Moon's centre, degrees, true and with the
    // standard atmosphere's refraction; NaN where the contact does not occur.
    double altitude_deg[kMaximum + 1] = {};
    double apparent_altitude_deg[kMaximum + 1] = {};
};
Result<Local> local(Engine& engine, double jd_tt, const frames::GeoSite& site);

// The saros series (van den Bergh 1955) and member of an eclipse whose
// greatest eclipse is at jd_tt, from the lunation alone.
struct Saros {
    int series = 0;
    int member = 0; // 0 when the series' first eclipse is not in the table
};
Saros saros_of(double jd_tt);

} // namespace prometheia::lunar_eclipse

#endif

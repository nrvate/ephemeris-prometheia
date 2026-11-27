// SPDX-License-Identifier: GPL-2.0-or-later
//
// Solar eclipses: global circumstances, the shadow axis on the ground at an
// instant, and local circumstances at a site (docs/SOLAR_ECLIPSES.md).
//
// Everything rests on the Besselian elements x, y, d, mu, l1, l2, tan f1,
// tan f2, computed at any instant from the engine's geocentric apparent Sun
// and Moon (Explanatory Supplement to the Astronomical Almanac, 3rd ed.,
// ch. 11; Meeus, Elements of Solar Eclipses 1951-2200, 1989). Greatest
// eclipse is the instant of least distance between the shadow axis and the
// Earth's centre, sqrt(x^2 + y^2), and gamma is that distance (NASA's
// definition), signed positive when the axis passes north of the centre.
//
// Units: x, y, l1, l2 in Earth equatorial radii (WGS84 a = 6378.137 km); d
// and mu in radians; times Julian dates, TT unless named ut1. The Moon's
// limb is a circle of radius k Earth radii (no limb profile, as in NASA's
// canon): the constants are NASA's, 0.272488 for the penumbra and 0.272281
// for the umbra, and an Options field exposes both.
//
// Not thread-safe (an Engine is not); no globals.
#ifndef PROMETHEIA_SOLAR_ECLIPSE_HPP
#define PROMETHEIA_SOLAR_ECLIPSE_HPP

#include "prometheia/engine.hpp"
#include "prometheia/error.hpp"
#include "prometheia/frames.hpp"
#include "prometheia/time.hpp"

namespace prometheia::solar_eclipse {

inline constexpr double kEarthRadiusKm = 6378.137;         // WGS84 equatorial radius
inline constexpr double kFlattening = 1.0 / 298.257223563; // WGS84
inline constexpr double kAuKm = 149597870.7;

// Type bits. A global eclipse carries exactly one of the four kinds, plus
// kCentral or kNonCentral for the three umbral kinds (a partial eclipse has
// neither). A local circumstance carries one of kPartial/kAnnular/kTotal.
enum Type : unsigned {
    kPartial = 1,
    kAnnular = 2,
    kTotal = 4,
    kHybrid = 8, // annular-total: l2 changes sign along the central line
    kAllKinds = 15,
    kCentral = 16,    // the shadow axis meets the Earth
    kNonCentral = 32, // the umbra touches the Earth but its axis misses it
};

struct Options {
    double k_umbra = 0.272281;    // the Moon's radius over the Earth's, for the umbra
    double k_penumbra = 0.272488; // ... for the penumbra
    double sun_radius_km = 696000.0;
    // Delta T for TT <-> UT1 (the earth's rotation). nullptr: the default
    // (time::delta_t), which is also the Engine's unless it was replaced.
    const time::DeltaTModel* delta_t = nullptr;
};

struct Besselian {
    double jd_tt = 0.0;
    double jd_ut1 = 0.0;
    double x = 0.0, y = 0.0;   // the axis in the fundamental plane (Earth radii)
    double d_rad = 0.0;        // declination of the axis
    double ra_rad = 0.0;       // right ascension of the axis (true equator of date)
    double mu_rad = 0.0;       // Greenwich hour angle of the axis (GAST - ra), [0, 2pi)
    double l1 = 0.0, l2 = 0.0; // penumbral and umbral radii in the plane (l2 < 0: total)
    double tan_f1 = 0.0, tan_f2 = 0.0;
    double z_moon = 0.0; // the Moon's height above the plane, toward the Sun
};

// The elements at a TT instant.
Result<Besselian> besselian_elements(Engine& engine, double jd_tt, const Options& opts = {});

struct Place {
    double lat_deg = 0.0; // geodetic; NaN when absent
    double lon_deg = 0.0; // east positive, (-180, 180]; NaN when absent
};

// The whole eclipse, from the first to the last touch of the penumbra.
struct Global {
    unsigned type = 0;                  // Type bits
    long lunation = 0;                  // NASA's number: 0 at the new moon of 2000-01-06
    int saros = 0;                      // van den Bergh's series number
    double max_tt = 0.0, max_ut1 = 0.0; // greatest eclipse
    double gamma = 0.0;
    // At the greatest-eclipse point. magnitude is NASA's canon convention: the
    // fraction of the Sun's diameter covered for a partial eclipse, the ratio of
    // the Moon's to the Sun's apparent diameter for a total, annular or hybrid
    // one. magnitude_covered is the fraction of the diameter covered in every
    // case (above 1 when the Moon covers more than the disc).
    double magnitude = 0.0;
    double magnitude_covered = 0.0;
    double obscuration = 0.0; // fraction of the Sun's disc covered
    Place greatest;           // the point of greatest eclipse (the nearest limb point if the
                              // axis misses the Earth)
    double sun_alt_deg = 0.0; // geometric altitude of the Sun's centre there
    double sun_az_deg = 0.0;
    double path_width_km = 0.0;      // umbral path width at greatest eclipse; NaN if none
    double central_duration_s = 0.0; // totality/annularity there, centre line; NaN if none
    // First and last contacts, TT; NaN where the event does not occur.
    // p: the penumbra touches the Earth; u: the umbra touches it; c: the
    // axis meets it.
    double p1_tt = 0.0, u1_tt = 0.0, c1_tt = 0.0, c2_tt = 0.0, u4_tt = 0.0, p4_tt = 0.0;
    Besselian elements; // at max_tt
};

// The next eclipse whose greatest-eclipse instant is after jd_start_tt (the
// last before it, with backward) and whose kind is among `kinds` (Type kind
// bits). Searches up to ~4 years; NotFound beyond.
Result<Global> next_global(Engine& engine, double jd_start_tt, unsigned kinds, bool backward,
                           const Options& opts = {});

// The shadow axis on the ground at an instant.
struct Where {
    double jd_tt = 0.0, jd_ut1 = 0.0;
    bool axis_hits = false;         // the axis meets the Earth; the rest is NaN if not
    unsigned type = 0;              // kTotal or kAnnular at the axis point (0 if not axis_hits)
    Place central;                  // the centre line
    Place north_limit, south_limit; // the umbra's limits across the track (NaN if the
                                    // umbra leaves the Earth there)
    double sun_alt_deg = 0.0;       // at the axis point
    double path_width_km = 0.0;     // between the limits
    double central_duration_s = 0.0;
    double magnitude = 0.0, obscuration = 0.0; // at the axis point
    Besselian elements;
};
Result<Where> where(Engine& engine, double jd_tt, const Options& opts = {});

// What one site sees. Times are NaN where the contact does not happen or the
// Sun is below the horizon then ("missing"). magnitude/obscuration are at the
// local maximum even when the Sun is down there (check visible_max).
struct Local {
    unsigned type = 0; // kPartial, kAnnular or kTotal, as seen here
    long lunation = 0;
    int saros = 0;
    double contact_tt[4] = {}, contact_ut1[4] = {}; // C1..C4
    double contact_alt_deg[4] = {}, contact_az_deg[4] = {};
    double max_tt = 0.0, max_ut1 = 0.0;
    double max_alt_deg = 0.0, max_az_deg = 0.0;
    bool visible_max = false;    // the Sun's centre is above the horizon at maximum
    double magnitude = 0.0;      // fraction of the Sun's diameter
    double obscuration = 0.0;    // fraction of the Sun's disc
    double diameter_ratio = 0.0; // Moon / Sun apparent diameters at maximum
    double duration_s = 0.0;     // C2 to C3; NaN if either is missing
    double gamma = 0.0;          // of the global eclipse
    double global_max_tt = 0.0;
};

// The circumstances of a global eclipse at a site. NotFound if the site is
// outside the penumbra at all times (no eclipse there).
Result<Local> local_at(Engine& engine, const Global& eclipse, const frames::GeoSite& site,
                       const Options& opts = {});

// The next eclipse (the last, backward) after jd_start_tt that is visible
// from the site: seen in some phase with the Sun above the horizon.
Result<Local> local_next(Engine& engine, double jd_start_tt, const frames::GeoSite& site,
                         bool backward, const Options& opts = {});

// Van den Bergh's saros number of the eclipse at NASA lunation `lunation`.
// Members of a series are 223 lunations apart and the series number
// advances by 38 per lunation modulo 223; the offset is pinned by the total
// eclipse of 2017-08-21 (lunation 218), published as saros 145 (NASA).
// The 223 residues map to -20..202, the canon's range being -13..190.
int saros_number(long lunation);

} // namespace prometheia::solar_eclipse

#endif // PROMETHEIA_SOLAR_ECLIPSE_HPP

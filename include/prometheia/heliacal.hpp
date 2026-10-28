// SPDX-License-Identifier: GPL-2.0-or-later
//
// Heliacal visibility: the first and last sightings of an object near the
// Sun (docs/HELIACAL.md).
//
// Two models decide whether an object can be seen at one moment of twilight:
//
//   - Schaefer: the visibility model of Schaefer (1993), "Astronomy and the
//     limits of vision", Vistas in Astronomy 36, 311-361 -- the object's
//     magnitude dimmed by the layered extinction (eqs. 2b, 3, 4b, 5, 6, 8b)
//     against a limiting magnitude that follows from the sky's brightness
//     (eqs. 14, 15) and Hecht's threshold (eqs. 10, 34), corrected for the
//     observer's age and Snellen ratio (eqs. 29-33). Equation numbers are
//     the paper's, in the source and the docs.
//   - Arcus visionis: the object is seen when the Sun is at least the arc
//     (a caller-supplied number of degrees) below the horizon and the
//     object at least a given altitude above it.
//
// The finder steps from the start date a day at a time, and on each day asks
// whether the object is visible at its best moment of the morning (or
// evening) twilight; an event is the first (or last) day of a run of such
// days. Everything is computed in TT and reported with UT1 beside it.
//
// Not thread-safe (an Engine is not); no globals.
#ifndef PROMETHEIA_HELIACAL_HPP
#define PROMETHEIA_HELIACAL_HPP

#include <cmath>

#include "prometheia/engine.hpp"
#include "prometheia/error.hpp"
#include "prometheia/frames.hpp"

namespace prometheia::heliacal {

// The four events, numbered as swetest's -hev types. Stars have the first
// two; planets and the Moon all four.
enum class Event {
    Rising = 1,       // morning first: the first morning the object is seen before sunrise
    Setting = 2,      // evening last: the last evening it is seen after sunset
    EveningFirst = 3, // the first evening it is seen after sunset
    MorningLast = 4,  // the last morning it is seen before sunrise
};

enum class Model { Schaefer, ArcusVisionis };

// The air. Pressure and temperature set the refraction; humidity gives the
// aerosol extinction (eq. 6a) unless one of the two coefficients below is
// finite: extinction_coefficient is the total extinction k (magnitudes per
// air mass, day vision, wavelength 0.55 um) and visual_range_km a
// meteorological range (Koschmieder). At most one may be finite.
struct Atmosphere {
    double pressure_hpa = 1013.25;
    double temperature_c = 15.0;
    double relative_humidity_pct = 40.0;
    double extinction_coefficient = std::nan("");
    double visual_range_km = std::nan("");
};

// The eye. The limits are those of a naked-eye observer of this age and
// Snellen ratio (1 is 20/20, 2 is 20/10), relative to Schaefer's standard
// observer (25 years, binocular).
struct Observer {
    double age_years = 36.0;
    double snellen_ratio = 1.0;
    bool binocular = true; // false: monocular, a factor sqrt(2) in the threshold (eq. 31b)
};

struct Options {
    Model model = Model::Schaefer;
    // Arcus visionis only: the Sun's depression (degrees below the geometric
    // horizon, positive) the object needs, and its apparent altitude then.
    // No default arc is supplied: the caller gives it (NaN is refused).
    double arcus_visionis_deg = std::nan("");
    double object_altitude_deg = 0.0;
    // Schaefer only: how far above the threshold the object must be, in
    // magnitudes. 0 is the paper's 50 % detection threshold, which finds
    // heliacal events 3 to 9 days early against swetest's; the default 1.2
    // is a constant fitted to swetest's printed events (docs/HELIACAL.md,
    // "Validation"), not a number from the paper, which refers to an
    // experience correction without giving one.
    double detection_margin_mag = 1.2;
    // Schaefer only: add the Moon's light to the sky (eq. 15b). Off, the finder
    // looks for the object's own heliacal event, not for a night the Moon spares.
    bool moonlight = false;
    // Schaefer only: the dark-sky zenith brightness B_o of eq. 15a, nanoLamberts.
    double dark_sky_nl = 180.0;
    // The finder tests days this far apart before it looks between them
    // (0: four, and one for the Moon).
    int day_step = 0;
    // How far past the start date the finder looks.
    double search_days = 800.0;
};

// The extinction coefficients at the site (magnitudes per air mass), for day
// or night vision (the latter at 0.51 um: Rayleigh x1.35, ozone x0.3, aerosol x1.10).
struct Extinction {
    double rayleigh = 0.0, ozone = 0.0, aerosol = 0.0, total = 0.0;
};
Extinction extinction(double height_m, double lat_deg, double sun_ra_deg, const Atmosphere& atm,
                      bool night_vision);

// Air masses at the apparent zenith distance z_deg (clamped to [0, 90]):
// eq. 2b (the whole atmosphere), eq. 3a for an exponential layer of scale
// height scale_height_km, eq. 3b for a shell at height_km.
double air_mass(double z_deg);
double air_mass_layer(double z_deg, double scale_height_km);
double air_mass_shell(double z_deg, double height_km);

// V magnitude <-> illuminance outside the atmosphere in foot-candles (eq. 10).
double illuminance_from_magnitude(double v);
double magnitude_from_illuminance(double foot_candles);

// What the sky looks like at one instant, from one place.
struct Scene {
    double object_alt_deg = 0.0;  // apparent (refracted) altitude
    double object_v = 0.0;        // V magnitude outside the atmosphere
    double sun_alt_deg = 0.0;     // true altitude
    double sun_sep_deg = 0.0;     // object to Sun
    double sun_ra_deg = 0.0;      // for the ozone and aerosol seasons
    double moon_alt_deg = -90.0;  // true altitude
    double moon_sep_deg = 180.0;  // object to Moon
    double moon_v = std::nan(""); // NaN: no Moon term
    double site_height_m = 0.0;
    double site_lat_deg = 0.0;
};

struct Visibility {
    double sky_brightness_nl = 0.0; // B_sky, eq. 15e (no glare, no instrument)
    bool night_vision = false;      // the extinction used the 0.51 um coefficients
    Extinction k;
    double air_mass_object = 0.0;   // eq. 2b, for reference
    double extinction_mag = 0.0;    // A_m of the object, eq. 8b
    double limit_outside_mag = 0.0; // the faintest V seen, outside the atmosphere
    double limit_at_eye_mag = 0.0;  // limit_outside_mag + extinction_mag
    double object_at_eye_mag = 0.0; // object_v + extinction_mag
    double margin_mag = 0.0;        // limit_at_eye_mag - object_at_eye_mag; > 0: seen
};
// Schaefer's model at one scene. The object must be above the horizon
// (object_alt_deg >= 0); below it, margin_mag is -infinity.
Visibility visibility(const Scene& scene, const Atmosphere& atm, const Observer& observer,
                      double dark_sky_nl = 180.0);

// The limiting magnitude at the eye for a point source on a uniform sky of
// b_nl nanoLamberts (eqs. 29-34 for the naked eye), before extinction.
// visibility() uses it; it is exposed for the checks.
double limiting_magnitude(double b_nl, const Observer& observer);

// The scene an engine makes of an object at a TT instant.
Result<Scene> scene_at(Engine& engine, const Engine::ObjectRef& object, double jd_tt,
                       const frames::GeoSite& site, const Atmosphere& atm);

struct Sight {
    double jd_tt = 0.0, jd_ut1 = 0.0;
    Scene scene;
    double sun_depression_deg = 0.0;
    double object_az_deg = 0.0;
    Visibility seen;     // Schaefer; for arcus visionis only the geometry fields are set
    double margin = 0.0; // the model's margin: magnitudes (Schaefer) or degrees (arcus)
};
// The object's visibility at a TT instant under the model in `options`.
Result<Sight> sight_at(Engine& engine, const Engine::ObjectRef& object, double jd_tt,
                       const frames::GeoSite& site, const Atmosphere& atm, const Observer& observer,
                       const Options& options);

struct Found {
    Event event = Event::Rising;
    Model model = Model::Schaefer;
    double jd_tt = 0.0, jd_ut1 = 0.0;           // the event: the start of the visibility window
    double optimum_tt = 0.0, optimum_ut1 = 0.0; // the best moment (largest margin)
    double end_tt = 0.0, end_ut1 = 0.0;         // the end of the window
    Sight at_optimum;
    // The inputs as used.
    Atmosphere atmosphere;
    Observer observer;
    Options options;
};

// The first event strictly after jd_start_tt. ArgumentError for a bad input
// (a star with an evening-first or morning-last event, an object with no
// magnitude under Schaefer, no arc under arcus visionis); NotFound when none
// falls within options.search_days.
Result<Found> next(Engine& engine, const Engine::ObjectRef& object, double jd_start_tt,
                   const frames::GeoSite& site, const Atmosphere& atm, const Observer& observer,
                   Event event, const Options& options = {});

} // namespace prometheia::heliacal

#endif // PROMETHEIA_HELIACAL_HPP

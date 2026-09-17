// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia::Engine — the positions API. An Engine owns one planetary
// ephemeris (a JPL DE binary or a NAIF SPK kernel, detected from the
// file) and answers calc(body, time, options) with ecliptic or
// equatorial coordinates, their daily rates, and the provenance of the
// answer.
//
// Pipeline for one query (details and validation in docs/ENGINE.md):
//   1. TT -> TDB; observer barycentric state (Earth, Earth + site, Sun
//      or the barycentre).
//   2. Light time: the body's barycentric position at t - tau, iterated
//      to convergence.
//   3. Gravitational deflection by the Sun, then relativistic annual
//      (and diurnal, when topocentric) aberration.
//   4. ICRF -> frame bias -> IAU 2006 precession -> IAU 2000A nutation
//      as the requested frame demands, then spherical coordinates.
// Rates come from central differences of the whole pipeline (h = 0.001
// day), so they are rates of the *apparent* coordinates.
//
// Body identity is the NAIF integer ID, the same key the catalog uses
// for small bodies. For Mars through Pluto the JPL planetary files carry
// system barycentres only (IDs 4-9); the planet-centre IDs 499..999 are
// answered only by an ephemeris that has them.
//
// Threading: an Engine caches ephemeris records and frame matrices and
// is not safe for concurrent use. Give each thread its own Engine;
// there is no process-wide state.
#ifndef PROMETHEIA_ENGINE_HPP
#define PROMETHEIA_ENGINE_HPP

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "prometheia/error.hpp"
#include "prometheia/frames.hpp"
#include "prometheia/time.hpp"

namespace prometheia {

namespace body {
constexpr int kSolarSystemBary = 0;
constexpr int kEarthMoonBary = 3;
constexpr int kSun = 10;
constexpr int kMercury = 199;
constexpr int kVenus = 299;
constexpr int kEarth = 399;
constexpr int kMoon = 301;
constexpr int kMars = 4; // system barycentres from here on
constexpr int kJupiter = 5;
constexpr int kSaturn = 6;
constexpr int kUranus = 7;
constexpr int kNeptune = 8;
constexpr int kPluto = 9;
} // namespace body

// Where the observer is.
enum class Center {
    Geocentric,   // the geocentre
    Topocentric,  // a site on the WGS84 ellipsoid (CalcOptions::site)
    Heliocentric, // the Sun's centre
    Barycentric,  // the solar-system barycentre
};

// Reference frame of the output.
enum class Frame {
    ICRF,       // ICRF axes; ecliptic output uses the J2000 mean obliquity
    J2000,      // mean equator and equinox of J2000.0 (ICRF + frame bias)
    MeanOfDate, // + IAU 2006 precession
    TrueOfDate, // + IAU 2000A nutation (equinox of date)
};

enum class Coords {
    Ecliptic,   // longitude, latitude
    Equatorial, // right ascension, declination
};

struct CalcOptions {
    Center center = Center::Geocentric;
    Frame frame = Frame::TrueOfDate;
    Coords coords = Coords::Ecliptic;
    bool light_time = true; // retarded position of the body
    bool deflection = true; // Sun's gravitational light bending
    bool aberration = true; // observer-velocity aberration
    bool speed = true;      // compute daily rates (3x the work)
    frames::GeoSite site{}; // Center::Topocentric only

    // Presets. apparent(): what an observer sees, in the true equinox of
    // date. astrometric(): light time only (ICRF-style catalogue place).
    // geometric(): instantaneous positions, no corrections.
    static CalcOptions apparent() { return CalcOptions{}; }
    static CalcOptions astrometric() {
        CalcOptions o;
        o.deflection = o.aberration = false;
        return o;
    }
    static CalcOptions geometric() {
        CalcOptions o;
        o.light_time = o.deflection = o.aberration = false;
        return o;
    }
};

struct Position {
    // Ecliptic: longitude [0, 360), latitude; equatorial: right
    // ascension [0, 360), declination. Degrees; distance in AU.
    double lon_deg = 0.0;
    double lat_deg = 0.0;
    double dist_au = 0.0;
    double lon_speed = 0.0;  // degrees/day
    double lat_speed = 0.0;  // degrees/day
    double dist_speed = 0.0; // AU/day
    // The same vector in rectangular form, in the output frame.
    double xyz_au[3] = {0.0, 0.0, 0.0};
    double vel_au_day[3] = {0.0, 0.0, 0.0};
};

struct Provenance {
    std::string_view source;      // e.g. "JPL DE440 binary" (valid while the Engine lives)
    int denum = 0;                // DE number when known, else 0
    double light_time_days = 0.0; // tau applied (0 without light_time)
};

struct CalcResult {
    Position pos;
    Provenance provenance;
    // 1-sigma positional uncertainty. Absent for the planetary
    // ephemeris, which publishes no per-epoch covariance; the catalog
    // overlay (small bodies) fills it.
    std::optional<double> sigma_arcsec;
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;

    // Opens a planetary ephemeris: a JPL DE binary or a DAF/SPK kernel
    // (detected by content, not extension).
    static Result<Engine> open(const std::string& ephemeris_path);

    // Position of `body` at a TT Julian date.
    Result<CalcResult> calc(int body, double jd_tt, const CalcOptions& opts = {});

    // Same, at a UT1 Julian date, converted with the Delta T model.
    Result<CalcResult> calc_ut(int body, double jd_ut1, const CalcOptions& opts = {});

    // Delta T model for UT inputs and topocentric Earth rotation. Not
    // owned; nullptr restores the default (time::ObservedDeltaT).
    void set_delta_t_model(const time::DeltaTModel* model);

    // Human-readable description of the loaded ephemeris.
    std::string_view source() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace prometheia

#endif // PROMETHEIA_ENGINE_HPP

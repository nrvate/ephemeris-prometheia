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
//      as the requested frame demands, then spherical coordinates; a
//      sidereal option then rotates the longitude zero point by the
//      zodiac's ayanamsha.
// Rates come from central differences of the whole pipeline (h = 0.001
// day), so they are rates of the *apparent* coordinates.
//
// Body identity is the NAIF integer ID, the same key the catalog uses
// for small bodies. For Mars through Pluto the JPL planetary files carry
// system barycentres only (IDs 4-9); the planet-centre IDs 499..999 are
// answered only by an ephemeris that has them. Bodies the ephemeris
// does not know are looked up in the catalogs added with add_catalog()
// — small bodies by SPK-ID, integrated on demand.
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

// Sidereal zodiac for the output. Tropical (the default) answers plain
// positions; the other modes subtract the zodiac's ayanamsha from the
// ecliptic longitude of date. The integer values of the published
// modes follow the Swiss Ephemeris command-line numbering (swetest
// -ay<mode>). User anchors the zodiac at an explicit epoch and value
// (CalcOptions::sidereal_epoch_jtdb, sidereal_ayanamsa_deg — the MEAN
// ayanamsha at that TT epoch).
enum class SiderealMode : int {
    Tropical = -1,
    FaganBradley = 0,
    Lahiri = 1,
    User = 255,
};

struct CalcOptions {
    Center center = Center::Geocentric;
    Frame frame = Frame::TrueOfDate;
    Coords coords = Coords::Ecliptic;
    SiderealMode sidereal = SiderealMode::Tropical;
    double sidereal_epoch_jtdb = 0.0;   // SiderealMode::User anchor epoch
    double sidereal_ayanamsa_deg = 0.0; // SiderealMode::User anchor value
    bool light_time = true;             // retarded position of the body
    bool deflection = true;             // Sun's gravitational light bending
    bool aberration = true;             // observer-velocity aberration
    bool speed = true;                  // compute daily rates (3x the work)
    bool sigma = true;                  // catalog bodies: sigma_arcsec (12 extra integrations)
    frames::GeoSite site{};             // Center::Topocentric only

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
    // 1-sigma uncertainty of the body's direction on the sky, in
    // arcsec: the square root of the larger eigenvalue of the position
    // covariance projected on the plane perpendicular to the
    // observer->body line, divided by the observer->body distance. The
    // covariance is the orbit solution's full element covariance (the
    // record's kCovariance block, at its own epoch) propagated through the
    // same integration that produces the position; the observer is exact.
    // Present (>= 0, zero for an all-zero covariance) only for catalog
    // bodies whose record carries a covariance — otherwise absent: the
    // planetary ephemeris publishes none, and per-element sigmas alone are
    // uncorrelated summaries that would overstate it 10-1000x (measured
    // against JPL Horizons), so they are not used. Eigenvalues survive the frame
    // rotations, so this is frame-independent and ignores the
    // light-optics corrections (deflection, aberration).
    std::optional<double> sigma_arcsec;

    // The longitude shift applied for a sidereal output (degrees): the
    // zodiac's true ayanamsha in the of-date frames, its mean value for
    // the J2000/ICRF frames (whose ecliptic is the mean ecliptic of
    // J2000, where the ayanamsha has a fixed value). Absent for a
    // tropical request.
    std::optional<double> ayanamsa_deg;
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

    // Adds a small-body catalog (an EPM1 container, docs/FORMAT.md).
    // Catalog bodies answer calc() under their SPK-ID — the same NAIF
    // integer ID space as the planets. Positions come from on-demand
    // integration of the catalog's osculating elements in a barycentric
    // point-mass force field built from the engine's own ephemeris
    // (Sun plus Mercury..Pluto at their system barycentres; Earth and
    // Moon split from the Earth-Moon barycentre), memoized per body.
    // Records carrying a full covariance also answer CalcResult::
    // sigma_arcsec. May be called more than once; the newest catalog
    // wins when several carry the same body, and any small-body
    // positions and uncertainties cached so far are invalidated.
    Result<void> add_catalog(const std::string& catalog_path);

    // Adds an asteroid perturber kernel (a NAIF SPK file of heliocentric
    // segments for numbered asteroids under ids 2000000 + number, such as
    // JPL's sb441-n16.bsp). Every body with a known mass — the DE file's
    // MAnnnn constant, or the built-in DE440 table for the SB441-N16 set —
    // joins the force model for catalog bodies; a body never perturbs itself
    // (the SBDB SPK-ID 20000000 + number is matched). The kernel's bodies are
    // still integrated from the catalog's current elements: the kernel's own
    // trajectories come from older orbit solutions (measured tens of km off
    // the current ones near the present). Replaces an earlier perturber
    // kernel and invalidates the integrated trajectories and uncertainty
    // tracks.
    Result<void> add_perturbers(const std::string& spk_path);

    // Drops every memoized small-body trajectory and uncertainty track (they
    // rebuild on the next query that needs them). Integrated trajectories are
    // kept for the engine's lifetime otherwise — a few kilobytes per body per
    // year of coverage — so a long-running process or a whole-catalog sweep
    // calls this to bound memory. The perturber tables are kept.
    void release_small_bodies();

    // Resolves a small body by its primary designation or proper name —
    // "1", "Ceres", "ceres" — to its SPK-ID, the integer calc() takes.
    // Matching is ASCII-case-insensitive. The index is built from the
    // catalogs added with add_catalog(); when several of them carry the
    // same name, the newest one wins. NotFound when no loaded catalog
    // answers the name (planets are not indexed; address those by their
    // NAIF IDs directly).
    Result<int> lookup(std::string_view name) const;

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

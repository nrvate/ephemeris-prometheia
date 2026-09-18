/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * prometheia.h: the C interface to Ephemeris Prometheia.
 *
 * A thin, stable-layout shim over prometheia::Engine (engine.hpp) for C
 * programs and language bindings. It adds no behaviour of its own: every
 * answer is bit-identical to the C++ engine's. Conventions:
 *
 *   - Handles are opaque. prometheia_engine_open() creates one,
 *     prometheia_engine_close() frees it. One engine per thread: an
 *     engine caches ephemeris records and is not safe for concurrent use.
 *     There is no process-wide state.
 *   - Fallible functions return a prometheia_status (PROMETHEIA_OK = 0)
 *     and take an optional trailing prometheia_error* (NULL is fine) that
 *     receives the same code plus a message. No exception crosses this
 *     interface; an allocation failure or unexpected internal fault
 *     reports PROMETHEIA_ERROR_INTERNAL.
 *   - Integer fields that select an option are plain ints validated at
 *     the boundary: an out-of-range value is PROMETHEIA_ERROR_ARGUMENT,
 *     never undefined behaviour.
 *   - Structs are laid out for this ABI version only
 *     (PROMETHEIA_ABI_VERSION); any layout change bumps it. Bindings can
 *     compare against prometheia_abi_version() at load time.
 *
 * Details of the model (pipeline, frames, validation): docs/ENGINE.md;
 * this interface: docs/C_API.md.
 */
#ifndef PROMETHEIA_H
#define PROMETHEIA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PROMETHEIA_API __attribute__((visibility("default")))
#else
#define PROMETHEIA_API
#endif

#define PROMETHEIA_ABI_VERSION 5

/* ---- Status and errors ------------------------------------------------ */

typedef enum prometheia_status {
    PROMETHEIA_OK = 0,
    PROMETHEIA_ERROR_IO = 1,         /* a file could not be opened or read */
    PROMETHEIA_ERROR_FORMAT = 2,     /* not a recognised or valid file */
    PROMETHEIA_ERROR_CORRUPTION = 3, /* checksum mismatch or truncation */
    PROMETHEIA_ERROR_ARGUMENT = 4,   /* invalid input, including time coverage */
    PROMETHEIA_ERROR_NOT_FOUND = 5,  /* body or name not answered */
    PROMETHEIA_ERROR_INTERNAL = 6    /* allocation failure or internal fault */
} prometheia_status;

#define PROMETHEIA_ERROR_MESSAGE_SIZE 256

typedef struct prometheia_error {
    int code;                                    /* a prometheia_status */
    char message[PROMETHEIA_ERROR_MESSAGE_SIZE]; /* NUL-terminated; empty on success */
} prometheia_error;

/* ---- Library ------------------------------------------------------------ */

PROMETHEIA_API const char* prometheia_version(void); /* "0.1.0" */
PROMETHEIA_API int prometheia_abi_version(void);     /* PROMETHEIA_ABI_VERSION */

/* ---- Bodies (NAIF IDs; catalog bodies use their SPK-ID) ----------------- */

#define PROMETHEIA_SOLAR_SYSTEM_BARY 0
#define PROMETHEIA_EARTH_MOON_BARY 3
#define PROMETHEIA_SUN 10
#define PROMETHEIA_MERCURY 199
#define PROMETHEIA_VENUS 299
#define PROMETHEIA_EARTH 399
#define PROMETHEIA_MOON 301
#define PROMETHEIA_MARS 4 /* system barycentres from here on */
#define PROMETHEIA_JUPITER 5
#define PROMETHEIA_SATURN 6
#define PROMETHEIA_URANUS 7
#define PROMETHEIA_NEPTUNE 8
#define PROMETHEIA_PLUTO 9

/* ---- Options ------------------------------------------------------------ */

#define PROMETHEIA_CENTER_GEOCENTRIC 0
#define PROMETHEIA_CENTER_TOPOCENTRIC 1
#define PROMETHEIA_CENTER_HELIOCENTRIC 2
#define PROMETHEIA_CENTER_BARYCENTRIC 3
#define PROMETHEIA_CENTER_BODY 4 /* the body prometheia_options.center_body */

#define PROMETHEIA_FRAME_ICRF 0
#define PROMETHEIA_FRAME_J2000 1
#define PROMETHEIA_FRAME_MEAN_OF_DATE 2
#define PROMETHEIA_FRAME_TRUE_OF_DATE 3

#define PROMETHEIA_COORDS_ECLIPTIC 0
#define PROMETHEIA_COORDS_EQUATORIAL 1

#define PROMETHEIA_SIDEREAL_TROPICAL (-1)
#define PROMETHEIA_SIDEREAL_FAGAN_BRADLEY 0
#define PROMETHEIA_SIDEREAL_LAHIRI 1
#define PROMETHEIA_SIDEREAL_USER 255

#define PROMETHEIA_PRECESSION_IAU2006 0
#define PROMETHEIA_PRECESSION_VONDRAK2011 1

/*
 * Start from prometheia_options_init(), not a zeroed struct: zero is not
 * the default for every field (frame 0 is ICRF, sidereal 0 is
 * Fagan/Bradley).
 */
typedef struct prometheia_options {
    int center;     /* PROMETHEIA_CENTER_*, default geocentric */
    int frame;      /* PROMETHEIA_FRAME_*, default true of date */
    int coords;     /* PROMETHEIA_COORDS_*, default ecliptic */
    int sidereal;   /* PROMETHEIA_SIDEREAL_*, default tropical */
    int precession; /* PROMETHEIA_PRECESSION_*, default IAU 2006 */
    /* PROMETHEIA_SIDEREAL_USER anchor: the MEAN ayanamsha (degrees) at a
     * TT Julian date. */
    double sidereal_epoch_jd;
    double sidereal_ayanamsa_deg;
    int light_time; /* nonzero: retarded position of the body (default 1) */
    int deflection; /* nonzero: Sun's gravitational light bending (default 1) */
    int aberration; /* nonzero: observer-velocity aberration (default 1) */
    int speed;      /* nonzero: daily rates, 3x the work (default 1) */
    int sigma;      /* nonzero: catalog bodies' sigma_arcsec, 12 extra integrations (default 1) */
    /* PROMETHEIA_CENTER_TOPOCENTRIC site on the WGS84 ellipsoid. */
    double site_lon_deg; /* geodetic longitude, east positive */
    double site_lat_deg; /* geodetic latitude */
    double site_height_m;
    /* PROMETHEIA_CENTER_BODY: the observing body's NAIF ID / SPK-ID
     * (default the Sun). ABI version 4. */
    int center_body;
} prometheia_options;

/* Apparent place, geocentric, true ecliptic and equinox of date, rates on. */
PROMETHEIA_API void prometheia_options_init(prometheia_options* opts);

/* ---- Results -------------------------------------------------------------- */

#define PROMETHEIA_HAS_SIGMA 1    /* sigma_arcsec is meaningful */
#define PROMETHEIA_HAS_AYANAMSA 2 /* ayanamsa_deg is meaningful */

typedef struct prometheia_result {
    /* Ecliptic: longitude [0, 360), latitude; equatorial: right ascension
     * [0, 360), declination. Degrees; distance in AU; rates per day (zero
     * when speed is off). */
    double lon_deg;
    double lat_deg;
    double dist_au;
    double lon_speed;
    double lat_speed;
    double dist_speed;
    /* The same vector in rectangular form, in the output frame. */
    double xyz_au[3];
    double vel_au_day[3];
    double light_time_days; /* tau applied (0 without light time) */
    /* 1-sigma direction uncertainty on the sky, arcsec (catalog bodies
     * whose record carries a full covariance; see engine.hpp). */
    double sigma_arcsec;
    /* The longitude shift applied for a sidereal request, degrees. */
    double ayanamsa_deg;
    int flags; /* PROMETHEIA_HAS_* bits */
    int denum; /* DE number of the planetary ephemeris when known, else 0 */
    /* Where the answer came from, e.g. "JPL DE440 binary". NUL-terminated;
     * valid until the engine is closed or a catalog is added. */
    const char* source;
} prometheia_result;

/* ---- Engine ------------------------------------------------------------- */

typedef struct prometheia_engine prometheia_engine;

/*
 * Opens a planetary ephemeris: a JPL DE binary or a NAIF SPK kernel,
 * detected by content. On success *out receives the handle; on failure
 * *out is set to NULL.
 */
PROMETHEIA_API prometheia_status prometheia_engine_open(const char* ephemeris_path,
                                                        prometheia_engine** out,
                                                        prometheia_error* err);

/* Frees the engine. NULL is a no-op. */
PROMETHEIA_API void prometheia_engine_close(prometheia_engine* engine);

/* Human-readable description of the loaded ephemeris ("" for NULL). Valid
 * while the engine lives. */
PROMETHEIA_API const char* prometheia_engine_source(const prometheia_engine* engine);

/*
 * Adds a small-body catalog (an EPM1 container). Newest catalog wins for
 * bodies and names several catalogs carry. Invalidates the source
 * strings of earlier results.
 */
PROMETHEIA_API prometheia_status prometheia_engine_add_catalog(prometheia_engine* engine,
                                                               const char* catalog_path,
                                                               prometheia_error* err);

/*
 * Adds an asteroid perturber kernel (e.g. JPL's sb441-n16.bsp): its
 * numbered asteroids with known masses perturb every catalog body (never
 * itself). Invalidates cached small-body trajectories.
 */
PROMETHEIA_API prometheia_status prometheia_engine_add_perturbers(prometheia_engine* engine,
                                                                  const char* spk_path,
                                                                  prometheia_error* err);

/* Resolves a designation or proper name ("1", "Ceres"; ASCII
 * case-insensitive) to the SPK-ID calc takes. */
PROMETHEIA_API prometheia_status prometheia_engine_lookup(const prometheia_engine* engine,
                                                          const char* name, int* body,
                                                          prometheia_error* err);

/*
 * Position of body at a TT Julian date (calc) or a UT1 Julian date
 * (calc_ut, converted with the engine's Delta T). opts NULL means the
 * defaults of prometheia_options_init(). On failure *result is zeroed.
 */
PROMETHEIA_API prometheia_status prometheia_calc(prometheia_engine* engine, int body, double jd_tt,
                                                 const prometheia_options* opts,
                                                 prometheia_result* result, prometheia_error* err);
PROMETHEIA_API prometheia_status prometheia_calc_ut(prometheia_engine* engine, int body,
                                                    double jd_ut1, const prometheia_options* opts,
                                                    prometheia_result* result,
                                                    prometheia_error* err);

/*
 * A node or apsis of a body's orbit as a point in space, seen with the
 * options' observer and frame (docs/ENGINE.md, "Nodes and apsides"): the
 * heliocentric orbit (geocentric for the Moon) on the output frame's
 * ecliptic. The options' corrections apply exactly as to a body -- a node
 * exists to be compared with apparent positions (docs/ORBIT-POINTS.md) --
 * and all three off give the geometric point. Rates by central
 * differences. PROMETHEIA_ERROR_ARGUMENT where the point is undefined.
 */
#define PROMETHEIA_ORBIT_ASCENDING_NODE 0
#define PROMETHEIA_ORBIT_DESCENDING_NODE 1
#define PROMETHEIA_ORBIT_PERIHELION 2
#define PROMETHEIA_ORBIT_APHELION 3

#define PROMETHEIA_ELEMENTS_MEAN 0
#define PROMETHEIA_ELEMENTS_OSCULATING 1

PROMETHEIA_API prometheia_status prometheia_calc_orbit_point(prometheia_engine* engine, int body,
                                                             int point, int elements, double jd_tt,
                                                             const prometheia_options* opts,
                                                             prometheia_result* result,
                                                             prometheia_error* err);
PROMETHEIA_API prometheia_status prometheia_calc_orbit_point_ut(
    prometheia_engine* engine, int body, int point, int elements, double jd_ut1,
    const prometheia_options* opts, prometheia_result* result, prometheia_error* err);

/* ---- Bodies from orbital elements (ABI version 5) --------------------------
 *
 * A body defined by orbital elements rather than an ephemeris: a hypothetical
 * planet, a predicted one, a fictitious moon (docs/HYPOTHETICALS.md). Each
 * element is a polynomial in T = (t_TT - epoch) / 36525 Julian centuries;
 * the motion is pure two-body Keplerian about its origin. This is ephemeris
 * protocol v4's kind 4, and the selector values are its registries'.
 *
 * The structs below are frozen for the life of an ABI version: a layout
 * change bumps PROMETHEIA_ABI_VERSION, which consumers compare against
 * prometheia_abi_version() at load. Every element is its own polynomial, so
 * a zero coefficient contributes nothing, and the mean-anomaly rule turns on
 * whether M's coefficients beyond the constant are nonzero. Padding a
 * shorter element with zeros to the shared n_terms therefore never changes
 * the body.
 */
#define PROMETHEIA_EQUINOX_J2000 0    /* mean ecliptic and equinox of J2000.0 */
#define PROMETHEIA_EQUINOX_B1950 1    /* ... of B1950.0, dynamically (not FK4) */
#define PROMETHEIA_EQUINOX_J1900 2    /* ... of J1900.0 */
#define PROMETHEIA_EQUINOX_OF_DATE 3  /* ... of the instant evaluated */
#define PROMETHEIA_EQUINOX_EXPLICIT 4 /* ... of equinox_jd_tt */

#define PROMETHEIA_ELEMENTS_ORIGIN_SUN 0
#define PROMETHEIA_ELEMENTS_ORIGIN_EARTH 1

typedef struct prometheia_elements {
    double epoch_jd_tt;
    int equinox;          /* PROMETHEIA_EQUINOX_* */
    double equinox_jd_tt; /* PROMETHEIA_EQUINOX_EXPLICIT: that date; otherwise 0 */
    int origin;           /* PROMETHEIA_ELEMENTS_ORIGIN_* */
    int n_terms;          /* 1..5: the terms each polynomial uses */
    /* Coefficients of T^0 .. T^(n_terms-1): mean anomaly (deg), semi-major
     * axis (AU), eccentricity, argument of perihelion (deg), ascending node
     * (deg), inclination (deg). Unused terms are ignored; zero-padding is
     * meaningless (the mean-anomaly rule reads M's nonzero coefficients). */
    double mean_anomaly[5];
    double semi_major_axis[5];
    double eccentricity[5];
    double arg_perihelion[5];
    double ascending_node[5];
    double inclination[5];
} prometheia_elements;

/*
 * Position of the body the elements define, at a TT (or, _ut, a UT1)
 * Julian date. The options' corrections apply as to any body, light time
 * solved through the same two-body motion. PROMETHEIA_ERROR_ARGUMENT for
 * elements out of range or not a bound orbit at the instant. result->source
 * is "two-body orbital elements".
 *
 * The TT form is a function of the elements alone: two implementations given
 * the same elements must agree, and it is the form to compare across servers.
 * The UT1 form also depends on the engine's Delta T (the observed model, or
 * prometheia_engine_set_delta_t's), so it is not.
 */
PROMETHEIA_API prometheia_status prometheia_calc_elements(
    prometheia_engine* engine, const prometheia_elements* elements, double jd_tt,
    const prometheia_options* opts, prometheia_result* result, prometheia_error* err);
PROMETHEIA_API prometheia_status prometheia_calc_elements_ut(
    prometheia_engine* engine, const prometheia_elements* elements, double jd_ut1,
    const prometheia_options* opts, prometheia_result* result, prometheia_error* err);

/* ---- Named hypothetical bodies (ABI version 5) ------------------------------
 *
 * Bodies named by a token (the protocol's A.15: "cupido", "hades", ...),
 * defined by element files in JSON Lines (docs/HYPOTHETICALS.md, "Element
 * files"). The engine opens with the set the library ships; each file added
 * redefines the tokens it carries. Tokens match ASCII case-insensitively.
 * Strings returned below, and the indexes prometheia_hypothetical_token
 * takes, are valid until the engine is closed or another element file is
 * added. Not thread-safe, like the rest of an engine.
 */
PROMETHEIA_API prometheia_status prometheia_engine_add_hypotheticals(prometheia_engine* engine,
                                                                     const char* element_file_path,
                                                                     prometheia_error* err);

/* The number of tokens defined, and the index-th (in the order first
 * defined; NULL out of range). */
PROMETHEIA_API int prometheia_hypothetical_count(const prometheia_engine* engine);
PROMETHEIA_API const char* prometheia_hypothetical_token(const prometheia_engine* engine,
                                                         int index);

typedef struct prometheia_hypothetical {
    const char* token;    /* lowercase */
    const char* name;     /* display name; "" when the file gives none */
    const char* set;      /* the element set's name; results carry it as source */
    const char* citation; /* where the numbers come from */
    prometheia_elements elements;
} prometheia_hypothetical;

/* A token's current definition. PROMETHEIA_ERROR_NOT_FOUND when undefined. */
PROMETHEIA_API prometheia_status prometheia_hypothetical_get(const prometheia_engine* engine,
                                                             const char* token,
                                                             prometheia_hypothetical* out,
                                                             prometheia_error* err);

/* Position of a named body, computed from its elements as
 * prometheia_calc_elements does; result->source is its element set's name.
 * PROMETHEIA_ERROR_NOT_FOUND for an undefined token. */
PROMETHEIA_API prometheia_status prometheia_calc_hypothetical(prometheia_engine* engine,
                                                              const char* token, double jd_tt,
                                                              const prometheia_options* opts,
                                                              prometheia_result* result,
                                                              prometheia_error* err);
PROMETHEIA_API prometheia_status prometheia_calc_hypothetical_ut(prometheia_engine* engine,
                                                                 const char* token, double jd_ut1,
                                                                 const prometheia_options* opts,
                                                                 prometheia_result* result,
                                                                 prometheia_error* err);

/* ---- Fixed stars ------------------------------------------------------------
 *
 * The compiled-in catalog of naked-eye stars and Messier objects
 * (docs/STARS.md). Objects are addressed by index, 0 .. prometheia_star_count()-1,
 * stable within a library release. Strings are UTF-8 and NUL-terminated.
 */

#define PROMETHEIA_STAR_KIND_STAR 0
#define PROMETHEIA_STAR_KIND_GALAXY 1
#define PROMETHEIA_STAR_KIND_GLOBULAR_CLUSTER 2
#define PROMETHEIA_STAR_KIND_OPEN_CLUSTER 3
#define PROMETHEIA_STAR_KIND_NEBULA 4
#define PROMETHEIA_STAR_KIND_PLANETARY_NEBULA 5
#define PROMETHEIA_STAR_KIND_SUPERNOVA_REMNANT 6
#define PROMETHEIA_STAR_KIND_ASTERISM 7
#define PROMETHEIA_STAR_KIND_DOUBLE_STAR 8

#define PROMETHEIA_ASTROMETRY_HIPPARCOS 0
#define PROMETHEIA_ASTROMETRY_BRIGHT_STAR 1
#define PROMETHEIA_ASTROMETRY_SIMBAD 2

#define PROMETHEIA_MATCH_EXACT 0
#define PROMETHEIA_MATCH_ALIAS 1 /* e.g. "Beta Sco" for beta1 Sco */
#define PROMETHEIA_MATCH_PREFIX 2

typedef struct prometheia_star {
    int index;
    int kind;                            /* PROMETHEIA_STAR_KIND_* */
    int astrometry;                      /* PROMETHEIA_ASTROMETRY_* */
    int hr, hd, hip, flamsteed, messier; /* 0 where none */
    int bayer;                           /* Greek letter 1 (alpha) .. 24 (omega), 0 none */
    int bayer_index;                     /* superscript, 0 none */
    char constellation[4];               /* IAU abbreviation, "Sco" */
    char name[64];                       /* display name: IAU name, else a designation */
    char names[512];            /* every name, IAU first, '|'-separated (truncated to fit) */
    char bayer_designation[24]; /* "β¹ Sco", empty without one */
    char spectral_type[24];
    double vmag; /* NaN where unknown */
    /* ICRS RA/Dec (deg) at epoch_jyear (Julian year TDB); proper motion in
     * RA*cos(Dec) and Dec (mas/yr); parallax (mas, <= 0 unknown); radial
     * velocity (km/s); deep-sky size (arcmin). */
    double ra_deg, dec_deg, epoch_jyear;
    double pm_ra_mas_yr, pm_dec_mas_yr, parallax_mas, rv_km_s, size_arcmin;
} prometheia_star;

typedef struct prometheia_star_match {
    int index;
    int quality;      /* PROMETHEIA_MATCH_* */
    char matched[64]; /* the name or designation that matched */
} prometheia_star_match;

PROMETHEIA_API int prometheia_star_count(void);

/* The one object a name or designation means ("Graffias", "β¹ Sco",
 * "Beta Scorpii", "HR 5984", "HIP 78820", "M 45"). NOT_FOUND when nothing
 * matches; ARGUMENT when several different objects match equally well. */
PROMETHEIA_API prometheia_status prometheia_star_find(const char* query, int* index,
                                                      prometheia_error* err);

/* Every object answering to query, best first, into matches (up to max);
 * with prefix nonzero, names starting with the query too. Returns the number
 * written (0 on no match or bad arguments). */
PROMETHEIA_API int prometheia_star_lookup(const char* query, int prefix,
                                          prometheia_star_match* matches, int max);

PROMETHEIA_API prometheia_status prometheia_star_info(int index, prometheia_star* star,
                                                      prometheia_error* err);

/* Apparent (or, per options, astrometric) place of a catalog object, as for
 * prometheia_calc. PROMETHEIA_HAS_SIGMA is never set. */
PROMETHEIA_API prometheia_status prometheia_calc_star(prometheia_engine* engine, int index,
                                                      double jd_tt, const prometheia_options* opts,
                                                      prometheia_result* result,
                                                      prometheia_error* err);
PROMETHEIA_API prometheia_status prometheia_calc_star_ut(prometheia_engine* engine, int index,
                                                         double jd_ut1,
                                                         const prometheia_options* opts,
                                                         prometheia_result* result,
                                                         prometheia_error* err);

/* The IAU constellation abbreviation containing an ICRS direction (degrees). */
PROMETHEIA_API const char* prometheia_constellation_at(double ra_deg, double dec_deg);

/*
 * Delta T = TT - UT1 in seconds as a function of JD(TT), for calc_ut and
 * topocentric Earth rotation. user is passed through untouched and must
 * outlive its use. fn NULL restores the default (observed USNO Delta T).
 */
typedef double (*prometheia_delta_t_fn)(void* user, double jd_tt);
PROMETHEIA_API void prometheia_engine_set_delta_t(prometheia_engine* engine,
                                                  prometheia_delta_t_fn fn, void* user);

/* ---- Time scales (docs/TIME.md) -------------------------------------------- */

/* JD of a proleptic Gregorian date and wall-clock time, in whatever
 * continuous scale the caller means. */
PROMETHEIA_API double prometheia_jd_from_ymdhms(int year, int month, int day, int hour, int minute,
                                                double second);

/* Inverse calendar conversion: day is 1-based and fractional. */
PROMETHEIA_API void prometheia_civil_from_jd(double jd, int* year, int* month, double* day);

/* A UTC instant (1972 onward; second may be 60.x in a leap second) to
 * JD(TT). */
PROMETHEIA_API prometheia_status prometheia_utc_to_tt(int year, int month, int day, int hour,
                                                      int minute, double second, double* jd_tt,
                                                      prometheia_error* err);

typedef struct prometheia_utc {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    double second;        /* 60.x during an inserted leap second */
    double tai_minus_utc; /* seconds in effect at this instant */
} prometheia_utc;

PROMETHEIA_API prometheia_status prometheia_tt_to_utc(double jd_tt, prometheia_utc* utc,
                                                      prometheia_error* err);

/* The default Delta T model (observed USNO values), seconds. */
PROMETHEIA_API double prometheia_delta_t(double jd_tt);

/* TDB - TT in seconds (geocentric series). */
PROMETHEIA_API double prometheia_tdb_minus_tt(double jd_tt);

#ifdef __cplusplus
}
#endif

#endif /* PROMETHEIA_H */

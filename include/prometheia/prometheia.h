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

#define PROMETHEIA_ABI_VERSION 2

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

/*
 * Start from prometheia_options_init(), not a zeroed struct: zero is not
 * the default for every field (frame 0 is ICRF, sidereal 0 is
 * Fagan/Bradley).
 */
typedef struct prometheia_options {
    int center;   /* PROMETHEIA_CENTER_*, default geocentric */
    int frame;    /* PROMETHEIA_FRAME_*, default true of date */
    int coords;   /* PROMETHEIA_COORDS_*, default ecliptic */
    int sidereal; /* PROMETHEIA_SIDEREAL_*, default tropical */
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

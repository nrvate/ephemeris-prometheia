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

#define PROMETHEIA_ABI_VERSION 6

/* ---- Status and errors ------------------------------------------------ */

typedef enum prometheia_status {
    PROMETHEIA_OK = 0,
    PROMETHEIA_ERROR_IO = 1,         /* a file could not be opened or read */
    PROMETHEIA_ERROR_FORMAT = 2,     /* not a recognised or valid file */
    PROMETHEIA_ERROR_CORRUPTION = 3, /* checksum mismatch or truncation */
    PROMETHEIA_ERROR_ARGUMENT = 4,   /* invalid input */
    PROMETHEIA_ERROR_NOT_FOUND = 5,  /* body or name not answered */
    PROMETHEIA_ERROR_INTERNAL = 6,   /* allocation failure or internal fault */
    PROMETHEIA_ERROR_COVERAGE = 7    /* an instant outside the loaded data's time
                                        span (after 0.7.0; ARGUMENT before) */
} prometheia_status;

#define PROMETHEIA_ERROR_MESSAGE_SIZE 256

typedef struct prometheia_error {
    int code;                                    /* a prometheia_status */
    char message[PROMETHEIA_ERROR_MESSAGE_SIZE]; /* NUL-terminated; empty on success */
} prometheia_error;

/* ---- Library ------------------------------------------------------------ */

PROMETHEIA_API const char* prometheia_version(void); /* "0.7.3" */
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
/* Zodiacs defined at the instant (library 0.4): the anchor's true position
 * held at a fixed longitude on the true ecliptic of date. Numbered as the
 * ephemeris protocol's A.11 tokens. They have no anchor epoch, so
 * PROMETHEIA_SIDEREAL_PLANE_ANCHOR is refused for them (docs/FRAMES.md,
 * "Zodiacs defined at the instant"). */
#define PROMETHEIA_SIDEREAL_GALCENT_0SAG 17         /* Sgr A* at 240 deg */
#define PROMETHEIA_SIDEREAL_TRUE_CITRA 27           /* Spica at 180 deg */
#define PROMETHEIA_SIDEREAL_TRUE_REVATI 28          /* zeta Psc at 359d50' */
#define PROMETHEIA_SIDEREAL_TRUE_PUSHYA 29          /* delta Cnc at 106 deg */
#define PROMETHEIA_SIDEREAL_GALCENT_RGILBRAND 30    /* Sgr A* at the golden section */
#define PROMETHEIA_SIDEREAL_GALEQU_IAU1958 31       /* galactic node, IAU 1958 pole */
#define PROMETHEIA_SIDEREAL_GALEQU_TRUE 32          /* galactic node, modern pole */
#define PROMETHEIA_SIDEREAL_GALEQU_MULA 33          /* galactic node at 246d40' */
#define PROMETHEIA_SIDEREAL_TRUE_MULA 35            /* lambda Sco at 240 deg */
#define PROMETHEIA_SIDEREAL_GALCENT_MULA_WILHELM 36 /* Sgr A*, polar, at 246d40' */
#define PROMETHEIA_SIDEREAL_GALCENT_COCHRANE 40     /* Sgr A* at 270 deg */
#define PROMETHEIA_SIDEREAL_USER 255

/* The plane a sidereal longitude is counted along (ABI version 6): the
 * ephemeris protocol's A.8 values. The two fixed planes need ecliptic
 * coordinates, ignore `frame`, and report the zodiac's anchor value A0 as
 * ayanamsa_deg; a tropical request ignores the field. */
#define PROMETHEIA_SIDEREAL_PLANE_DATE 0       /* ecliptic of date (default) */
#define PROMETHEIA_SIDEREAL_PLANE_ANCHOR 1     /* mean ecliptic & equinox of the anchor epoch */
#define PROMETHEIA_SIDEREAL_PLANE_INVARIABLE 2 /* invariable plane of the solar system */

#define PROMETHEIA_PRECESSION_IAU2006 0
#define PROMETHEIA_PRECESSION_VONDRAK2011 1 /* the default since 0.8 (docs/FRAMES.md) */

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
    int precession; /* PROMETHEIA_PRECESSION_*, default Vondrak 2011 (IAU 2006 before 0.8) */
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
    /* PROMETHEIA_SIDEREAL_PLANE_*, with a sidereal zodiac. ABI version 6. */
    int sidereal_plane;
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
 * The planetary ephemeris files in the order they answer: how many (0 for
 * NULL), and each one's span as JD (TDB), first and last, with its
 * description. A cut DE file covers whole 32-day records, so its span can
 * run a few days past the years in its name; this is the span it answers.
 * *source, when asked for, is valid until the next call of this function or
 * prometheia_engine_add_ephemeris(). An index out of range is
 * PROMETHEIA_ERROR_ARGUMENT. Added after library 0.7.3; the ABI version is
 * unchanged, as no layout changed.
 */
PROMETHEIA_API int prometheia_engine_ephemeris_count(const prometheia_engine* engine);
PROMETHEIA_API prometheia_status prometheia_engine_ephemeris_span(const prometheia_engine* engine,
                                                                  int index, double* first_jd_tdb,
                                                                  double* last_jd_tdb,
                                                                  const char** source,
                                                                  prometheia_error* err);

/*
 * The small-body catalogs in the order they were added: how many (0 for
 * NULL); each one's record count and the path it was added by (a later
 * catalog wins for a body both carry); and the distinct bodies they answer
 * between them. With several catalogs, the first call of _bodies takes a
 * pass over their indexes and keeps the answer until a catalog is added.
 * *path, when asked for, is valid until the next call of _catalog_info or
 * prometheia_engine_add_catalog(). An index out of range is
 * PROMETHEIA_ERROR_ARGUMENT. Added after library 0.7.3; the ABI version is
 * unchanged, as no layout changed.
 */
PROMETHEIA_API int prometheia_engine_catalog_count(const prometheia_engine* engine);
PROMETHEIA_API prometheia_status prometheia_engine_catalog_info(const prometheia_engine* engine,
                                                                int index, int* bodies,
                                                                const char** path,
                                                                prometheia_error* err);
PROMETHEIA_API prometheia_status prometheia_engine_catalog_bodies(const prometheia_engine* engine,
                                                                  int* bodies,
                                                                  prometheia_error* err);

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
 * itself). Invalidates cached small-body trajectories. Without one, a
 * catalog body more than 100 years from its element epoch fails with
 * PROMETHEIA_ERROR_COVERAGE; with one, it is served as far as the kernel
 * spans (docs/VALIDATION.md, "Small bodies").
 */
PROMETHEIA_API prometheia_status prometheia_engine_add_perturbers(prometheia_engine* engine,
                                                                  const char* spk_path,
                                                                  prometheia_error* err);

/*
 * Adds a further planetary ephemeris behind the one(s) open, answering the
 * instants they do not cover: opened with DE440 and given DE441, the engine
 * answers 1550-2650 from DE440 and -13000..17000 outside it from DE441.
 * result.source names the file that answered. Library 0.8; ABI unchanged.
 */
PROMETHEIA_API prometheia_status prometheia_engine_add_ephemeris(prometheia_engine* engine,
                                                                 const char* ephemeris_path,
                                                                 prometheia_error* err);

/* Resolves a designation or proper name ("1", "Ceres"; ASCII
 * case-insensitive) to the SPK-ID calc takes. From a 1.2 catalog, a
 * numbered body's old provisional designation too ("2013 FZ27"). */
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
 * heliocentric orbit (geocentric for the Moon) on the mean ecliptic of
 * date, in every frame (the frame gives the coordinates, not which point
 * it is). The options' corrections apply exactly as to a body -- a node
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
/* The Moon's apogee and perigee only: interpolated between its actual
 * passages (the "natural" apsides, docs/ORBIT-POINTS.md). Library 0.6. */
#define PROMETHEIA_ELEMENTS_INTERPOLATED 2

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
 * prometheia_calc. PROMETHEIA_HAS_SIGMA is never set. The distance rate is
 * computed in closed form rather than differenced (docs/ENGINE.md, "Rates"). */
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

/* ---- Houses, sidereal time, obliquity, rotations (after 0.7.3) -------------
 *
 * docs/HOUSES.md. Added after library 0.7.3; the ABI version is unchanged,
 * as no existing layout changed. Angles are degrees throughout.
 */

/* House systems: ephemeris protocol v4's A.22 values. */
#define PROMETHEIA_HOUSES_PLACIDUS 0
#define PROMETHEIA_HOUSES_KOCH 1
#define PROMETHEIA_HOUSES_PORPHYRY 2
#define PROMETHEIA_HOUSES_REGIOMONTANUS 3
#define PROMETHEIA_HOUSES_CAMPANUS 4
#define PROMETHEIA_HOUSES_EQUAL 5
#define PROMETHEIA_HOUSES_WHOLE_SIGN 6
#define PROMETHEIA_HOUSES_ALCABITIUS 7
#define PROMETHEIA_HOUSES_MORINUS 8
#define PROMETHEIA_HOUSES_MERIDIAN 9
#define PROMETHEIA_HOUSES_TOPOCENTRIC 10

/* Every longitude below is ecliptic of date, true equinox, in [0, 360). */
typedef struct prometheia_houses {
    double cusp_deg[12]; /* cusps 1..12 at [0..11], each the next eastward */
    /* The ecliptic's intersection with the eastern horizon: the rising
     * point at every latitude, inside a polar circle too. */
    double asc_deg;
    /* The ecliptic's point on the upper meridian (hour angle 0), in every
     * system, even where it culminates below the horizon. */
    double mc_deg;
    double armc_deg;   /* local apparent sidereal time as an angle; never sidereal-shifted */
    double vertex_deg; /* the ecliptic's intersection with the prime vertical, in the west */
    double equatorial_asc_deg; /* the ecliptic point at right ascension ARMC + 90 */
    double obliquity_deg;      /* the true obliquity used */
    double ayanamsa_deg;       /* the shift applied, with PROMETHEIA_HAS_AYANAMSA */
    int flags;                 /* PROMETHEIA_HAS_AYANAMSA */
} prometheia_houses;

/*
 * Cusps and angles at a TT (or, _ut, a UT1) Julian date for a site at
 * geodetic longitude lon_deg (east positive) and latitude lat_deg (north
 * positive, strictly inside +-90). The sidereal time needs UT1 and the
 * obliquity TT; whichever is not given comes from the engine's Delta T,
 * the same one calc and calc_ut use (the observed model, or the function
 * installed with prometheia_engine_set_delta_t).
 *
 * From the options only the zodiac (sidereal, its user anchor) and the
 * precession model enter. A sidereal zodiac shifts every cusp and angle but
 * the ARMC, on the ecliptic of date only; Whole Sign's houses then follow
 * the sidereal Ascendant's sign. The frame must be
 * PROMETHEIA_FRAME_TRUE_OF_DATE (the default).
 *
 * The twelve cusps answered always go once round the zodiac in order.
 * PROMETHEIA_ERROR_ARGUMENT, with a message naming the system and the
 * reason, and never another system in its place:
 *   - Placidus and Koch inside a polar circle (|lat| > 90 - obliquity);
 *   - Regiomontanus and Campanus inside one, and Topocentric from about 1.6
 *     degrees short of one, at the sidereal times their cusps fall out of
 *     zodiacal order;
 *   - every system at an instant when the ecliptic lies along one of its
 *     defining circles (only on a polar circle's edge).
 * On failure *out is zeroed.
 */
PROMETHEIA_API prometheia_status prometheia_calc_houses(
    prometheia_engine* engine, int system, double jd_tt, double lon_deg, double lat_deg,
    const prometheia_options* opts, prometheia_houses* out, prometheia_error* err);
PROMETHEIA_API prometheia_status prometheia_calc_houses_ut(
    prometheia_engine* engine, int system, double jd_ut1, double lon_deg, double lat_deg,
    const prometheia_options* opts, prometheia_houses* out, prometheia_error* err);

/*
 * Tropical cusps and every angle, Vertex and equatorial Ascendant included,
 * from an ARMC, the TRUE obliquity (mean plus the nutation in obliquity, as
 * prometheia_obliquity(..., PROMETHEIA_OBLIQUITY_TRUE, ...) gives) and a
 * geodetic latitude, with no instant and no engine: an oblique Ascendant,
 * or a chart at a reference latitude. Refused as prometheia_calc_houses is.
 * Given the ARMC and obliquity prometheia_calc_houses reports, everything
 * agrees with its tropical answer to 1e-6 arcsec: the ARMC passes through
 * degrees. obliquity_deg is echoed; flags is 0.
 */
PROMETHEIA_API prometheia_status prometheia_houses_from_armc(int system, double armc_deg,
                                                             double obliquity_deg, double lat_deg,
                                                             prometheia_houses* out,
                                                             prometheia_error* err);

/*
 * Greenwich sidereal time as an angle, [0, 360), at a UT1 Julian date (TT
 * from the engine's Delta T, as for the houses): mean (GMST, IAU 2006) or apparent (GAST: plus
 * the equation of the equinoxes under the options' precession model, the
 * angle the houses' ARMC comes from). Add the east longitude for local
 * sidereal time. Only opts->precession enters; NULL means the default.
 */
#define PROMETHEIA_SIDEREAL_TIME_MEAN 0
#define PROMETHEIA_SIDEREAL_TIME_APPARENT 1
PROMETHEIA_API prometheia_status prometheia_sidereal_time_ut(prometheia_engine* engine,
                                                             double jd_ut1, int kind,
                                                             const prometheia_options* opts,
                                                             double* out_deg,
                                                             prometheia_error* err);

/*
 * The obliquity of the ecliptic at a TT Julian date: mean, under the
 * options' precession model, or true (plus the nutation in obliquity), the
 * one the houses use. Only opts->precession enters; NULL means the default.
 */
#define PROMETHEIA_OBLIQUITY_MEAN 0
#define PROMETHEIA_OBLIQUITY_TRUE 1
PROMETHEIA_API prometheia_status prometheia_obliquity(prometheia_engine* engine, double jd_tt,
                                                      int kind, const prometheia_options* opts,
                                                      double* out_deg, prometheia_error* err);

/*
 * Rotation between ecliptic and equatorial coordinates about the equinox
 * by an obliquity: a pure function, no instant. Longitude and right
 * ascension in [0, 360), latitude and declination by atan2 (never asin).
 */
PROMETHEIA_API void prometheia_ecliptic_to_equatorial(double lon_deg, double lat_deg,
                                                      double obliquity_deg, double* ra_deg,
                                                      double* dec_deg);
PROMETHEIA_API void prometheia_equatorial_to_ecliptic(double ra_deg, double dec_deg,
                                                      double obliquity_deg, double* lon_deg,
                                                      double* lat_deg);

/* ---- The horizon and refraction (after 0.7.3) -------------------------------
 *
 * docs/FRAMES.md, "The horizon". Added after library 0.7.3; the ABI version
 * is unchanged, as no existing layout changed. Degrees throughout.
 */

/* The air refraction is scaled by. prometheia_atmosphere_init gives the
 * standard 1013.25 hPa and 15 C. The dip of the sea horizon is the almanac
 * value for standard conditions, 1.76' sqrt(height_m); no lapse rate
 * enters. A dip from a lapse rate, if it comes, is a call of its own, so
 * this struct keeps its layout. */
typedef struct prometheia_atmosphere {
    double pressure_hpa;
    double temperature_c;
} prometheia_atmosphere;

PROMETHEIA_API void prometheia_atmosphere_init(prometheia_atmosphere* atmosphere);

typedef struct prometheia_horizontal {
    /* From NORTH through east, [0, 360): 90 is east. (pyswisseph's azalt
     * counts from the south, westward: 180 deg apart.) */
    double azimuth_deg;
    double altitude_deg;          /* true: no refraction */
    double apparent_altitude_deg; /* refracted; the true altitude without an atmosphere */
    /* Local apparent hour angle, positive westward, [0, 360): 350 is 10 deg
     * east of the meridian. */
    double hour_angle_deg;
} prometheia_horizontal;

/*
 * A direction on the true equator and equinox of date, ra_deg/dec_deg, in
 * the horizon of a site at a UT1 Julian date. For a body, that is its
 * topocentric apparent position from the same site: prometheia_calc_ut
 * with PROMETHEIA_CENTER_TOPOCENTRIC, PROMETHEIA_FRAME_TRUE_OF_DATE and
 * PROMETHEIA_COORDS_EQUATORIAL. No parallax is applied here; the direction
 * is taken as given, and height_m sets only the dip.
 *
 * The hour angle is from the local apparent sidereal time: GAST, as
 * prometheia_sidereal_time_ut gives it (TT from the engine's Delta T, the
 * same one calc_ut uses, including a function installed with
 * prometheia_engine_set_delta_t), plus lon_deg, east positive. The latitude
 * is geodetic; polar motion is neglected. Only opts->precession enters
 * (NULL: the default).
 *
 * With an atmosphere, the apparent altitude carries Bennett's refraction
 * (prometheia_refraction). NULL means no atmosphere: apparent = true.
 */
PROMETHEIA_API prometheia_status prometheia_horizontal_ut(
    prometheia_engine* engine, double jd_ut1, double lon_deg, double lat_deg, double height_m,
    double ra_deg, double dec_deg, const prometheia_atmosphere* atmosphere,
    const prometheia_options* opts, prometheia_horizontal* out, prometheia_error* err);

/* And back: an azimuth and altitude to right ascension [0, 360) and
 * declination of date. With apparent nonzero and an atmosphere, the
 * altitude is apparent and its refraction is removed first, as
 * prometheia_refraction(..., PROMETHEIA_REFRACTION_FROM_APPARENT, ...) does. */
PROMETHEIA_API prometheia_status prometheia_equatorial_from_horizontal_ut(
    prometheia_engine* engine, double jd_ut1, double lon_deg, double lat_deg, double height_m,
    double azimuth_deg, double altitude_deg, int apparent, const prometheia_atmosphere* atmosphere,
    const prometheia_options* opts, double* ra_deg, double* dec_deg, prometheia_error* err);

/*
 * Refraction R, degrees: Bennett 1982, cot(h + 7.31/(h + 4.4)) arcminutes
 * with his correction -0.06 sin(14.7 R + 13 deg), scaled by
 * (P/1010)(283/(273 + T)) (atmosphere NULL: the standard one).
 *
 * The cutoff c is the dip for height_m, below the astronomical horizon, or
 * -1 deg, whichever is higher. An apparent altitude at or above c is
 * refracted; below it, apparent = true and R = 0.
 *   - FROM_APPARENT, altitude h: R(h) when h >= c, else 0; true = h - R.
 *   - FROM_TRUE, altitude t: apparent = t + R, the formula inverted; R = 0
 *     when that apparent altitude would fall below c.
 * So true altitudes at or above t* = c - R(c) (about half a degree below c)
 * become apparent ones at or above c, and those below t* stay below t*: the
 * apparent altitudes in [t*, c) are never produced. From a true altitude
 * and back is exact to 1e-10 arcsec everywhere; from an apparent altitude
 * and back, everywhere outside [t*, c).
 */
#define PROMETHEIA_REFRACTION_FROM_TRUE 0
#define PROMETHEIA_REFRACTION_FROM_APPARENT 1
PROMETHEIA_API prometheia_status prometheia_refraction(double altitude_deg, int from,
                                                       const prometheia_atmosphere* atmosphere,
                                                       double height_m, double* out_deg,
                                                       prometheia_error* err);

/* ---- Rise, set and transit (after 0.7.3) ------------------------------------
 *
 * docs/ENGINE.md, "Rise, set and transit". Added after library 0.7.3; the ABI
 * version is unchanged, as no existing layout changed.
 */

/* What to find events of. */
#define PROMETHEIA_OBJECT_BODY 0         /* id: a NAIF id, or a catalog SPK-ID */
#define PROMETHEIA_OBJECT_STAR 1         /* id: a star catalog index */
#define PROMETHEIA_OBJECT_HYPOTHETICAL 2 /* token: a hypothetical body's */
typedef struct prometheia_object {
    int kind;          /* PROMETHEIA_OBJECT_* */
    int id;            /* BODY, STAR */
    const char* token; /* HYPOTHETICAL */
} prometheia_object;

#define PROMETHEIA_EVENT_RISE 0
#define PROMETHEIA_EVENT_SET 1
#define PROMETHEIA_EVENT_UPPER_TRANSIT 2
#define PROMETHEIA_EVENT_LOWER_TRANSIT 3

#define PROMETHEIA_LIMB_UPPER 1
#define PROMETHEIA_LIMB_CENTRE 0
#define PROMETHEIA_LIMB_LOWER (-1)

/* prometheia_rise_set_options_init gives the defaults below. */
typedef struct prometheia_rise_set_options {
    int limb;                         /* PROMETHEIA_LIMB_*, default UPPER */
    int refraction;                   /* nonzero: Bennett, with `atmosphere` (default 1) */
    prometheia_atmosphere atmosphere; /* default 1013.25 hPa, 15 C */
    /* The eye's height above a sea horizon, for its dip (1.76' sqrt(h)).
     * Default 0: the mathematical horizon, whatever the site's height. */
    double dip_height_m;
    double window_days; /* how far to look, > 0 and <= 3660; default 2 */
    int backward;       /* nonzero: the last event before the start */
} prometheia_rise_set_options;

PROMETHEIA_API void prometheia_rise_set_options_init(prometheia_rise_set_options* options);

#define PROMETHEIA_OUTCOME_FOUND 0
#define PROMETHEIA_OUTCOME_ALWAYS_ABOVE 1  /* the limb stayed above the horizon */
#define PROMETHEIA_OUTCOME_ALWAYS_BELOW 2  /* ... below it */
#define PROMETHEIA_OUTCOME_NOT_IN_WINDOW 3 /* both sides, but not this event */

typedef struct prometheia_event {
    double jd_ut1; /* the event, UT1; NaN unless FOUND */
    int outcome;   /* PROMETHEIA_OUTCOME_* */
} prometheia_event;

/*
 * The first event strictly after jd_ut1_start (the last before it, with
 * backward) within the window, at a site: geodetic lon_deg (east positive)
 * and lat_deg, and height_m above the ellipsoid, which gives the parallax
 * and no dip. A window with no such event is still PROMETHEIA_OK, and the
 * outcome says why.
 *
 * The position is the topocentric apparent one from the site, the Moon's
 * parallax included, with TT from the engine's Delta T (the observed model,
 * or a function installed with prometheia_engine_set_delta_t). Only
 * opts->precession enters (NULL: the default).
 *   - Rise and set: the limb's true altitude is -dip - R(-dip), so the
 *     apparent limb is on the observer's horizon; without refraction, -dip.
 *   - The limb is the centre plus or minus the semi-diameter asin(radius /
 *     distance), from IAU radii for the Sun, the Moon and the planets; a
 *     star, catalog body or hypothetical has none.
 *   - Transits are where the local apparent hour angle is 0 (upper) or
 *     180 deg (lower).
 *   - Transits are found first and none is missed. A rise or set is then
 *     sought between the altitude extrema next to them, so even a Sun up for
 *     a few seconds is found.
 */
PROMETHEIA_API prometheia_status prometheia_rise_set_ut(
    prometheia_engine* engine, const prometheia_object* object, int event, double jd_ut1_start,
    double lon_deg, double lat_deg, double height_m, const prometheia_rise_set_options* options,
    const prometheia_options* opts, prometheia_event* out, prometheia_error* err);

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

/* TDB - TT in seconds at the geocentre: the full Fairhead & Bretagnon
   series, 787 terms (docs/TIME.md). */
PROMETHEIA_API double prometheia_tdb_minus_tt(double jd_tt);

#ifdef __cplusplus
}
#endif

#endif /* PROMETHEIA_H */

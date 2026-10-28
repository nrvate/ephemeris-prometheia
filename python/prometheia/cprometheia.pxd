# SPDX-License-Identifier: GPL-2.0-or-later
#
# The C interface (include/prometheia/prometheia.h) for Cython: cimport it as
#
#     from prometheia cimport cprometheia as c
#
# and call it with or without the GIL. Every function is plain C: it takes no
# Python object, calls back into nothing but a prometheia_delta_t_fn the
# caller installs, never raises, and allocates only inside engine_open,
# add_* and the first lookup that builds an index (docs/C_API.md). So the
# whole block is nogil and noexcept. One engine per thread; engines share
# nothing. Layouts are those of PROMETHEIA_ABI_VERSION, which a binding
# compares with prometheia_abi_version() at load (docs/C_API.md, "ABI
# stability"); this file is kept in step with the header by
# python/tests/test_native.py.

cdef extern from "prometheia/prometheia.h" nogil:
    int PROMETHEIA_ABI_VERSION

    ctypedef enum prometheia_status:
        PROMETHEIA_OK
        PROMETHEIA_ERROR_IO
        PROMETHEIA_ERROR_FORMAT
        PROMETHEIA_ERROR_CORRUPTION
        PROMETHEIA_ERROR_ARGUMENT
        PROMETHEIA_ERROR_NOT_FOUND
        PROMETHEIA_ERROR_INTERNAL
        PROMETHEIA_ERROR_COVERAGE

    int PROMETHEIA_ERROR_MESSAGE_SIZE

    ctypedef struct prometheia_error:
        int code
        char message[256]

    const char* prometheia_version() noexcept
    int prometheia_abi_version() noexcept

    # Bodies
    int PROMETHEIA_SOLAR_SYSTEM_BARY
    int PROMETHEIA_EARTH_MOON_BARY
    int PROMETHEIA_SUN
    int PROMETHEIA_MERCURY
    int PROMETHEIA_VENUS
    int PROMETHEIA_EARTH
    int PROMETHEIA_MOON
    int PROMETHEIA_MARS
    int PROMETHEIA_JUPITER
    int PROMETHEIA_SATURN
    int PROMETHEIA_URANUS
    int PROMETHEIA_NEPTUNE
    int PROMETHEIA_PLUTO

    # Options
    int PROMETHEIA_CENTER_GEOCENTRIC
    int PROMETHEIA_CENTER_TOPOCENTRIC
    int PROMETHEIA_CENTER_HELIOCENTRIC
    int PROMETHEIA_CENTER_BARYCENTRIC
    int PROMETHEIA_CENTER_BODY
    int PROMETHEIA_FRAME_ICRF
    int PROMETHEIA_FRAME_J2000
    int PROMETHEIA_FRAME_MEAN_OF_DATE
    int PROMETHEIA_FRAME_TRUE_OF_DATE
    int PROMETHEIA_COORDS_ECLIPTIC
    int PROMETHEIA_COORDS_EQUATORIAL
    int PROMETHEIA_SIDEREAL_TROPICAL
    int PROMETHEIA_SIDEREAL_FAGAN_BRADLEY
    int PROMETHEIA_SIDEREAL_LAHIRI
    int PROMETHEIA_SIDEREAL_GALCENT_0SAG
    int PROMETHEIA_SIDEREAL_TRUE_CITRA
    int PROMETHEIA_SIDEREAL_TRUE_REVATI
    int PROMETHEIA_SIDEREAL_TRUE_PUSHYA
    int PROMETHEIA_SIDEREAL_GALCENT_RGILBRAND
    int PROMETHEIA_SIDEREAL_GALEQU_IAU1958
    int PROMETHEIA_SIDEREAL_GALEQU_TRUE
    int PROMETHEIA_SIDEREAL_GALEQU_MULA
    int PROMETHEIA_SIDEREAL_TRUE_MULA
    int PROMETHEIA_SIDEREAL_GALCENT_MULA_WILHELM
    int PROMETHEIA_SIDEREAL_GALCENT_COCHRANE
    int PROMETHEIA_SIDEREAL_USER
    int PROMETHEIA_SIDEREAL_PLANE_DATE
    int PROMETHEIA_SIDEREAL_PLANE_ANCHOR
    int PROMETHEIA_SIDEREAL_PLANE_INVARIABLE
    int PROMETHEIA_PRECESSION_IAU2006
    int PROMETHEIA_PRECESSION_VONDRAK2011

    ctypedef struct prometheia_options:
        int center
        int frame
        int coords
        int sidereal
        int precession
        double sidereal_epoch_jd
        double sidereal_ayanamsa_deg
        int light_time
        int deflection
        int aberration
        int speed
        int sigma
        double site_lon_deg
        double site_lat_deg
        double site_height_m
        int center_body
        int sidereal_plane

    void prometheia_options_init(prometheia_options* opts) noexcept

    # Results
    int PROMETHEIA_HAS_SIGMA
    int PROMETHEIA_HAS_AYANAMSA

    ctypedef struct prometheia_result:
        double lon_deg
        double lat_deg
        double dist_au
        double lon_speed
        double lat_speed
        double dist_speed
        double xyz_au[3]
        double vel_au_day[3]
        double light_time_days
        double sigma_arcsec
        double ayanamsa_deg
        int flags
        int denum
        const char* source

    # Engine
    ctypedef struct prometheia_engine:
        pass

    prometheia_status prometheia_engine_open(const char* ephemeris_path,
                                             prometheia_engine** out,
                                             prometheia_error* err) noexcept
    void prometheia_engine_close(prometheia_engine* engine) noexcept
    const char* prometheia_engine_source(const prometheia_engine* engine) noexcept
    int prometheia_engine_ephemeris_count(const prometheia_engine* engine) noexcept
    prometheia_status prometheia_engine_ephemeris_span(const prometheia_engine* engine, int index,
                                                       double* first_jd_tdb,
                                                       double* last_jd_tdb,
                                                       const char** source,
                                                       prometheia_error* err) noexcept
    int prometheia_engine_catalog_count(const prometheia_engine* engine) noexcept
    prometheia_status prometheia_engine_catalog_info(const prometheia_engine* engine, int index,
                                                     int* bodies, const char** path,
                                                     prometheia_error* err) noexcept
    prometheia_status prometheia_engine_catalog_bodies(const prometheia_engine* engine,
                                                       int* bodies,
                                                       prometheia_error* err) noexcept
    prometheia_status prometheia_engine_add_catalog(prometheia_engine* engine,
                                                    const char* catalog_path,
                                                    prometheia_error* err) noexcept
    prometheia_status prometheia_engine_add_perturbers(prometheia_engine* engine,
                                                       const char* spk_path,
                                                       prometheia_error* err) noexcept
    prometheia_status prometheia_engine_add_ephemeris(prometheia_engine* engine,
                                                      const char* ephemeris_path,
                                                      prometheia_error* err) noexcept
    prometheia_status prometheia_engine_lookup(const prometheia_engine* engine, const char* name,
                                               int* body, prometheia_error* err) noexcept

    prometheia_status prometheia_calc(prometheia_engine* engine, int body, double jd_tt,
                                      const prometheia_options* opts, prometheia_result* result,
                                      prometheia_error* err) noexcept
    prometheia_status prometheia_calc_ut(prometheia_engine* engine, int body, double jd_ut1,
                                         const prometheia_options* opts,
                                         prometheia_result* result,
                                         prometheia_error* err) noexcept

    # Orbit points
    int PROMETHEIA_ORBIT_ASCENDING_NODE
    int PROMETHEIA_ORBIT_DESCENDING_NODE
    int PROMETHEIA_ORBIT_PERIHELION
    int PROMETHEIA_ORBIT_APHELION
    int PROMETHEIA_ELEMENTS_MEAN
    int PROMETHEIA_ELEMENTS_OSCULATING
    int PROMETHEIA_ELEMENTS_INTERPOLATED

    prometheia_status prometheia_calc_orbit_point(prometheia_engine* engine, int body, int point,
                                                  int elements, double jd_tt,
                                                  const prometheia_options* opts,
                                                  prometheia_result* result,
                                                  prometheia_error* err) noexcept
    prometheia_status prometheia_calc_orbit_point_ut(prometheia_engine* engine, int body,
                                                     int point, int elements, double jd_ut1,
                                                     const prometheia_options* opts,
                                                     prometheia_result* result,
                                                     prometheia_error* err) noexcept

    # Bodies from orbital elements
    int PROMETHEIA_EQUINOX_J2000
    int PROMETHEIA_EQUINOX_B1950
    int PROMETHEIA_EQUINOX_J1900
    int PROMETHEIA_EQUINOX_OF_DATE
    int PROMETHEIA_EQUINOX_EXPLICIT
    int PROMETHEIA_ELEMENTS_ORIGIN_SUN
    int PROMETHEIA_ELEMENTS_ORIGIN_EARTH

    ctypedef struct prometheia_elements:
        double epoch_jd_tt
        int equinox
        double equinox_jd_tt
        int origin
        int n_terms
        double mean_anomaly[5]
        double semi_major_axis[5]
        double eccentricity[5]
        double arg_perihelion[5]
        double ascending_node[5]
        double inclination[5]

    prometheia_status prometheia_calc_elements(prometheia_engine* engine,
                                               const prometheia_elements* elements,
                                               double jd_tt, const prometheia_options* opts,
                                               prometheia_result* result,
                                               prometheia_error* err) noexcept
    prometheia_status prometheia_calc_elements_ut(prometheia_engine* engine,
                                                  const prometheia_elements* elements,
                                                  double jd_ut1, const prometheia_options* opts,
                                                  prometheia_result* result,
                                                  prometheia_error* err) noexcept

    # Named hypothetical bodies
    prometheia_status prometheia_engine_add_hypotheticals(prometheia_engine* engine,
                                                          const char* element_file_path,
                                                          prometheia_error* err) noexcept
    int prometheia_hypothetical_count(const prometheia_engine* engine) noexcept
    const char* prometheia_hypothetical_token(const prometheia_engine* engine,
                                              int index) noexcept

    ctypedef struct prometheia_hypothetical:
        const char* token
        const char* name
        const char* set_ "set"
        const char* citation
        prometheia_elements elements

    prometheia_status prometheia_hypothetical_get(const prometheia_engine* engine,
                                                  const char* token,
                                                  prometheia_hypothetical* out,
                                                  prometheia_error* err) noexcept
    prometheia_status prometheia_calc_hypothetical(prometheia_engine* engine, const char* token,
                                                   double jd_tt, const prometheia_options* opts,
                                                   prometheia_result* result,
                                                   prometheia_error* err) noexcept
    prometheia_status prometheia_calc_hypothetical_ut(prometheia_engine* engine,
                                                      const char* token, double jd_ut1,
                                                      const prometheia_options* opts,
                                                      prometheia_result* result,
                                                      prometheia_error* err) noexcept

    # Fixed stars
    int PROMETHEIA_STAR_KIND_STAR
    int PROMETHEIA_STAR_KIND_GALAXY
    int PROMETHEIA_STAR_KIND_GLOBULAR_CLUSTER
    int PROMETHEIA_STAR_KIND_OPEN_CLUSTER
    int PROMETHEIA_STAR_KIND_NEBULA
    int PROMETHEIA_STAR_KIND_PLANETARY_NEBULA
    int PROMETHEIA_STAR_KIND_SUPERNOVA_REMNANT
    int PROMETHEIA_STAR_KIND_ASTERISM
    int PROMETHEIA_STAR_KIND_DOUBLE_STAR
    int PROMETHEIA_ASTROMETRY_HIPPARCOS
    int PROMETHEIA_ASTROMETRY_BRIGHT_STAR
    int PROMETHEIA_ASTROMETRY_SIMBAD
    int PROMETHEIA_MATCH_EXACT
    int PROMETHEIA_MATCH_ALIAS
    int PROMETHEIA_MATCH_PREFIX

    ctypedef struct prometheia_star:
        int index
        int kind
        int astrometry
        int hr
        int hd
        int hip
        int flamsteed
        int messier
        int bayer
        int bayer_index
        char constellation[4]
        char name[64]
        char names[512]
        char bayer_designation[24]
        char spectral_type[24]
        double vmag
        double ra_deg
        double dec_deg
        double epoch_jyear
        double pm_ra_mas_yr
        double pm_dec_mas_yr
        double parallax_mas
        double rv_km_s
        double size_arcmin

    ctypedef struct prometheia_star_match:
        int index
        int quality
        char matched[64]

    int prometheia_star_count() noexcept
    prometheia_status prometheia_star_find(const char* query, int* index,
                                           prometheia_error* err) noexcept
    int prometheia_star_lookup(const char* query, int prefix, prometheia_star_match* matches,
                               int max) noexcept
    prometheia_status prometheia_star_info(int index, prometheia_star* star,
                                           prometheia_error* err) noexcept
    prometheia_status prometheia_calc_star(prometheia_engine* engine, int index, double jd_tt,
                                           const prometheia_options* opts,
                                           prometheia_result* result,
                                           prometheia_error* err) noexcept
    prometheia_status prometheia_calc_star_ut(prometheia_engine* engine, int index,
                                              double jd_ut1, const prometheia_options* opts,
                                              prometheia_result* result,
                                              prometheia_error* err) noexcept
    const char* prometheia_constellation_at(double ra_deg, double dec_deg) noexcept

    ctypedef double (*prometheia_delta_t_fn)(void* user, double jd_tt) noexcept nogil
    void prometheia_engine_set_delta_t(prometheia_engine* engine, prometheia_delta_t_fn fn,
                                       void* user) noexcept

    # Houses, sidereal time, obliquity, rotations
    int PROMETHEIA_HOUSES_PLACIDUS
    int PROMETHEIA_HOUSES_KOCH
    int PROMETHEIA_HOUSES_PORPHYRY
    int PROMETHEIA_HOUSES_REGIOMONTANUS
    int PROMETHEIA_HOUSES_CAMPANUS
    int PROMETHEIA_HOUSES_EQUAL
    int PROMETHEIA_HOUSES_WHOLE_SIGN
    int PROMETHEIA_HOUSES_ALCABITIUS
    int PROMETHEIA_HOUSES_MORINUS
    int PROMETHEIA_HOUSES_MERIDIAN
    int PROMETHEIA_HOUSES_TOPOCENTRIC

    ctypedef struct prometheia_houses:
        double cusp_deg[12]
        double asc_deg
        double mc_deg
        double armc_deg
        double vertex_deg
        double equatorial_asc_deg
        double obliquity_deg
        double ayanamsa_deg
        int flags

    prometheia_status prometheia_calc_houses(prometheia_engine* engine, int system, double jd_tt,
                                             double lon_deg, double lat_deg,
                                             const prometheia_options* opts,
                                             prometheia_houses* out,
                                             prometheia_error* err) noexcept
    prometheia_status prometheia_calc_houses_ut(prometheia_engine* engine, int system,
                                                double jd_ut1, double lon_deg, double lat_deg,
                                                const prometheia_options* opts,
                                                prometheia_houses* out,
                                                prometheia_error* err) noexcept
    prometheia_status prometheia_houses_from_armc(int system, double armc_deg,
                                                  double obliquity_deg, double lat_deg,
                                                  prometheia_houses* out,
                                                  prometheia_error* err) noexcept

    int PROMETHEIA_SIDEREAL_TIME_MEAN
    int PROMETHEIA_SIDEREAL_TIME_APPARENT
    prometheia_status prometheia_sidereal_time_ut(prometheia_engine* engine, double jd_ut1,
                                                  int kind, const prometheia_options* opts,
                                                  double* out_deg,
                                                  prometheia_error* err) noexcept
    int PROMETHEIA_OBLIQUITY_MEAN
    int PROMETHEIA_OBLIQUITY_TRUE
    prometheia_status prometheia_obliquity(prometheia_engine* engine, double jd_tt, int kind,
                                           const prometheia_options* opts, double* out_deg,
                                           prometheia_error* err) noexcept
    void prometheia_ecliptic_to_equatorial(double lon_deg, double lat_deg,
                                           double obliquity_deg, double* ra_deg,
                                           double* dec_deg) noexcept
    void prometheia_equatorial_to_ecliptic(double ra_deg, double dec_deg,
                                           double obliquity_deg, double* lon_deg,
                                           double* lat_deg) noexcept

    # The horizon and refraction
    ctypedef struct prometheia_atmosphere:
        double pressure_hpa
        double temperature_c

    void prometheia_atmosphere_init(prometheia_atmosphere* atmosphere) noexcept

    ctypedef struct prometheia_horizontal:
        double azimuth_deg
        double altitude_deg
        double apparent_altitude_deg
        double hour_angle_deg

    prometheia_status prometheia_horizontal_ut(prometheia_engine* engine, double jd_ut1,
                                               double lon_deg, double lat_deg, double height_m,
                                               double ra_deg, double dec_deg,
                                               const prometheia_atmosphere* atmosphere,
                                               const prometheia_options* opts,
                                               prometheia_horizontal* out,
                                               prometheia_error* err) noexcept
    prometheia_status prometheia_equatorial_from_horizontal_ut(
        prometheia_engine* engine, double jd_ut1, double lon_deg, double lat_deg,
        double height_m, double azimuth_deg, double altitude_deg, int apparent,
        const prometheia_atmosphere* atmosphere, const prometheia_options* opts,
        double* ra_deg, double* dec_deg, prometheia_error* err) noexcept

    int PROMETHEIA_REFRACTION_FROM_TRUE
    int PROMETHEIA_REFRACTION_FROM_APPARENT
    prometheia_status prometheia_refraction(double altitude_deg, int from_,
                                            const prometheia_atmosphere* atmosphere,
                                            double height_m, double* out_deg,
                                            prometheia_error* err) noexcept

    # Time scales
    double prometheia_jd_from_ymdhms(int year, int month, int day, int hour, int minute,
                                     double second) noexcept
    void prometheia_civil_from_jd(double jd, int* year, int* month, double* day) noexcept
    prometheia_status prometheia_utc_to_tt(int year, int month, int day, int hour, int minute,
                                           double second, double* jd_tt,
                                           prometheia_error* err) noexcept

    ctypedef struct prometheia_utc:
        int year
        int month
        int day
        int hour
        int minute
        double second
        double tai_minus_utc

    prometheia_status prometheia_tt_to_utc(double jd_tt, prometheia_utc* utc,
                                           prometheia_error* err) noexcept
    double prometheia_delta_t(double jd_tt) noexcept
    double prometheia_tdb_minus_tt(double jd_tt) noexcept

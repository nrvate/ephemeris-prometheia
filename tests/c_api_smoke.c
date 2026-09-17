/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The C interface exercised from genuine C (compiled as C99, -pedantic):
 * proves the header is C-clean and the calling pattern works without any
 * C++ in the caller. Driven by tests/test_c_api.cpp.
 */
#include <prometheia/prometheia.h>

#include <math.h>
#include <string.h>

static double fixed_delta_t(void* user, double jd_tt) {
    (void)jd_tt;
    return *(const double*)user;
}

/* Returns 0 on success, else the number of the first failed step. */
int prometheia_c_smoke(const char* kernel_path) {
    prometheia_engine* engine = NULL;
    prometheia_error err;
    prometheia_options opts;
    prometheia_result res;
    int body = -1;
    double seconds = 69.0;

    if (prometheia_abi_version() != PROMETHEIA_ABI_VERSION)
        return 1;
    if (prometheia_engine_open("/nonexistent/prometheia.bsp", &engine, &err) !=
            PROMETHEIA_ERROR_IO ||
        engine != NULL || err.code != PROMETHEIA_ERROR_IO || err.message[0] == '\0')
        return 2;
    if (prometheia_engine_open(kernel_path, &engine, &err) != PROMETHEIA_OK || engine == NULL ||
        err.code != PROMETHEIA_OK || err.message[0] != '\0')
        return 3;
    if (strstr(prometheia_engine_source(engine), "SPK") == NULL)
        return 4;

    /* Defaults, then a geometric J2000 equatorial request. */
    if (prometheia_calc(engine, PROMETHEIA_SUN, 2451545.0, NULL, &res, NULL) != PROMETHEIA_OK ||
        !(res.dist_au > 0.0) || res.flags != 0 || res.source[0] == '\0')
        return 5;
    prometheia_options_init(&opts);
    opts.frame = PROMETHEIA_FRAME_J2000;
    opts.coords = PROMETHEIA_COORDS_EQUATORIAL;
    opts.light_time = opts.deflection = opts.aberration = 0;
    if (prometheia_calc(engine, PROMETHEIA_JUPITER, 2451545.0, &opts, &res, &err) !=
            PROMETHEIA_OK ||
        !(res.lon_deg >= 0.0 && res.lon_deg < 360.0) || res.light_time_days != 0.0)
        return 6;

    /* Sidereal output reports its shift. */
    prometheia_options_init(&opts);
    opts.sidereal = PROMETHEIA_SIDEREAL_LAHIRI;
    if (prometheia_calc(engine, PROMETHEIA_SUN, 2451545.0, &opts, &res, &err) != PROMETHEIA_OK ||
        !(res.flags & PROMETHEIA_HAS_AYANAMSA) || fabs(res.ayanamsa_deg - 23.86) > 0.05)
        return 7;

    /* Errors: unknown body, bad selector, the observer as the body. */
    if (prometheia_calc(engine, 999, 2451545.0, NULL, &res, &err) != PROMETHEIA_ERROR_NOT_FOUND ||
        err.code != PROMETHEIA_ERROR_NOT_FOUND || res.dist_au != 0.0)
        return 8;
    prometheia_options_init(&opts);
    opts.frame = 42;
    if (prometheia_calc(engine, PROMETHEIA_SUN, 2451545.0, &opts, &res, &err) !=
        PROMETHEIA_ERROR_ARGUMENT)
        return 9;
    if (prometheia_calc(engine, PROMETHEIA_EARTH, 2451545.0, NULL, &res, &err) !=
        PROMETHEIA_ERROR_ARGUMENT)
        return 10;
    if (prometheia_engine_lookup(engine, "Ceres", &body, &err) != PROMETHEIA_ERROR_NOT_FOUND ||
        body != 0)
        return 11;

    /* A caller-supplied Delta T through a function pointer. */
    prometheia_engine_set_delta_t(engine, fixed_delta_t, &seconds);
    if (prometheia_calc_ut(engine, PROMETHEIA_SUN, 2451545.0, NULL, &res, &err) != PROMETHEIA_OK)
        return 12;
    prometheia_engine_set_delta_t(engine, NULL, NULL);

    /* Time scales. */
    {
        double jd_tt = 0.0;
        prometheia_utc utc;
        if (prometheia_utc_to_tt(2016, 12, 31, 23, 59, 60.5, &jd_tt, &err) != PROMETHEIA_OK)
            return 13;
        if (prometheia_tt_to_utc(jd_tt, &utc, &err) != PROMETHEIA_OK || utc.year != 2016 ||
            utc.minute != 59 || fabs(utc.second - 60.5) > 1e-4)
            return 14;
        if (prometheia_utc_to_tt(1960, 1, 1, 0, 0, 0.0, &jd_tt, &err) != PROMETHEIA_ERROR_ARGUMENT)
            return 15;
    }

    prometheia_engine_close(engine);
    prometheia_engine_close(NULL);
    return 0;
}

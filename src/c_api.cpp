// SPDX-License-Identifier: GPL-2.0-or-later
//
// The C interface (include/prometheia/prometheia.h): argument checks,
// enum and unit translation, and exception containment around
// prometheia::Engine. No behaviour of its own.

#include "prometheia/prometheia.h"

#include <cstring>
#include <new>
#include <string>

#include "prometheia/engine.hpp"
#include "prometheia/prometheia.hpp"
#include "prometheia/time.hpp"

using namespace prometheia;

namespace {

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

class CallbackDeltaT final : public time::DeltaTModel {
public:
    prometheia_delta_t_fn fn = nullptr;
    void* user = nullptr;
    double delta_t_seconds(double jd_tt) const override { return fn(user, jd_tt); }
};

} // namespace

struct prometheia_engine {
    Engine engine;
    CallbackDeltaT delta_t;
};

namespace {

prometheia_status to_status(ErrorCode code) {
    switch (code) {
    case ErrorCode::Ok:
        return PROMETHEIA_OK;
    case ErrorCode::IoError:
        return PROMETHEIA_ERROR_IO;
    case ErrorCode::FormatError:
        return PROMETHEIA_ERROR_FORMAT;
    case ErrorCode::CorruptionError:
        return PROMETHEIA_ERROR_CORRUPTION;
    case ErrorCode::ArgumentError:
        return PROMETHEIA_ERROR_ARGUMENT;
    case ErrorCode::NotFound:
        return PROMETHEIA_ERROR_NOT_FOUND;
    }
    return PROMETHEIA_ERROR_INTERNAL;
}

prometheia_status report(prometheia_error* err, prometheia_status code, const char* message) {
    if (err) {
        err->code = code;
        std::strncpy(err->message, message, PROMETHEIA_ERROR_MESSAGE_SIZE - 1);
        err->message[PROMETHEIA_ERROR_MESSAGE_SIZE - 1] = '\0';
    }
    return code;
}

prometheia_status report(prometheia_error* err, const Error& e) {
    return report(err, to_status(e.code), e.message.c_str());
}

prometheia_status succeed(prometheia_error* err) {
    return report(err, PROMETHEIA_OK, "");
}

prometheia_status argument(prometheia_error* err, const char* message) {
    return report(err, PROMETHEIA_ERROR_ARGUMENT, message);
}

// Runs f, turning any escaping exception into PROMETHEIA_ERROR_INTERNAL.
template <typename F>
prometheia_status guarded(prometheia_error* err, F&& f) {
    try {
        return f();
    } catch (const std::bad_alloc&) {
        return report(err, PROMETHEIA_ERROR_INTERNAL, "out of memory");
    } catch (const std::exception& e) {
        return report(err, PROMETHEIA_ERROR_INTERNAL, e.what());
    } catch (...) {
        return report(err, PROMETHEIA_ERROR_INTERNAL, "unknown internal error");
    }
}

// C options -> CalcOptions, rejecting out-of-range selector fields.
prometheia_status translate(const prometheia_options& c, CalcOptions& o, prometheia_error* err) {
    if (c.center < PROMETHEIA_CENTER_GEOCENTRIC || c.center > PROMETHEIA_CENTER_BARYCENTRIC) {
        return argument(err, "options: center out of range");
    }
    if (c.frame < PROMETHEIA_FRAME_ICRF || c.frame > PROMETHEIA_FRAME_TRUE_OF_DATE) {
        return argument(err, "options: frame out of range");
    }
    if (c.coords != PROMETHEIA_COORDS_ECLIPTIC && c.coords != PROMETHEIA_COORDS_EQUATORIAL) {
        return argument(err, "options: coords out of range");
    }
    switch (c.sidereal) {
    case PROMETHEIA_SIDEREAL_TROPICAL:
    case PROMETHEIA_SIDEREAL_FAGAN_BRADLEY:
    case PROMETHEIA_SIDEREAL_LAHIRI:
    case PROMETHEIA_SIDEREAL_USER:
        break;
    default:
        return argument(err, "options: sidereal mode out of range");
    }
    o.center = static_cast<Center>(c.center);
    o.frame = static_cast<Frame>(c.frame);
    o.coords = static_cast<Coords>(c.coords);
    o.sidereal = static_cast<SiderealMode>(c.sidereal);
    o.sidereal_epoch_jtdb = c.sidereal_epoch_jd;
    o.sidereal_ayanamsa_deg = c.sidereal_ayanamsa_deg;
    o.light_time = c.light_time != 0;
    o.deflection = c.deflection != 0;
    o.aberration = c.aberration != 0;
    o.speed = c.speed != 0;
    o.site.lon_rad = c.site_lon_deg * kDegToRad;
    o.site.lat_rad = c.site_lat_deg * kDegToRad;
    o.site.height_m = c.site_height_m;
    return PROMETHEIA_OK;
}

void fill(const CalcResult& r, prometheia_result& out) {
    out.lon_deg = r.pos.lon_deg;
    out.lat_deg = r.pos.lat_deg;
    out.dist_au = r.pos.dist_au;
    out.lon_speed = r.pos.lon_speed;
    out.lat_speed = r.pos.lat_speed;
    out.dist_speed = r.pos.dist_speed;
    for (int i = 0; i < 3; ++i) {
        out.xyz_au[i] = r.pos.xyz_au[i];
        out.vel_au_day[i] = r.pos.vel_au_day[i];
    }
    out.light_time_days = r.provenance.light_time_days;
    out.flags = 0;
    if (r.sigma_arcsec) {
        out.sigma_arcsec = *r.sigma_arcsec;
        out.flags |= PROMETHEIA_HAS_SIGMA;
    }
    if (r.ayanamsa_deg) {
        out.ayanamsa_deg = *r.ayanamsa_deg;
        out.flags |= PROMETHEIA_HAS_AYANAMSA;
    }
    out.denum = r.provenance.denum;
    // Provenance strings are std::string members of the engine, so the
    // view's data is NUL-terminated.
    out.source = r.provenance.source.empty() ? "" : r.provenance.source.data();
}

template <typename Call>
prometheia_status calc_common(prometheia_engine* engine, int body, double jd,
                              const prometheia_options* opts, prometheia_result* result,
                              prometheia_error* err, Call call) {
    if (result)
        std::memset(result, 0, sizeof *result);
    if (!engine)
        return argument(err, "engine is NULL");
    if (!result)
        return argument(err, "result is NULL");
    return guarded(err, [&] {
        CalcOptions o;
        if (opts) {
            if (prometheia_status s = translate(*opts, o, err); s != PROMETHEIA_OK)
                return s;
        }
        auto r = call(engine->engine, body, jd, o);
        if (!r)
            return report(err, r.error());
        fill(r.value(), *result);
        return succeed(err);
    });
}

} // namespace

extern "C" {

const char* prometheia_version(void) {
    return prometheia::version_string;
}

int prometheia_abi_version(void) {
    return PROMETHEIA_ABI_VERSION;
}

void prometheia_options_init(prometheia_options* opts) {
    if (!opts)
        return;
    const CalcOptions d;
    std::memset(opts, 0, sizeof *opts);
    opts->center = static_cast<int>(d.center);
    opts->frame = static_cast<int>(d.frame);
    opts->coords = static_cast<int>(d.coords);
    opts->sidereal = static_cast<int>(d.sidereal);
    opts->sidereal_epoch_jd = d.sidereal_epoch_jtdb;
    opts->sidereal_ayanamsa_deg = d.sidereal_ayanamsa_deg;
    opts->light_time = d.light_time;
    opts->deflection = d.deflection;
    opts->aberration = d.aberration;
    opts->speed = d.speed;
    opts->site_lon_deg = d.site.lon_rad / kDegToRad;
    opts->site_lat_deg = d.site.lat_rad / kDegToRad;
    opts->site_height_m = d.site.height_m;
}

prometheia_status prometheia_engine_open(const char* path, prometheia_engine** out,
                                         prometheia_error* err) {
    if (out)
        *out = nullptr;
    if (!path)
        return argument(err, "ephemeris path is NULL");
    if (!out)
        return argument(err, "output handle pointer is NULL");
    return guarded(err, [&] {
        auto e = Engine::open(path);
        if (!e)
            return report(err, e.error());
        *out = new prometheia_engine{std::move(e).value(), {}};
        return succeed(err);
    });
}

void prometheia_engine_close(prometheia_engine* engine) {
    delete engine;
}

const char* prometheia_engine_source(const prometheia_engine* engine) {
    if (!engine)
        return "";
    const std::string_view s = engine->engine.source();
    return s.empty() ? "" : s.data();
}

prometheia_status prometheia_engine_add_catalog(prometheia_engine* engine, const char* path,
                                                prometheia_error* err) {
    if (!engine)
        return argument(err, "engine is NULL");
    if (!path)
        return argument(err, "catalog path is NULL");
    return guarded(err, [&] {
        auto r = engine->engine.add_catalog(path);
        return r ? succeed(err) : report(err, r.error());
    });
}

prometheia_status prometheia_engine_lookup(const prometheia_engine* engine, const char* name,
                                           int* body, prometheia_error* err) {
    if (body)
        *body = 0;
    if (!engine)
        return argument(err, "engine is NULL");
    if (!name)
        return argument(err, "name is NULL");
    if (!body)
        return argument(err, "body pointer is NULL");
    return guarded(err, [&] {
        auto r = engine->engine.lookup(name);
        if (!r)
            return report(err, r.error());
        *body = r.value();
        return succeed(err);
    });
}

prometheia_status prometheia_calc(prometheia_engine* engine, int body, double jd_tt,
                                  const prometheia_options* opts, prometheia_result* result,
                                  prometheia_error* err) {
    return calc_common(
        engine, body, jd_tt, opts, result, err,
        [](Engine& e, int b, double jd, const CalcOptions& o) { return e.calc(b, jd, o); });
}

prometheia_status prometheia_calc_ut(prometheia_engine* engine, int body, double jd_ut1,
                                     const prometheia_options* opts, prometheia_result* result,
                                     prometheia_error* err) {
    return calc_common(
        engine, body, jd_ut1, opts, result, err,
        [](Engine& e, int b, double jd, const CalcOptions& o) { return e.calc_ut(b, jd, o); });
}

void prometheia_engine_set_delta_t(prometheia_engine* engine, prometheia_delta_t_fn fn,
                                   void* user) {
    if (!engine)
        return;
    engine->delta_t.fn = fn;
    engine->delta_t.user = user;
    engine->engine.set_delta_t_model(fn ? &engine->delta_t : nullptr);
}

double prometheia_jd_from_ymdhms(int year, int month, int day, int hour, int minute,
                                 double second) {
    return time::jd_from_ymdhms(year, month, day, hour, minute, second);
}

void prometheia_civil_from_jd(double jd, int* year, int* month, double* day) {
    const time::Civil c = time::civil_from_jd(jd);
    if (year)
        *year = c.year;
    if (month)
        *month = c.month;
    if (day)
        *day = c.day;
}

prometheia_status prometheia_utc_to_tt(int year, int month, int day, int hour, int minute,
                                       double second, double* jd_tt, prometheia_error* err) {
    if (jd_tt)
        *jd_tt = 0.0;
    if (!jd_tt)
        return argument(err, "jd_tt pointer is NULL");
    return guarded(err, [&] {
        auto r = time::utc_to_tt(year, month, day, hour, minute, second);
        if (!r)
            return report(err, r.error());
        *jd_tt = r.value();
        return succeed(err);
    });
}

prometheia_status prometheia_tt_to_utc(double jd_tt, prometheia_utc* utc, prometheia_error* err) {
    if (utc)
        std::memset(utc, 0, sizeof *utc);
    if (!utc)
        return argument(err, "utc pointer is NULL");
    return guarded(err, [&] {
        auto r = time::tt_to_utc(jd_tt);
        if (!r)
            return report(err, r.error());
        const time::Utc& u = r.value();
        *utc = prometheia_utc{u.year, u.month, u.day, u.hour, u.minute, u.second, u.tai_minus_utc};
        return succeed(err);
    });
}

double prometheia_delta_t(double jd_tt) {
    return time::delta_t(jd_tt);
}

double prometheia_tdb_minus_tt(double jd_tt) {
    return time::tdb_minus_tt(jd_tt);
}

} // extern "C"

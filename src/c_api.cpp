// SPDX-License-Identifier: GPL-2.0-or-later
//
// The C interface (include/prometheia/prometheia.h): argument checks,
// enum and unit translation, and exception containment around
// prometheia::Engine. No behaviour of its own.

#include "prometheia/prometheia.h"
#include "prometheia/stars.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "prometheia/engine.hpp"
#include "prometheia/hypotheticals.hpp"
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

// The C sidereal values are the engine's (a static_cast passes them through).
static_assert(PROMETHEIA_SIDEREAL_FAGAN_BRADLEY == int(SiderealMode::FaganBradley));
static_assert(PROMETHEIA_SIDEREAL_LAHIRI == int(SiderealMode::Lahiri));
static_assert(PROMETHEIA_SIDEREAL_GALCENT_0SAG == int(SiderealMode::GalacticCentre0Sag));
static_assert(PROMETHEIA_SIDEREAL_TRUE_CITRA == int(SiderealMode::TrueCitra));
static_assert(PROMETHEIA_SIDEREAL_TRUE_REVATI == int(SiderealMode::TrueRevati));
static_assert(PROMETHEIA_SIDEREAL_TRUE_PUSHYA == int(SiderealMode::TruePushya));
static_assert(PROMETHEIA_SIDEREAL_GALCENT_RGILBRAND == int(SiderealMode::GalacticCentreGilBrand));
static_assert(PROMETHEIA_SIDEREAL_GALEQU_IAU1958 == int(SiderealMode::GalacticEquatorIau1958));
static_assert(PROMETHEIA_SIDEREAL_GALEQU_TRUE == int(SiderealMode::GalacticEquatorTrue));
static_assert(PROMETHEIA_SIDEREAL_GALEQU_MULA == int(SiderealMode::GalacticEquatorMula));
static_assert(PROMETHEIA_SIDEREAL_TRUE_MULA == int(SiderealMode::TrueMula));
static_assert(PROMETHEIA_SIDEREAL_GALCENT_MULA_WILHELM ==
              int(SiderealMode::GalacticCentreMulaWilhelm));
static_assert(PROMETHEIA_SIDEREAL_GALCENT_COCHRANE == int(SiderealMode::GalacticCentreCochrane));
static_assert(PROMETHEIA_SIDEREAL_USER == int(SiderealMode::User));
static_assert(PROMETHEIA_SIDEREAL_PLANE_DATE == int(SiderealPlane::EclipticOfDate));
static_assert(PROMETHEIA_SIDEREAL_PLANE_ANCHOR == int(SiderealPlane::EclipticOfAnchor));
static_assert(PROMETHEIA_SIDEREAL_PLANE_INVARIABLE == int(SiderealPlane::Invariable));

// C options -> CalcOptions, rejecting out-of-range selector fields.
prometheia_status translate(const prometheia_options& c, CalcOptions& o, prometheia_error* err) {
    if (c.center < PROMETHEIA_CENTER_GEOCENTRIC || c.center > PROMETHEIA_CENTER_BODY) {
        return argument(err, "options: center out of range");
    }
    if (c.frame < PROMETHEIA_FRAME_ICRF || c.frame > PROMETHEIA_FRAME_TRUE_OF_DATE) {
        return argument(err, "options: frame out of range");
    }
    if (c.coords != PROMETHEIA_COORDS_ECLIPTIC && c.coords != PROMETHEIA_COORDS_EQUATORIAL) {
        return argument(err, "options: coords out of range");
    }
    if (c.precession != PROMETHEIA_PRECESSION_IAU2006 &&
        c.precession != PROMETHEIA_PRECESSION_VONDRAK2011)
        return argument(err, "options: precession model out of range");
    switch (c.sidereal) {
    case PROMETHEIA_SIDEREAL_TROPICAL:
    case PROMETHEIA_SIDEREAL_FAGAN_BRADLEY:
    case PROMETHEIA_SIDEREAL_LAHIRI:
    case PROMETHEIA_SIDEREAL_GALCENT_0SAG:
    case PROMETHEIA_SIDEREAL_TRUE_CITRA:
    case PROMETHEIA_SIDEREAL_TRUE_REVATI:
    case PROMETHEIA_SIDEREAL_TRUE_PUSHYA:
    case PROMETHEIA_SIDEREAL_GALCENT_RGILBRAND:
    case PROMETHEIA_SIDEREAL_GALEQU_IAU1958:
    case PROMETHEIA_SIDEREAL_GALEQU_TRUE:
    case PROMETHEIA_SIDEREAL_GALEQU_MULA:
    case PROMETHEIA_SIDEREAL_TRUE_MULA:
    case PROMETHEIA_SIDEREAL_GALCENT_MULA_WILHELM:
    case PROMETHEIA_SIDEREAL_GALCENT_COCHRANE:
    case PROMETHEIA_SIDEREAL_USER:
        break;
    default:
        return argument(err, "options: sidereal mode out of range");
    }
    if (c.sidereal_plane < PROMETHEIA_SIDEREAL_PLANE_DATE ||
        c.sidereal_plane > PROMETHEIA_SIDEREAL_PLANE_INVARIABLE) {
        return argument(err, "options: sidereal plane out of range");
    }
    o.center = static_cast<Center>(c.center);
    o.frame = static_cast<Frame>(c.frame);
    o.coords = static_cast<Coords>(c.coords);
    o.sidereal = static_cast<SiderealMode>(c.sidereal);
    o.sidereal_plane = static_cast<SiderealPlane>(c.sidereal_plane);
    o.precession = static_cast<Precession>(c.precession);
    o.sidereal_epoch_jtdb = c.sidereal_epoch_jd;
    o.sidereal_ayanamsa_deg = c.sidereal_ayanamsa_deg;
    o.light_time = c.light_time != 0;
    o.deflection = c.deflection != 0;
    o.aberration = c.aberration != 0;
    o.speed = c.speed != 0;
    o.sigma = c.sigma != 0;
    o.site.lon_rad = c.site_lon_deg * kDegToRad;
    o.site.lat_rad = c.site_lat_deg * kDegToRad;
    o.site.height_m = c.site_height_m;
    o.center_body = c.center_body;
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

// NUL-terminated copy, truncated to fit.
void copy_string(char* dst, size_t cap, std::string_view src) {
    if (cap == 0)
        return;
    const size_t n = std::min(src.size(), cap - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
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

bool orbit_selectors(int point, int elements, prometheia_result* result, prometheia_error* err,
                     prometheia_status& status) {
    if (point < PROMETHEIA_ORBIT_ASCENDING_NODE || point > PROMETHEIA_ORBIT_APHELION) {
        if (result)
            std::memset(result, 0, sizeof *result);
        status = argument(err, "orbit point out of range");
        return false;
    }
    if (elements != PROMETHEIA_ELEMENTS_MEAN && elements != PROMETHEIA_ELEMENTS_OSCULATING) {
        if (result)
            std::memset(result, 0, sizeof *result);
        status = argument(err, "orbit elements out of range");
        return false;
    }
    return true;
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
    opts->precession = static_cast<int>(d.precession);
    opts->sidereal_epoch_jd = d.sidereal_epoch_jtdb;
    opts->sidereal_ayanamsa_deg = d.sidereal_ayanamsa_deg;
    opts->light_time = d.light_time;
    opts->deflection = d.deflection;
    opts->aberration = d.aberration;
    opts->speed = d.speed;
    opts->sigma = d.sigma;
    opts->site_lon_deg = d.site.lon_rad / kDegToRad;
    opts->site_lat_deg = d.site.lat_rad / kDegToRad;
    opts->site_height_m = d.site.height_m;
    opts->center_body = d.center_body;
    opts->sidereal_plane = static_cast<int>(d.sidereal_plane);
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

prometheia_status prometheia_engine_add_perturbers(prometheia_engine* engine, const char* path,
                                                   prometheia_error* err) {
    if (!engine)
        return argument(err, "engine is NULL");
    if (!path)
        return argument(err, "perturber kernel path is NULL");
    return guarded(err, [&] {
        auto r = engine->engine.add_perturbers(path);
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

prometheia_status prometheia_calc_orbit_point(prometheia_engine* engine, int body, int point,
                                              int elements, double jd_tt,
                                              const prometheia_options* opts,
                                              prometheia_result* result, prometheia_error* err) {
    prometheia_status status = PROMETHEIA_OK;
    if (!orbit_selectors(point, elements, result, err, status))
        return status;
    return calc_common(engine, body, jd_tt, opts, result, err,
                       [point, elements](Engine& e, int b, double jd, const CalcOptions& o) {
                           return e.calc_orbit_point(b, static_cast<OrbitPoint>(point),
                                                     static_cast<OrbitElements>(elements), jd, o);
                       });
}

prometheia_status prometheia_calc_orbit_point_ut(prometheia_engine* engine, int body, int point,
                                                 int elements, double jd_ut1,
                                                 const prometheia_options* opts,
                                                 prometheia_result* result, prometheia_error* err) {
    prometheia_status status = PROMETHEIA_OK;
    if (!orbit_selectors(point, elements, result, err, status))
        return status;
    return calc_common(engine, body, jd_ut1, opts, result, err,
                       [point, elements](Engine& e, int b, double jd, const CalcOptions& o) {
                           return e.calc_orbit_point_ut(b, static_cast<OrbitPoint>(point),
                                                        static_cast<OrbitElements>(elements), jd,
                                                        o);
                       });
}

// ---- bodies from elements, named hypotheticals ------------------------------

namespace {

// C elements -> PolynomialElements, rejecting out-of-range selectors. The
// engine checks the term count and the orbit itself.
bool to_elements(const prometheia_elements& c, PolynomialElements& p, prometheia_error* err,
                 prometheia_status& status) {
    if (c.equinox < PROMETHEIA_EQUINOX_J2000 || c.equinox > PROMETHEIA_EQUINOX_EXPLICIT) {
        status = argument(err, "elements: equinox out of range");
        return false;
    }
    if (c.origin != PROMETHEIA_ELEMENTS_ORIGIN_SUN &&
        c.origin != PROMETHEIA_ELEMENTS_ORIGIN_EARTH) {
        status = argument(err, "elements: origin out of range");
        return false;
    }
    p.epoch_jd_tt = c.epoch_jd_tt;
    p.equinox = static_cast<ElementEquinox>(c.equinox);
    p.equinox_jd_tt = c.equinox_jd_tt;
    p.origin = static_cast<ElementOrigin>(c.origin);
    p.n_terms = c.n_terms;
    for (int k = 0; k < 5; ++k) {
        p.mean_anomaly[k] = c.mean_anomaly[k];
        p.semi_major_axis[k] = c.semi_major_axis[k];
        p.eccentricity[k] = c.eccentricity[k];
        p.arg_perihelion[k] = c.arg_perihelion[k];
        p.ascending_node[k] = c.ascending_node[k];
        p.inclination[k] = c.inclination[k];
    }
    return true;
}

void from_elements(const PolynomialElements& p, prometheia_elements& c) {
    c.epoch_jd_tt = p.epoch_jd_tt;
    c.equinox = static_cast<int>(p.equinox);
    c.equinox_jd_tt = p.equinox_jd_tt;
    c.origin = static_cast<int>(p.origin);
    c.n_terms = p.n_terms;
    for (int k = 0; k < 5; ++k) {
        c.mean_anomaly[k] = p.mean_anomaly[k];
        c.semi_major_axis[k] = p.semi_major_axis[k];
        c.eccentricity[k] = p.eccentricity[k];
        c.arg_perihelion[k] = p.arg_perihelion[k];
        c.ascending_node[k] = p.ascending_node[k];
        c.inclination[k] = p.inclination[k];
    }
}

prometheia_status calc_elements_common(prometheia_engine* engine,
                                       const prometheia_elements* elements, double jd,
                                       const prometheia_options* opts, prometheia_result* result,
                                       prometheia_error* err, bool ut) {
    if (!elements) {
        if (result)
            std::memset(result, 0, sizeof *result);
        return argument(err, "elements is NULL");
    }
    PolynomialElements p;
    prometheia_status status = PROMETHEIA_OK;
    if (!to_elements(*elements, p, err, status)) {
        if (result)
            std::memset(result, 0, sizeof *result);
        return status;
    }
    return calc_common(engine, 0, jd, opts, result, err,
                       [&p, ut](Engine& e, int, double t, const CalcOptions& o) {
                           return ut ? e.calc_elements_ut(p, t, o) : e.calc_elements(p, t, o);
                       });
}

prometheia_status calc_hypothetical_common(prometheia_engine* engine, const char* token, double jd,
                                           const prometheia_options* opts,
                                           prometheia_result* result, prometheia_error* err,
                                           bool ut) {
    if (!token) {
        if (result)
            std::memset(result, 0, sizeof *result);
        return argument(err, "token is NULL");
    }
    return calc_common(engine, 0, jd, opts, result, err,
                       [token, ut](Engine& e, int, double t, const CalcOptions& o) {
                           return ut ? e.calc_hypothetical_ut(token, t, o)
                                     : e.calc_hypothetical(token, t, o);
                       });
}

} // namespace

prometheia_status prometheia_calc_elements(prometheia_engine* engine,
                                           const prometheia_elements* elements, double jd_tt,
                                           const prometheia_options* opts,
                                           prometheia_result* result, prometheia_error* err) {
    return calc_elements_common(engine, elements, jd_tt, opts, result, err, false);
}

prometheia_status prometheia_calc_elements_ut(prometheia_engine* engine,
                                              const prometheia_elements* elements, double jd_ut1,
                                              const prometheia_options* opts,
                                              prometheia_result* result, prometheia_error* err) {
    return calc_elements_common(engine, elements, jd_ut1, opts, result, err, true);
}

prometheia_status prometheia_engine_add_hypotheticals(prometheia_engine* engine,
                                                      const char* element_file_path,
                                                      prometheia_error* err) {
    if (!engine)
        return argument(err, "engine is NULL");
    if (!element_file_path)
        return argument(err, "element file path is NULL");
    return guarded(err, [&] {
        auto r = engine->engine.add_hypotheticals(element_file_path);
        return r ? succeed(err) : report(err, r.error());
    });
}

int prometheia_hypothetical_count(const prometheia_engine* engine) {
    if (!engine)
        return 0;
    try {
        return static_cast<int>(engine->engine.hypothetical_tokens().size());
    } catch (...) {
        return 0;
    }
}

const char* prometheia_hypothetical_token(const prometheia_engine* engine, int index) {
    if (!engine || index < 0)
        return nullptr;
    try {
        const std::vector<std::string> tokens = engine->engine.hypothetical_tokens();
        if (size_t(index) >= tokens.size())
            return nullptr;
        // The engine's own copy, which outlives this call.
        const hypotheticals::Body* b = engine->engine.hypothetical(tokens[size_t(index)]);
        return b ? b->token.c_str() : nullptr;
    } catch (...) {
        return nullptr;
    }
}

prometheia_status prometheia_hypothetical_get(const prometheia_engine* engine, const char* token,
                                              prometheia_hypothetical* out, prometheia_error* err) {
    if (out)
        std::memset(out, 0, sizeof *out);
    if (!engine)
        return argument(err, "engine is NULL");
    if (!token)
        return argument(err, "token is NULL");
    if (!out)
        return argument(err, "output pointer is NULL");
    return guarded(err, [&] {
        const hypotheticals::Body* b = engine->engine.hypothetical(token);
        if (!b)
            return report(err, PROMETHEIA_ERROR_NOT_FOUND, "hypothetical body is not defined");
        out->token = b->token.c_str();
        out->name = b->name.c_str();
        out->set = b->set.c_str();
        out->citation = b->citation.c_str();
        from_elements(b->elements, out->elements);
        return succeed(err);
    });
}

prometheia_status prometheia_calc_hypothetical(prometheia_engine* engine, const char* token,
                                               double jd_tt, const prometheia_options* opts,
                                               prometheia_result* result, prometheia_error* err) {
    return calc_hypothetical_common(engine, token, jd_tt, opts, result, err, false);
}

prometheia_status prometheia_calc_hypothetical_ut(prometheia_engine* engine, const char* token,
                                                  double jd_ut1, const prometheia_options* opts,
                                                  prometheia_result* result,
                                                  prometheia_error* err) {
    return calc_hypothetical_common(engine, token, jd_ut1, opts, result, err, true);
}

int prometheia_star_count(void) {
    return static_cast<int>(stars::count());
}

prometheia_status prometheia_star_find(const char* query, int* index, prometheia_error* err) {
    if (index)
        *index = -1;
    if (!query)
        return argument(err, "query is NULL");
    if (!index)
        return argument(err, "index pointer is NULL");
    return guarded(err, [&] {
        auto r = stars::find(query);
        if (!r)
            return report(err, r.error());
        *index = static_cast<int>(r.value());
        return succeed(err);
    });
}

int prometheia_star_lookup(const char* query, int prefix, prometheia_star_match* matches, int max) {
    if (!query || !matches || max <= 0)
        return 0;
    try {
        const auto found = stars::lookup(query, size_t(max), prefix != 0);
        for (size_t k = 0; k < found.size(); ++k) {
            matches[k].index = static_cast<int>(found[k].index);
            matches[k].quality = static_cast<int>(found[k].quality);
            copy_string(matches[k].matched, sizeof matches[k].matched, found[k].matched);
        }
        return static_cast<int>(found.size());
    } catch (...) {
        return 0;
    }
}

prometheia_status prometheia_star_info(int index, prometheia_star* star, prometheia_error* err) {
    if (star)
        std::memset(star, 0, sizeof *star);
    if (!star)
        return argument(err, "star is NULL");
    if (index < 0 || size_t(index) >= stars::count())
        return report(err, PROMETHEIA_ERROR_NOT_FOUND, "no catalog star with that index");
    return guarded(err, [&] {
        const stars::Object& o = stars::at(size_t(index));
        star->index = index;
        star->kind = static_cast<int>(o.kind);
        star->astrometry = static_cast<int>(o.astrometry);
        star->hr = o.hr;
        star->hd = o.hd;
        star->hip = o.hip;
        star->flamsteed = o.flamsteed;
        star->messier = o.messier;
        star->bayer = o.bayer;
        star->bayer_index = o.bayer_index;
        copy_string(star->constellation, sizeof star->constellation, o.constellation);
        copy_string(star->name, sizeof star->name, o.name());
        std::string names;
        for (std::string_view n : o.names) {
            if (names.size() + n.size() + 1 >= sizeof star->names)
                break;
            names += (names.empty() ? "" : "|") + std::string(n);
        }
        copy_string(star->names, sizeof star->names, names);
        copy_string(star->bayer_designation, sizeof star->bayer_designation, o.bayer_designation());
        copy_string(star->spectral_type, sizeof star->spectral_type, o.spectral_type);
        star->vmag = o.vmag;
        star->ra_deg = o.ra_deg;
        star->dec_deg = o.dec_deg;
        star->epoch_jyear = o.epoch_jyear;
        star->pm_ra_mas_yr = o.pm_ra_mas_yr;
        star->pm_dec_mas_yr = o.pm_dec_mas_yr;
        star->parallax_mas = o.parallax_mas;
        star->rv_km_s = o.rv_km_s;
        star->size_arcmin = o.size_arcmin;
        return succeed(err);
    });
}

prometheia_status prometheia_calc_star(prometheia_engine* engine, int index, double jd_tt,
                                       const prometheia_options* opts, prometheia_result* result,
                                       prometheia_error* err) {
    if (index < 0) {
        if (result)
            std::memset(result, 0, sizeof *result);
        return report(err, PROMETHEIA_ERROR_NOT_FOUND, "no catalog star with that index");
    }
    return calc_common(engine, index, jd_tt, opts, result, err,
                       [](Engine& e, int i, double jd, const CalcOptions& o) {
                           return e.calc_star(size_t(i), jd, o);
                       });
}

prometheia_status prometheia_calc_star_ut(prometheia_engine* engine, int index, double jd_ut1,
                                          const prometheia_options* opts, prometheia_result* result,
                                          prometheia_error* err) {
    if (index < 0) {
        if (result)
            std::memset(result, 0, sizeof *result);
        return report(err, PROMETHEIA_ERROR_NOT_FOUND, "no catalog star with that index");
    }
    return calc_common(engine, index, jd_ut1, opts, result, err,
                       [](Engine& e, int i, double jd, const CalcOptions& o) {
                           return e.calc_star_ut(size_t(i), jd, o);
                       });
}

const char* prometheia_constellation_at(double ra_deg, double dec_deg) {
    const std::string_view c = stars::constellation_at(ra_deg, dec_deg);
    // The views point into NUL-terminated string literals.
    return c.empty() ? "" : c.data();
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

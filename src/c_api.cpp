// SPDX-License-Identifier: GPL-2.0-or-later
//
// The C interface (include/prometheia/prometheia.h): argument checks,
// enum and unit translation, and exception containment around
// prometheia::Engine. No behaviour of its own.

#include "prometheia/prometheia.h"
#include "prometheia/stars.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "prometheia/engine.hpp"
#include "prometheia/frames.hpp"
#include "prometheia/heliacal.hpp"
#include "prometheia/hypotheticals.hpp"
#include "prometheia/lunar_eclipse.hpp"
#include "prometheia/prometheia.hpp"
#include "prometheia/registry.hpp"
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
    // What prometheia_engine_ephemeris_span() last returned, so that its
    // source string outlives the call.
    mutable std::vector<Engine::EphemerisSpan> spans;
    // The same for prometheia_engine_catalog_info()'s path.
    mutable std::vector<Engine::CatalogInfo> catalogs;
    // The object registry, made on first use (its indexes are lazy too).
    std::unique_ptr<registry::Registry> registry_;
    registry::Registry& registry() {
        if (!registry_)
            registry_ = std::make_unique<registry::Registry>(engine);
        return *registry_;
    }
};

// A dataset handle holds one reference to the corpus; each context opened
// from it holds another.
struct prometheia_dataset {
    std::shared_ptr<Corpus> corpus;
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
    case ErrorCode::CoverageError:
        return PROMETHEIA_ERROR_COVERAGE;
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

// One of the dataset's add_* calls, after the common checks.
template <typename Add>
prometheia_status dataset_add(prometheia_dataset* dataset, const char* path, const char* what,
                              prometheia_error* err, Add&& add) {
    if (!dataset)
        return argument(err, "dataset is NULL");
    if (!path)
        return argument(err, what);
    return guarded(err, [&] {
        Result<void> r = add(*dataset->corpus, std::string(path));
        return r ? succeed(err) : report(err, r.error());
    });
}

// The C sidereal values are the engine's (a static_cast passes them through).
static_assert(PROMETHEIA_SIDEREAL_FAGAN_BRADLEY == int(SiderealMode::FaganBradley));
static_assert(PROMETHEIA_SIDEREAL_LAHIRI == int(SiderealMode::Lahiri));
static_assert(PROMETHEIA_SIDEREAL_RAMAN == int(SiderealMode::Raman));
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
// The C house systems are protocol v4's A.22 values, which are the engine's.
static_assert(PROMETHEIA_HOUSES_PLACIDUS == int(houses::System::Placidus));
static_assert(PROMETHEIA_HOUSES_KOCH == int(houses::System::Koch));
static_assert(PROMETHEIA_HOUSES_PORPHYRY == int(houses::System::Porphyry));
static_assert(PROMETHEIA_HOUSES_REGIOMONTANUS == int(houses::System::Regiomontanus));
static_assert(PROMETHEIA_HOUSES_CAMPANUS == int(houses::System::Campanus));
static_assert(PROMETHEIA_HOUSES_EQUAL == int(houses::System::Equal));
static_assert(PROMETHEIA_HOUSES_WHOLE_SIGN == int(houses::System::WholeSign));
static_assert(PROMETHEIA_HOUSES_ALCABITIUS == int(houses::System::Alcabitius));
static_assert(PROMETHEIA_HOUSES_MORINUS == int(houses::System::Morinus));
static_assert(PROMETHEIA_HOUSES_MERIDIAN == int(houses::System::Meridian));
static_assert(PROMETHEIA_HOUSES_TOPOCENTRIC == int(houses::System::Topocentric));

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
    case PROMETHEIA_SIDEREAL_RAMAN:
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

void fill(const houses::Houses& h, prometheia_houses& out) {
    for (int k = 0; k < 12; ++k)
        out.cusp_deg[k] = h.cusp_deg[size_t(k)];
    out.asc_deg = h.angles.asc_deg;
    out.mc_deg = h.angles.mc_deg;
    out.armc_deg = h.angles.armc_deg;
    out.vertex_deg = h.angles.vertex_deg;
    out.equatorial_asc_deg = h.angles.equatorial_asc_deg;
}

bool house_system(int system, houses::System& s, prometheia_error* err, prometheia_status& status) {
    if (system < PROMETHEIA_HOUSES_PLACIDUS || system > PROMETHEIA_HOUSES_TOPOCENTRIC) {
        status = argument(err, "house system out of range");
        return false;
    }
    s = static_cast<houses::System>(system);
    return true;
}

template <typename Call>
prometheia_status houses_common(prometheia_engine* engine, int system, double jd, double lon_deg,
                                double lat_deg, const prometheia_options* opts,
                                prometheia_houses* out, prometheia_error* err, Call call) {
    if (out)
        std::memset(out, 0, sizeof *out);
    if (!engine)
        return argument(err, "engine is NULL");
    if (!out)
        return argument(err, "houses output is NULL");
    houses::System s{};
    if (prometheia_status st; !house_system(system, s, err, st))
        return st;
    return guarded(err, [&] {
        CalcOptions o;
        if (opts) {
            if (prometheia_status st = translate(*opts, o, err); st != PROMETHEIA_OK)
                return st;
        }
        frames::GeoSite site;
        site.lon_rad = lon_deg * kDegToRad;
        site.lat_rad = lat_deg * kDegToRad;
        auto r = call(engine->engine, s, jd, site, o);
        if (!r)
            return report(err, r.error());
        fill(r.value().houses, *out);
        out->obliquity_deg = r.value().obliquity_deg;
        if (r.value().ayanamsa_deg) {
            out->ayanamsa_deg = *r.value().ayanamsa_deg;
            out->flags |= PROMETHEIA_HAS_AYANAMSA;
        }
        return succeed(err);
    });
}

// The precession model a call reads from its options (NULL: the default).
bool precession_of(const prometheia_options* opts, Precession& p, prometheia_error* err,
                   prometheia_status& status) {
    p = CalcOptions{}.precession;
    if (!opts)
        return true;
    if (opts->precession != PROMETHEIA_PRECESSION_IAU2006 &&
        opts->precession != PROMETHEIA_PRECESSION_VONDRAK2011) {
        status = argument(err, "options: precession model out of range");
        return false;
    }
    p = static_cast<Precession>(opts->precession);
    return true;
}

static_assert(PROMETHEIA_EVENT_RISE == int(Engine::Event::Rise));
static_assert(PROMETHEIA_EVENT_SET == int(Engine::Event::Set));
static_assert(PROMETHEIA_EVENT_UPPER_TRANSIT == int(Engine::Event::UpperTransit));
static_assert(PROMETHEIA_EVENT_LOWER_TRANSIT == int(Engine::Event::LowerTransit));
static_assert(PROMETHEIA_OUTCOME_FOUND == int(Engine::Outcome::Found));
static_assert(PROMETHEIA_OUTCOME_ALWAYS_ABOVE == int(Engine::Outcome::AlwaysAbove));
static_assert(PROMETHEIA_OUTCOME_ALWAYS_BELOW == int(Engine::Outcome::AlwaysBelow));
static_assert(PROMETHEIA_OUTCOME_NOT_IN_WINDOW == int(Engine::Outcome::NotInWindow));

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
    if (elements != PROMETHEIA_ELEMENTS_MEAN && elements != PROMETHEIA_ELEMENTS_OSCULATING &&
        elements != PROMETHEIA_ELEMENTS_INTERPOLATED) {
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
        *out = new prometheia_engine{std::move(e).value(), {}, {}, {}, {}};
        return succeed(err);
    });
}

void prometheia_engine_close(prometheia_engine* engine) {
    delete engine;
}

prometheia_status prometheia_dataset_open(const char* path, prometheia_dataset** out,
                                          prometheia_error* err) {
    if (out)
        *out = nullptr;
    if (!path)
        return argument(err, "ephemeris path is NULL");
    if (!out)
        return argument(err, "output handle pointer is NULL");
    return guarded(err, [&] {
        auto c = Corpus::open(path);
        if (!c)
            return report(err, c.error());
        *out = new prometheia_dataset{std::move(c).value()};
        return succeed(err);
    });
}

prometheia_status prometheia_dataset_add_ephemeris(prometheia_dataset* dataset, const char* path,
                                                   prometheia_error* err) {
    return dataset_add(dataset, path, "ephemeris path is NULL", err,
                       [](Corpus& c, const std::string& p) { return c.add_ephemeris(p); });
}

prometheia_status prometheia_dataset_add_catalog(prometheia_dataset* dataset, const char* path,
                                                 prometheia_error* err) {
    return dataset_add(dataset, path, "catalog path is NULL", err,
                       [](Corpus& c, const std::string& p) { return c.add_catalog(p); });
}

prometheia_status prometheia_dataset_add_perturbers(prometheia_dataset* dataset, const char* path,
                                                    prometheia_error* err) {
    return dataset_add(dataset, path, "perturber kernel path is NULL", err,
                       [](Corpus& c, const std::string& p) { return c.add_perturbers(p); });
}

prometheia_status prometheia_dataset_add_hypotheticals(prometheia_dataset* dataset,
                                                       const char* element_file_path,
                                                       prometheia_error* err) {
    return dataset_add(dataset, element_file_path, "element file path is NULL", err,
                       [](Corpus& c, const std::string& p) { return c.add_hypotheticals(p); });
}

prometheia_status prometheia_dataset_freeze(prometheia_dataset* dataset, prometheia_error* err) {
    if (!dataset)
        return argument(err, "dataset is NULL");
    dataset->corpus->freeze();
    return succeed(err);
}

void prometheia_dataset_close(prometheia_dataset* dataset) {
    delete dataset;
}

prometheia_status prometheia_context_open(const prometheia_dataset* dataset,
                                          prometheia_engine** out, prometheia_error* err) {
    if (out)
        *out = nullptr;
    if (!dataset)
        return argument(err, "dataset is NULL");
    if (!out)
        return argument(err, "output handle pointer is NULL");
    return guarded(err, [&] {
        auto e = Engine::open_context(dataset->corpus);
        if (!e)
            return report(err, e.error());
        *out = new prometheia_engine{std::move(e).value(), {}, {}, {}, {}};
        return succeed(err);
    });
}

const char* prometheia_engine_source(const prometheia_engine* engine) {
    if (!engine)
        return "";
    const std::string_view s = engine->engine.source();
    return s.empty() ? "" : s.data();
}

int prometheia_engine_ephemeris_count(const prometheia_engine* engine) {
    if (!engine)
        return 0;
    return int(engine->engine.ephemeris_spans().size());
}

prometheia_status prometheia_engine_ephemeris_span(const prometheia_engine* engine, int index,
                                                   double* first_jd_tdb, double* last_jd_tdb,
                                                   const char** source, prometheia_error* err) {
    if (!engine)
        return argument(err, "engine is NULL");
    if (!first_jd_tdb || !last_jd_tdb)
        return argument(err, "span outputs are NULL");
    return guarded(err, [&] {
        engine->spans = engine->engine.ephemeris_spans();
        if (index < 0 || size_t(index) >= engine->spans.size())
            return argument(err,
                            ("ephemeris index " + std::to_string(index) + " out of range").c_str());
        const Engine::EphemerisSpan& s = engine->spans[size_t(index)];
        *first_jd_tdb = s.first_jd_tdb;
        *last_jd_tdb = s.last_jd_tdb;
        if (source)
            *source = s.source.c_str();
        return succeed(err);
    });
}

int prometheia_engine_catalog_count(const prometheia_engine* engine) {
    if (!engine)
        return 0;
    return int(engine->engine.catalogs().size());
}

prometheia_status prometheia_engine_catalog_info(const prometheia_engine* engine, int index,
                                                 int* bodies, const char** path,
                                                 prometheia_error* err) {
    if (!engine)
        return argument(err, "engine is NULL");
    if (!bodies)
        return argument(err, "bodies output is NULL");
    return guarded(err, [&] {
        engine->catalogs = engine->engine.catalogs();
        if (index < 0 || size_t(index) >= engine->catalogs.size())
            return argument(err,
                            ("catalog index " + std::to_string(index) + " out of range").c_str());
        const Engine::CatalogInfo& c = engine->catalogs[size_t(index)];
        *bodies = int(c.bodies);
        if (path)
            *path = c.path.c_str();
        return succeed(err);
    });
}

prometheia_status prometheia_engine_catalog_bodies(const prometheia_engine* engine, int* bodies,
                                                   prometheia_error* err) {
    if (!engine)
        return argument(err, "engine is NULL");
    if (!bodies)
        return argument(err, "bodies output is NULL");
    return guarded(err, [&] {
        auto n = engine->engine.catalog_bodies();
        if (!n)
            return report(err, n.error());
        *bodies = int(n.value());
        return succeed(err);
    });
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

prometheia_status prometheia_engine_add_ephemeris(prometheia_engine* engine, const char* path,
                                                  prometheia_error* err) {
    if (!engine)
        return argument(err, "engine is NULL");
    if (!path)
        return argument(err, "ephemeris path is NULL");
    return guarded(err, [&] {
        auto r = engine->engine.add_ephemeris(path);
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

prometheia_status prometheia_calc_houses(prometheia_engine* engine, int system, double jd_tt,
                                         double lon_deg, double lat_deg,
                                         const prometheia_options* opts, prometheia_houses* out,
                                         prometheia_error* err) {
    return houses_common(engine, system, jd_tt, lon_deg, lat_deg, opts, out, err,
                         [](Engine& e, houses::System s, double jd, const frames::GeoSite& site,
                            const CalcOptions& o) { return e.houses(s, jd, site, o); });
}

prometheia_status prometheia_calc_houses_ut(prometheia_engine* engine, int system, double jd_ut1,
                                            double lon_deg, double lat_deg,
                                            const prometheia_options* opts, prometheia_houses* out,
                                            prometheia_error* err) {
    return houses_common(engine, system, jd_ut1, lon_deg, lat_deg, opts, out, err,
                         [](Engine& e, houses::System s, double jd, const frames::GeoSite& site,
                            const CalcOptions& o) { return e.houses_ut(s, jd, site, o); });
}

prometheia_status prometheia_houses_from_armc(int system, double armc_deg, double obliquity_deg,
                                              double lat_deg, prometheia_houses* out,
                                              prometheia_error* err) {
    if (out)
        std::memset(out, 0, sizeof *out);
    if (!out)
        return argument(err, "houses output is NULL");
    houses::System s{};
    if (prometheia_status st; !house_system(system, s, err, st))
        return st;
    return guarded(err, [&] {
        auto h = houses::compute(s, armc_deg * kDegToRad, obliquity_deg * kDegToRad,
                                 lat_deg * kDegToRad);
        if (!h)
            return report(err, h.error());
        fill(h.value(), *out);
        out->obliquity_deg = obliquity_deg;
        return succeed(err);
    });
}

prometheia_status prometheia_sidereal_time_ut(prometheia_engine* engine, double jd_ut1, int kind,
                                              const prometheia_options* opts, double* out_deg,
                                              prometheia_error* err) {
    if (out_deg)
        *out_deg = 0.0;
    if (!engine)
        return argument(err, "engine is NULL");
    if (!out_deg)
        return argument(err, "sidereal time output is NULL");
    if (kind != PROMETHEIA_SIDEREAL_TIME_MEAN && kind != PROMETHEIA_SIDEREAL_TIME_APPARENT)
        return argument(err, "sidereal time kind out of range");
    Precession p{};
    if (prometheia_status st; !precession_of(opts, p, err, st))
        return st;
    return guarded(err, [&] {
        auto r =
            engine->engine.sidereal_time_ut(jd_ut1, kind == PROMETHEIA_SIDEREAL_TIME_APPARENT, p);
        if (!r)
            return report(err, r.error());
        *out_deg = r.value();
        return succeed(err);
    });
}

prometheia_status prometheia_obliquity(prometheia_engine* engine, double jd_tt, int kind,
                                       const prometheia_options* opts, double* out_deg,
                                       prometheia_error* err) {
    if (out_deg)
        *out_deg = 0.0;
    if (!engine)
        return argument(err, "engine is NULL");
    if (!out_deg)
        return argument(err, "obliquity output is NULL");
    if (kind != PROMETHEIA_OBLIQUITY_MEAN && kind != PROMETHEIA_OBLIQUITY_TRUE)
        return argument(err, "obliquity kind out of range");
    Precession p{};
    if (prometheia_status st; !precession_of(opts, p, err, st))
        return st;
    return guarded(err, [&] {
        auto r = engine->engine.obliquity(jd_tt, kind == PROMETHEIA_OBLIQUITY_TRUE, p);
        if (!r)
            return report(err, r.error());
        *out_deg = r.value();
        return succeed(err);
    });
}

void prometheia_ecliptic_to_equatorial(double lon_deg, double lat_deg, double obliquity_deg,
                                       double* ra_deg, double* dec_deg) {
    double ra = 0.0, dec = 0.0;
    frames::ecliptic_to_equatorial(lon_deg * kDegToRad, lat_deg * kDegToRad,
                                   obliquity_deg * kDegToRad, ra, dec);
    if (ra_deg)
        *ra_deg = ra / kDegToRad;
    if (dec_deg)
        *dec_deg = dec / kDegToRad;
}

void prometheia_equatorial_to_ecliptic(double ra_deg, double dec_deg, double obliquity_deg,
                                       double* lon_deg, double* lat_deg) {
    double lon = 0.0, lat = 0.0;
    frames::equatorial_to_ecliptic(ra_deg * kDegToRad, dec_deg * kDegToRad,
                                   obliquity_deg * kDegToRad, lon, lat);
    if (lon_deg)
        *lon_deg = lon / kDegToRad;
    if (lat_deg)
        *lat_deg = lat / kDegToRad;
}

void prometheia_atmosphere_init(prometheia_atmosphere* atmosphere) {
    if (!atmosphere)
        return;
    const frames::Atmosphere d;
    atmosphere->pressure_hpa = d.pressure_hpa;
    atmosphere->temperature_c = d.temperature_c;
}

prometheia_status prometheia_horizontal_ut(prometheia_engine* engine, double jd_ut1, double lon_deg,
                                           double lat_deg, double height_m, double ra_deg,
                                           double dec_deg, const prometheia_atmosphere* atmosphere,
                                           const prometheia_options* opts,
                                           prometheia_horizontal* out, prometheia_error* err) {
    if (out)
        std::memset(out, 0, sizeof *out);
    if (!engine)
        return argument(err, "engine is NULL");
    if (!out)
        return argument(err, "horizontal output is NULL");
    Precession p{};
    if (prometheia_status st; !precession_of(opts, p, err, st))
        return st;
    return guarded(err, [&] {
        const frames::GeoSite site{lon_deg * kDegToRad, lat_deg * kDegToRad, height_m};
        frames::Atmosphere air;
        if (atmosphere)
            air = {atmosphere->pressure_hpa, atmosphere->temperature_c};
        auto r = engine->engine.horizontal_ut(jd_ut1, site, ra_deg, dec_deg,
                                              atmosphere ? &air : nullptr, p);
        if (!r)
            return report(err, r.error());
        out->azimuth_deg = r.value().azimuth_deg;
        out->altitude_deg = r.value().altitude_deg;
        out->apparent_altitude_deg = r.value().apparent_altitude_deg;
        out->hour_angle_deg = r.value().hour_angle_deg;
        return succeed(err);
    });
}

prometheia_status prometheia_equatorial_from_horizontal_ut(
    prometheia_engine* engine, double jd_ut1, double lon_deg, double lat_deg, double height_m,
    double azimuth_deg, double altitude_deg, int apparent, const prometheia_atmosphere* atmosphere,
    const prometheia_options* opts, double* ra_deg, double* dec_deg, prometheia_error* err) {
    if (ra_deg)
        *ra_deg = 0.0;
    if (dec_deg)
        *dec_deg = 0.0;
    if (!engine)
        return argument(err, "engine is NULL");
    if (!ra_deg || !dec_deg)
        return argument(err, "equatorial outputs are NULL");
    Precession p{};
    if (prometheia_status st; !precession_of(opts, p, err, st))
        return st;
    return guarded(err, [&] {
        const frames::GeoSite site{lon_deg * kDegToRad, lat_deg * kDegToRad, height_m};
        frames::Atmosphere air;
        if (atmosphere)
            air = {atmosphere->pressure_hpa, atmosphere->temperature_c};
        auto r = engine->engine.equatorial_from_horizontal_ut(
            jd_ut1, site, azimuth_deg, altitude_deg, apparent != 0, atmosphere ? &air : nullptr, p);
        if (!r)
            return report(err, r.error());
        *ra_deg = r.value().ra_deg;
        *dec_deg = r.value().dec_deg;
        return succeed(err);
    });
}

prometheia_status prometheia_refraction(double altitude_deg, int from,
                                        const prometheia_atmosphere* atmosphere, double height_m,
                                        double* out_deg, prometheia_error* err) {
    if (out_deg)
        *out_deg = 0.0;
    if (!out_deg)
        return argument(err, "refraction output is NULL");
    if (from != PROMETHEIA_REFRACTION_FROM_TRUE && from != PROMETHEIA_REFRACTION_FROM_APPARENT)
        return argument(err, "refraction direction out of range");
    if (!std::isfinite(altitude_deg) || std::fabs(altitude_deg) > 90.0 || !std::isfinite(height_m))
        return argument(err, "refraction needs a finite altitude within -90..90 and height");
    frames::Atmosphere air;
    if (atmosphere)
        air = {atmosphere->pressure_hpa, atmosphere->temperature_c};
    if (!std::isfinite(air.pressure_hpa) || !std::isfinite(air.temperature_c) ||
        air.pressure_hpa < 0.0 || air.temperature_c <= -273.0)
        return argument(err, "refraction needs a pressure of 0 hPa or more and a temperature "
                             "above -273 C");
    const double dip = frames::horizon_dip_deg(height_m);
    *out_deg = from == PROMETHEIA_REFRACTION_FROM_TRUE
                   ? frames::refraction_from_true_deg(altitude_deg, air, dip)
                   : frames::refraction_from_apparent_deg(altitude_deg, air, dip);
    return succeed(err);
}

void prometheia_rise_set_options_init(prometheia_rise_set_options* options) {
    if (!options)
        return;
    const Engine::RiseSetOptions d;
    options->limb = d.limb;
    options->refraction = d.refraction;
    options->atmosphere.pressure_hpa = d.atmosphere.pressure_hpa;
    options->atmosphere.temperature_c = d.atmosphere.temperature_c;
    options->dip_height_m = d.dip_height_m;
    options->window_days = d.window_days;
    options->backward = d.backward;
}

namespace {

// A C object -> the engine's ObjectRef; a registry id is resolved.
prometheia_status to_ref(prometheia_engine* engine, const prometheia_object& object,
                         Engine::ObjectRef& ref, prometheia_error* err) {
    switch (object.kind) {
    case PROMETHEIA_OBJECT_BODY:
        ref.kind = Engine::ObjectRef::Kind::Body;
        ref.id = object.id;
        return PROMETHEIA_OK;
    case PROMETHEIA_OBJECT_STAR:
        ref.kind = Engine::ObjectRef::Kind::Star;
        ref.id = object.id;
        return PROMETHEIA_OK;
    case PROMETHEIA_OBJECT_HYPOTHETICAL:
        if (!object.token)
            return argument(err, "a hypothetical object needs a token");
        ref.kind = Engine::ObjectRef::Kind::Hypothetical;
        ref.token = object.token;
        return PROMETHEIA_OK;
    case PROMETHEIA_OBJECT_ORBIT_POINT: {
        if (!object.token)
            return argument(err, "an orbit point needs a token, \"<point>:<method>\"");
        // The registry's own syntax, so that both spell a point one way.
        auto parsed =
            registry::parse_id("orbit:naif:" + std::to_string(object.id) + ":" + object.token);
        if (!parsed)
            return report(err, parsed.error());
        ref.kind = Engine::ObjectRef::Kind::OrbitPoint;
        ref.id = object.id;
        ref.point = parsed.value().point;
        ref.method = parsed.value().method;
        return PROMETHEIA_OK;
    }
    case PROMETHEIA_OBJECT_ID: {
        if (!object.token)
            return argument(err, "an object id needs a token");
        auto r = engine->registry().resolve(object.token);
        if (!r)
            return report(err, r.error());
        ref = std::move(r).value();
        return PROMETHEIA_OK;
    }
    default:
        return argument(err, "object kind out of range");
    }
}

} // namespace

prometheia_status prometheia_rise_set_ut(prometheia_engine* engine, const prometheia_object* object,
                                         int event, double jd_ut1_start, double lon_deg,
                                         double lat_deg, double height_m,
                                         const prometheia_rise_set_options* options,
                                         const prometheia_options* opts, prometheia_event* out,
                                         prometheia_error* err) {
    if (out) {
        out->jd_ut1 = std::nan("");
        out->outcome = PROMETHEIA_OUTCOME_NOT_IN_WINDOW;
    }
    if (!engine)
        return argument(err, "engine is NULL");
    if (!object)
        return argument(err, "object is NULL");
    if (!out)
        return argument(err, "event output is NULL");
    if (event < PROMETHEIA_EVENT_RISE || event > PROMETHEIA_EVENT_LOWER_TRANSIT)
        return argument(err, "event out of range");
    Precession p{};
    if (prometheia_status st; !precession_of(opts, p, err, st))
        return st;
    return guarded(err, [&] {
        Engine::ObjectRef ref;
        if (prometheia_status st = to_ref(engine, *object, ref, err); st != PROMETHEIA_OK)
            return st;
        Engine::RiseSetOptions ro;
        if (options) {
            ro.limb = options->limb;
            ro.refraction = options->refraction != 0;
            ro.atmosphere = {options->atmosphere.pressure_hpa, options->atmosphere.temperature_c};
            ro.dip_height_m = options->dip_height_m;
            ro.window_days = options->window_days;
            ro.backward = options->backward != 0;
        }
        ro.precession = p;
        const frames::GeoSite site{lon_deg * kDegToRad, lat_deg * kDegToRad, height_m};
        auto r = engine->engine.rise_set_ut(ref, static_cast<Engine::Event>(event), jd_ut1_start,
                                            site, ro);
        if (!r)
            return report(err, r.error());
        out->jd_ut1 = r.value().jd_ut1;
        out->outcome = int(r.value().outcome);
        return succeed(err);
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

double prometheia_engine_delta_t(const prometheia_engine* engine, double jd_tt) {
    if (!engine)
        return std::nan("");
    return engine->engine.delta_t_seconds(jd_tt);
}

double prometheia_delta_t(double jd_tt) {
    return time::delta_t(jd_tt);
}

double prometheia_tdb_minus_tt(double jd_tt) {
    return time::tdb_minus_tt(jd_tt);
}

// ---- phenomena -------------------------------------------------------------

extern "C++" {
namespace {

template <typename Call>
prometheia_status phenomena_common(prometheia_engine* engine, const prometheia_object* object,
                                   double jd, const prometheia_options* opts,
                                   prometheia_phenomena* out, prometheia_error* err, Call call) {
    if (out) {
        std::memset(out, 0, sizeof *out);
        out->phase_angle_deg = out->illuminated_fraction = out->elongation_deg =
            out->diameter_arcsec = out->magnitude = std::nan("");
    }
    if (!engine)
        return argument(err, "engine is NULL");
    if (!object)
        return argument(err, "object is NULL");
    if (!out)
        return argument(err, "phenomena output is NULL");
    return guarded(err, [&] {
        Engine::ObjectRef ref;
        if (prometheia_status st = to_ref(engine, *object, ref, err); st != PROMETHEIA_OK)
            return st;
        CalcOptions o;
        if (opts) {
            if (prometheia_status s = translate(*opts, o, err); s != PROMETHEIA_OK)
                return s;
        }
        auto r = call(engine->engine, ref, jd, o);
        if (!r)
            return report(err, r.error());
        const Engine::Phenomena& p = r.value();
        out->phase_angle_deg = p.phase_angle_deg;
        out->illuminated_fraction = p.illuminated_fraction;
        out->elongation_deg = p.elongation_deg;
        out->diameter_arcsec = p.diameter_arcsec;
        out->magnitude = p.magnitude;
        copy_string(out->magnitude_model, sizeof out->magnitude_model, p.magnitude_model);
        return succeed(err);
    });
}

} // namespace
} // extern "C++"

prometheia_status prometheia_calc_phenomena(prometheia_engine* engine,
                                            const prometheia_object* object, double jd_tt,
                                            const prometheia_options* opts,
                                            prometheia_phenomena* out, prometheia_error* err) {
    return phenomena_common(engine, object, jd_tt, opts, out, err,
                            [](Engine& e, const Engine::ObjectRef& ref, double jd,
                               const CalcOptions& o) { return e.phenomena(ref, jd, o); });
}

prometheia_status prometheia_calc_phenomena_ut(prometheia_engine* engine,
                                               const prometheia_object* object, double jd_ut1,
                                               const prometheia_options* opts,
                                               prometheia_phenomena* out, prometheia_error* err) {
    return phenomena_common(engine, object, jd_ut1, opts, out, err,
                            [](Engine& e, const Engine::ObjectRef& ref, double jd,
                               const CalcOptions& o) { return e.phenomena_ut(ref, jd, o); });
}

// ---- the object registry ---------------------------------------------------

static_assert(PROMETHEIA_KIND_COMET == int(registry::Kind::Comet));
static_assert(PROMETHEIA_STANDING_DEFINED == int(registry::Standing::Defined));

void prometheia_object_filter_init(prometheia_object_filter* filter) {
    if (!filter)
        return;
    std::memset(filter, 0, sizeof *filter);
    filter->h_max = std::nan("");
    filter->vmag_max = std::nan("");
}

prometheia_status prometheia_object_info(prometheia_engine* engine, const char* id,
                                         prometheia_object_entry* info, prometheia_error* err) {
    if (info)
        std::memset(info, 0, sizeof *info);
    if (!engine)
        return argument(err, "engine is NULL");
    if (!id)
        return argument(err, "id is NULL");
    if (!info)
        return argument(err, "info output is NULL");
    return guarded(err, [&] {
        auto r = engine->registry().info(id);
        if (!r)
            return report(err, r.error());
        const registry::Info& i = r.value();
        copy_string(info->id, sizeof info->id, i.id);
        info->kind = int(i.kind);
        copy_string(info->orbit_class, sizeof info->orbit_class, i.orbit_class);
        copy_string(info->name, sizeof info->name, i.name);
        info->standing = int(i.standing);
        copy_string(info->standing_source, sizeof info->standing_source, i.standing_source);
        info->number = i.number;
        copy_string(info->designation, sizeof info->designation, i.designation);
        copy_string(info->provisional_designation, sizeof info->provisional_designation,
                    i.provisional_designation);
        info->h_mag = i.h_mag;
        info->g_slope = i.g_slope;
        info->diameter_km = i.diameter_km;
        info->vmag = i.vmag;
        info->elements_epoch_jd = i.elements_epoch_jd;
        info->mean_motion_deg_day = i.mean_motion_deg_day;
        info->sidereal_mean_motion_deg_day = i.sidereal_mean_motion_deg_day;
        info->period_days = i.period_days;
        copy_string(info->source, sizeof info->source, i.source);
        info->object_id = int(i.ref.id);
        switch (i.ref.kind) {
        case Engine::ObjectRef::Kind::Body:
            info->object_kind = PROMETHEIA_OBJECT_BODY;
            break;
        case Engine::ObjectRef::Kind::Star:
            info->object_kind = PROMETHEIA_OBJECT_STAR;
            break;
        case Engine::ObjectRef::Kind::Hypothetical:
            info->object_kind = PROMETHEIA_OBJECT_HYPOTHETICAL;
            copy_string(info->object_token, sizeof info->object_token, i.ref.token);
            break;
        case Engine::ObjectRef::Kind::OrbitPoint: {
            info->object_kind = PROMETHEIA_OBJECT_ORBIT_POINT;
            // The id's last two fields are the point and the method.
            const size_t m = i.id.rfind(':');
            const size_t p = i.id.rfind(':', m - 1);
            copy_string(info->object_token, sizeof info->object_token, i.id.substr(p + 1));
            break;
        }
        }
        return succeed(err);
    });
}

namespace {

void fill_match(const registry::Match& m, prometheia_object_match& out) {
    std::memset(&out, 0, sizeof out);
    copy_string(out.id, sizeof out.id, m.id);
    out.kind = int(m.kind);
    switch (m.quality) {
    case registry::MatchQuality::Exact:
        out.quality = PROMETHEIA_MATCH_EXACT;
        break;
    case registry::MatchQuality::Alias:
        out.quality = PROMETHEIA_MATCH_ALIAS;
        break;
    case registry::MatchQuality::OldDesignation:
        out.quality = PROMETHEIA_MATCH_OLD_DESIGNATION;
        break;
    case registry::MatchQuality::Prefix:
        out.quality = PROMETHEIA_MATCH_PREFIX;
        break;
    }
    copy_string(out.name, sizeof out.name, m.name);
    copy_string(out.matched, sizeof out.matched, m.matched);
}

} // namespace

prometheia_status prometheia_object_lookup(prometheia_engine* engine, const char* query, int flags,
                                           prometheia_object_match* matches, int max, int* count,
                                           prometheia_error* err) {
    if (count)
        *count = 0;
    if (!engine)
        return argument(err, "engine is NULL");
    if (!query)
        return argument(err, "query is NULL");
    if (max < 0 || (max > 0 && !matches))
        return argument(err, "matches needs room for max entries");
    return guarded(err, [&] {
        auto r =
            engine->registry().lookup(query, (flags & PROMETHEIA_LOOKUP_PREFIX) != 0, size_t(max));
        if (!r)
            return report(err, r.error());
        const auto& v = r.value();
        for (size_t k = 0; k < v.size(); ++k)
            fill_match(v[k], matches[k]);
        if (count)
            *count = int(v.size());
        return succeed(err);
    });
}

prometheia_status prometheia_object_list(prometheia_engine* engine,
                                         const prometheia_object_filter* filter,
                                         unsigned long long* cursor,
                                         prometheia_object_match* objects, int max, int* count,
                                         prometheia_error* err) {
    if (count)
        *count = 0;
    if (!engine)
        return argument(err, "engine is NULL");
    if (!cursor)
        return argument(err, "cursor is NULL");
    if (max < 0 || (max > 0 && !objects))
        return argument(err, "objects needs room for max entries");
    return guarded(err, [&] {
        registry::Filter f;
        if (filter) {
            f.kinds = filter->kinds;
            f.orbit_class = std::string(filter->orbit_class,
                                        strnlen(filter->orbit_class, sizeof filter->orbit_class));
            f.named_only = filter->named_only != 0;
            f.numbered_only = filter->numbered_only != 0;
            f.h_max = filter->h_max;
            f.vmag_max = filter->vmag_max;
        }
        auto r = engine->registry().list(f, *cursor, size_t(max));
        if (!r)
            return report(err, r.error());
        const auto& v = r.value().objects;
        for (size_t k = 0; k < v.size(); ++k)
            fill_match(v[k], objects[k]);
        if (count)
            *count = int(v.size());
        *cursor = r.value().next;
        return succeed(err);
    });
}

prometheia_status prometheia_calc_object(prometheia_engine* engine, const prometheia_object* object,
                                         double jd_tt, const prometheia_options* opts,
                                         prometheia_result* result, prometheia_error* err) {
    if (!object) {
        if (result)
            std::memset(result, 0, sizeof *result);
        return argument(err, "object is NULL");
    }
    Engine::ObjectRef ref;
    if (engine) {
        if (prometheia_status st = guarded(err, [&] { return to_ref(engine, *object, ref, err); });
            st != PROMETHEIA_OK) {
            if (result)
                std::memset(result, 0, sizeof *result);
            return st;
        }
    }
    return calc_common(engine, 0, jd_tt, opts, result, err,
                       [&ref](Engine& e, int, double jd, const CalcOptions& o) {
                           return e.calc_object(ref, jd, o);
                       });
}

prometheia_status prometheia_calc_object_ut(prometheia_engine* engine,
                                            const prometheia_object* object, double jd_ut1,
                                            const prometheia_options* opts,
                                            prometheia_result* result, prometheia_error* err) {
    if (!object) {
        if (result)
            std::memset(result, 0, sizeof *result);
        return argument(err, "object is NULL");
    }
    Engine::ObjectRef ref;
    if (engine) {
        if (prometheia_status st = guarded(err, [&] { return to_ref(engine, *object, ref, err); });
            st != PROMETHEIA_OK) {
            if (result)
                std::memset(result, 0, sizeof *result);
            return st;
        }
    }
    return calc_common(engine, 0, jd_ut1, opts, result, err,
                       [&ref](Engine& e, int, double jd, const CalcOptions& o) {
                           return e.calc_object_ut(ref, jd, o);
                       });
}

// ---- Lunar eclipses ------------------------------------------------------

static_assert(PROMETHEIA_LUNAR_ECLIPSE_TOTAL == int(lunar_eclipse::kTotal));
static_assert(PROMETHEIA_LUNAR_ECLIPSE_PARTIAL == int(lunar_eclipse::kPartial));
static_assert(PROMETHEIA_LUNAR_ECLIPSE_PENUMBRAL == int(lunar_eclipse::kPenumbral));
static_assert(PROMETHEIA_LUNAR_CONTACT_MAXIMUM == lunar_eclipse::kMaximum);

static void fill_lunar_eclipse(prometheia_lunar_eclipse& o, const lunar_eclipse::Eclipse& e) {
    std::memset(&o, 0, sizeof o);
    o.kind = e.kind == lunar_eclipse::Kind::Total     ? PROMETHEIA_LUNAR_ECLIPSE_TOTAL
             : e.kind == lunar_eclipse::Kind::Partial ? PROMETHEIA_LUNAR_ECLIPSE_PARTIAL
                                                      : PROMETHEIA_LUNAR_ECLIPSE_PENUMBRAL;
    o.saros_series = e.saros_series;
    o.saros_member = e.saros_member;
    o.jd_max_tt = e.jd_max_tt;
    o.jd_max_ut1 = e.jd_max_ut1;
    o.gamma = e.gamma;
    o.umbral_magnitude = e.umbral_magnitude;
    o.penumbral_magnitude = e.penumbral_magnitude;
    o.umbra_radius_deg = e.umbra_radius_deg;
    o.penumbra_radius_deg = e.penumbra_radius_deg;
    o.moon_semidiameter_deg = e.moon_semidiameter_deg;
    for (int i = 0; i < 6; ++i) {
        o.contact_tt[i] = e.contact_tt[i];
        o.contact_ut1[i] = e.contact_ut1[i];
    }
}

prometheia_status prometheia_lunar_eclipse_next(prometheia_engine* engine, double jd_start_tt,
                                                unsigned kinds, int backward,
                                                prometheia_lunar_eclipse* out,
                                                prometheia_error* err) {
    if (out)
        std::memset(out, 0, sizeof *out);
    if (!engine)
        return argument(err, "engine is NULL");
    if (!out)
        return argument(err, "output is NULL");
    return guarded(err, [&] {
        auto r = lunar_eclipse::next(engine->engine, jd_start_tt, kinds, backward != 0);
        if (!r)
            return report(err, r.error());
        fill_lunar_eclipse(*out, r.value());
        return succeed(err);
    });
}

prometheia_status prometheia_lunar_eclipse_at(prometheia_engine* engine, double jd_max_tt,
                                              const prometheia_site* site,
                                              prometheia_lunar_eclipse_local* out,
                                              prometheia_error* err) {
    if (out)
        std::memset(out, 0, sizeof *out);
    if (!engine)
        return argument(err, "engine is NULL");
    if (!out)
        return argument(err, "output is NULL");
    return guarded(err, [&] {
        if (!site) {
            auto r = lunar_eclipse::at(engine->engine, jd_max_tt);
            if (!r)
                return report(err, r.error());
            fill_lunar_eclipse(out->eclipse, r.value());
            for (int i = 0; i < 7; ++i)
                out->altitude_deg[i] = out->apparent_altitude_deg[i] = std::nan("");
            return succeed(err);
        }
        const frames::GeoSite gs{site->lon_deg * kDegToRad, site->lat_deg * kDegToRad,
                                 site->height_m};
        auto r = lunar_eclipse::local(engine->engine, jd_max_tt, gs);
        if (!r)
            return report(err, r.error());
        fill_lunar_eclipse(out->eclipse, r.value().eclipse);
        for (int i = 0; i < 7; ++i) {
            out->altitude_deg[i] = r.value().altitude_deg[i];
            out->apparent_altitude_deg[i] = r.value().apparent_altitude_deg[i];
        }
        return succeed(err);
    });
}

} // extern "C"

// ---- Solar eclipses (docs/SOLAR_ECLIPSES.md) --------------------------------

#include "prometheia/solar_eclipse.hpp"

namespace {

namespace se = prometheia::solar_eclipse;

bool solar_options(prometheia_engine* engine, const prometheia_solar_eclipse_options* in,
                   se::Options& out, prometheia_error* err, prometheia_status& st) {
    if (in) {
        if (!(in->k_umbra > 0.0 && in->k_umbra < 1.0 && in->k_penumbra > 0.0 &&
              in->k_penumbra < 1.0 && in->sun_radius_km > 0.0)) {
            st = argument(err,
                          "lunar radius ratios must be in (0, 1) and the Sun's radius positive");
            return false;
        }
        out.k_umbra = in->k_umbra;
        out.k_penumbra = in->k_penumbra;
        out.sun_radius_km = in->sun_radius_km;
    }
    // The engine's Delta T, so the Earth's rotation here is the one calc_ut uses.
    out.delta_t = engine->delta_t.fn ? &engine->delta_t : nullptr;
    return true;
}

void fill_besselian(const se::Besselian& b, prometheia_besselian& o) {
    o.x = b.x;
    o.y = b.y;
    o.d_deg = b.d_rad / kDegToRad;
    o.mu_deg = b.mu_rad / kDegToRad;
    o.l1 = b.l1;
    o.l2 = b.l2;
    o.tan_f1 = b.tan_f1;
    o.tan_f2 = b.tan_f2;
}

} // namespace

extern "C" {

void prometheia_solar_eclipse_options_init(prometheia_solar_eclipse_options* opts) {
    if (!opts)
        return;
    const se::Options d;
    opts->k_umbra = d.k_umbra;
    opts->k_penumbra = d.k_penumbra;
    opts->sun_radius_km = d.sun_radius_km;
}

prometheia_status prometheia_solar_eclipse_next_ex(prometheia_engine* engine, double jd_start_tt,
                                                   unsigned kinds, int backward,
                                                   const prometheia_solar_eclipse_options* options,
                                                   prometheia_solar_eclipse* out,
                                                   prometheia_error* err) {
    if (out)
        std::memset(out, 0, sizeof *out);
    if (!engine)
        return argument(err, "engine is NULL");
    if (!out)
        return argument(err, "output is NULL");
    if (kinds > unsigned(PROMETHEIA_SOLAR_ALL_KINDS))
        return argument(err, "kinds out of range");
    se::Options o;
    prometheia_status st = PROMETHEIA_OK;
    if (!solar_options(engine, options, o, err, st))
        return st;
    return guarded(err, [&] {
        auto r = se::next_global(engine->engine, jd_start_tt, unsigned(kinds), backward != 0, o);
        if (!r.ok())
            return report(err, r.error());
        const se::Global& g = r.value();
        out->type = int(g.type);
        out->saros = g.saros;
        out->lunation = g.lunation;
        out->max_tt = g.max_tt;
        out->max_ut1 = g.max_ut1;
        out->gamma = g.gamma;
        out->magnitude = g.magnitude;
        out->magnitude_covered = g.magnitude_covered;
        out->obscuration = g.obscuration;
        out->lat_deg = g.greatest.lat_deg;
        out->lon_deg = g.greatest.lon_deg;
        out->sun_alt_deg = g.sun_alt_deg;
        out->sun_az_deg = g.sun_az_deg;
        out->path_width_km = g.path_width_km;
        out->central_duration_s = g.central_duration_s;
        out->p1_tt = g.p1_tt;
        out->u1_tt = g.u1_tt;
        out->c1_tt = g.c1_tt;
        out->c2_tt = g.c2_tt;
        out->u4_tt = g.u4_tt;
        out->p4_tt = g.p4_tt;
        fill_besselian(g.elements, out->elements);
        return succeed(err);
    });
}

prometheia_status prometheia_solar_eclipse_next(prometheia_engine* engine, double jd_start_tt,
                                                unsigned kinds, int backward,
                                                prometheia_solar_eclipse* out,
                                                prometheia_error* err) {
    return prometheia_solar_eclipse_next_ex(engine, jd_start_tt, kinds, backward, nullptr, out,
                                            err);
}

prometheia_status prometheia_solar_eclipse_where_ex(prometheia_engine* engine, double jd_tt,
                                                    const prometheia_solar_eclipse_options* options,
                                                    prometheia_solar_where* out,
                                                    prometheia_error* err) {
    if (out)
        std::memset(out, 0, sizeof *out);
    if (!engine)
        return argument(err, "engine is NULL");
    if (!out)
        return argument(err, "output is NULL");
    if (!std::isfinite(jd_tt))
        return argument(err, "jd is not finite");
    se::Options o;
    prometheia_status st = PROMETHEIA_OK;
    if (!solar_options(engine, options, o, err, st))
        return st;
    return guarded(err, [&] {
        auto r = se::where(engine->engine, jd_tt, o);
        if (!r.ok())
            return report(err, r.error());
        const se::Where& w = r.value();
        out->jd_tt = w.jd_tt;
        out->jd_ut1 = w.jd_ut1;
        out->axis_hits = w.axis_hits ? 1 : 0;
        out->type = int(w.type);
        out->lat_deg = w.central.lat_deg;
        out->lon_deg = w.central.lon_deg;
        out->north_lat_deg = w.north_limit.lat_deg;
        out->north_lon_deg = w.north_limit.lon_deg;
        out->south_lat_deg = w.south_limit.lat_deg;
        out->south_lon_deg = w.south_limit.lon_deg;
        out->sun_alt_deg = w.sun_alt_deg;
        out->path_width_km = w.path_width_km;
        out->central_duration_s = w.central_duration_s;
        out->magnitude = w.magnitude;
        out->obscuration = w.obscuration;
        fill_besselian(w.elements, out->elements);
        return succeed(err);
    });
}

prometheia_status prometheia_solar_eclipse_where(prometheia_engine* engine, double jd_tt,
                                                 prometheia_solar_where* out,
                                                 prometheia_error* err) {
    return prometheia_solar_eclipse_where_ex(engine, jd_tt, nullptr, out, err);
}

prometheia_status
prometheia_solar_eclipse_local_next_ex(prometheia_engine* engine, double jd_start_tt,
                                       const prometheia_site* site, int backward,
                                       const prometheia_solar_eclipse_options* options,
                                       prometheia_solar_local* out, prometheia_error* err) {
    if (out)
        std::memset(out, 0, sizeof *out);
    if (!engine)
        return argument(err, "engine is NULL");
    if (!site)
        return argument(err, "site is NULL");
    if (!out)
        return argument(err, "output is NULL");
    if (!std::isfinite(jd_start_tt))
        return argument(err, "jd is not finite");
    if (!(std::fabs(site->lat_deg) <= 90.0) || !std::isfinite(site->lon_deg) ||
        !std::isfinite(site->height_m))
        return argument(err, "site out of range");
    se::Options o;
    prometheia_status st = PROMETHEIA_OK;
    if (!solar_options(engine, options, o, err, st))
        return st;
    return guarded(err, [&] {
        const frames::GeoSite gs{site->lon_deg * kDegToRad, site->lat_deg * kDegToRad,
                                 site->height_m};
        auto r = se::local_next(engine->engine, jd_start_tt, gs, backward != 0, o);
        if (!r.ok())
            return report(err, r.error());
        const se::Local& l = r.value();
        out->type = int(l.type);
        out->saros = l.saros;
        out->lunation = l.lunation;
        for (int i = 0; i < 4; ++i) {
            out->contact_tt[i] = l.contact_tt[i];
            out->contact_ut1[i] = l.contact_ut1[i];
            out->contact_alt_deg[i] = l.contact_alt_deg[i];
            out->contact_az_deg[i] = l.contact_az_deg[i];
        }
        out->max_tt = l.max_tt;
        out->max_ut1 = l.max_ut1;
        out->max_alt_deg = l.max_alt_deg;
        out->max_az_deg = l.max_az_deg;
        out->visible_max = l.visible_max ? 1 : 0;
        out->magnitude = l.magnitude;
        out->obscuration = l.obscuration;
        out->diameter_ratio = l.diameter_ratio;
        out->duration_s = l.duration_s;
        out->gamma = l.gamma;
        out->global_max_tt = l.global_max_tt;
        return succeed(err);
    });
}

prometheia_status prometheia_solar_eclipse_local_next(prometheia_engine* engine, double jd_start_tt,
                                                      const prometheia_site* site, int backward,
                                                      prometheia_solar_local* out,
                                                      prometheia_error* err) {
    return prometheia_solar_eclipse_local_next_ex(engine, jd_start_tt, site, backward, nullptr, out,
                                                  err);
}

// ---- heliacal visibility -----------------------------------------------------

static_assert(PROMETHEIA_HELIACAL_RISING == int(heliacal::Event::Rising));
static_assert(PROMETHEIA_HELIACAL_SETTING == int(heliacal::Event::Setting));
static_assert(PROMETHEIA_HELIACAL_EVENING_FIRST == int(heliacal::Event::EveningFirst));
static_assert(PROMETHEIA_HELIACAL_MORNING_LAST == int(heliacal::Event::MorningLast));

void prometheia_air_init(prometheia_air* a) {
    if (!a)
        return;
    const heliacal::Atmosphere d;
    *a = prometheia_air{d.pressure_hpa, d.temperature_c, d.relative_humidity_pct,
                        d.extinction_coefficient, d.visual_range_km};
}

void prometheia_observer_init(prometheia_observer* o) {
    if (!o)
        return;
    const heliacal::Observer d;
    *o = prometheia_observer{d.age_years, d.snellen_ratio, d.binocular ? 1 : 0};
}

void prometheia_visibility_model_init(prometheia_visibility_model* m) {
    if (!m)
        return;
    const heliacal::Options d;
    *m = prometheia_visibility_model{PROMETHEIA_VISIBILITY_SCHAEFER,
                                     d.arcus_visionis_deg,
                                     d.object_altitude_deg,
                                     d.detection_margin_mag,
                                     d.moonlight ? 1 : 0,
                                     d.dark_sky_nl,
                                     d.search_days};
}

extern "C++" {
namespace {

// The inputs of the heliacal calls, from their C structs (NULL: the defaults).
struct HeliacalInputs {
    heliacal::Atmosphere atm;
    heliacal::Observer observer;
    heliacal::Options options;
    frames::GeoSite site;
};

bool heliacal_inputs(const prometheia_site* site, const prometheia_air* atm,
                     const prometheia_observer* ob, const prometheia_visibility_model* model,
                     HeliacalInputs& in, prometheia_error* err, prometheia_status& st) {
    if (!site) {
        st = argument(err, "site is NULL");
        return false;
    }
    if (!(std::fabs(site->lat_deg) <= 90.0) || !std::isfinite(site->lon_deg) ||
        !std::isfinite(site->height_m)) {
        st = argument(err, "site out of range");
        return false;
    }
    in.site = frames::GeoSite{site->lon_deg * kDegToRad, site->lat_deg * kDegToRad, site->height_m};
    if (atm) {
        in.atm.pressure_hpa = atm->pressure_hpa;
        in.atm.temperature_c = atm->temperature_c;
        in.atm.relative_humidity_pct = atm->relative_humidity_pct;
        in.atm.extinction_coefficient = atm->extinction_coefficient;
        in.atm.visual_range_km = atm->visual_range_km;
    }
    if (ob) {
        in.observer.age_years = ob->age_years;
        in.observer.snellen_ratio = ob->snellen_ratio;
        in.observer.binocular = ob->binocular != 0;
    }
    if (model) {
        if (model->kind != PROMETHEIA_VISIBILITY_SCHAEFER &&
            model->kind != PROMETHEIA_VISIBILITY_ARCUS_VISIONIS) {
            st = argument(err, "unknown visibility model");
            return false;
        }
        if (!std::isfinite(model->detection_margin_mag) || !std::isfinite(model->search_days) ||
            !(model->search_days > 0.0)) {
            st = argument(err, "visibility model out of range");
            return false;
        }
        in.options.model = model->kind == PROMETHEIA_VISIBILITY_ARCUS_VISIONIS
                               ? heliacal::Model::ArcusVisionis
                               : heliacal::Model::Schaefer;
        in.options.arcus_visionis_deg = model->arcus_visionis_deg;
        in.options.object_altitude_deg = model->object_altitude_deg;
        in.options.detection_margin_mag = model->detection_margin_mag;
        in.options.moonlight = model->moonlight != 0;
        in.options.dark_sky_nl = model->dark_sky_nl;
        in.options.search_days = model->search_days;
    }
    return true;
}

void fill_echo(const heliacal::Atmosphere& a, const heliacal::Observer& o,
               const heliacal::Options& m, prometheia_air& oa, prometheia_observer& oo,
               prometheia_visibility_model& om) {
    oa = prometheia_air{a.pressure_hpa, a.temperature_c, a.relative_humidity_pct,
                        a.extinction_coefficient, a.visual_range_km};
    oo = prometheia_observer{o.age_years, o.snellen_ratio, o.binocular ? 1 : 0};
    om = prometheia_visibility_model{m.model == heliacal::Model::ArcusVisionis
                                         ? PROMETHEIA_VISIBILITY_ARCUS_VISIONIS
                                         : PROMETHEIA_VISIBILITY_SCHAEFER,
                                     m.arcus_visionis_deg,
                                     m.object_altitude_deg,
                                     m.detection_margin_mag,
                                     m.moonlight ? 1 : 0,
                                     m.dark_sky_nl,
                                     m.search_days};
}

} // namespace
} // extern "C++"

prometheia_status prometheia_heliacal_next(prometheia_engine* engine,
                                           const prometheia_object* object, double jd_start_tt,
                                           const prometheia_site* site,
                                           const prometheia_air* atmosphere,
                                           const prometheia_observer* observer,
                                           const prometheia_visibility_model* model, int event,
                                           prometheia_heliacal* out, prometheia_error* err) {
    if (out)
        std::memset(out, 0, sizeof *out);
    if (!engine)
        return argument(err, "engine is NULL");
    if (!object)
        return argument(err, "object is NULL");
    if (!out)
        return argument(err, "output is NULL");
    if (!std::isfinite(jd_start_tt))
        return argument(err, "jd is not finite");
    if (event < PROMETHEIA_HELIACAL_RISING || event > PROMETHEIA_HELIACAL_MORNING_LAST)
        return argument(err, "unknown heliacal event");
    HeliacalInputs in;
    prometheia_status st = PROMETHEIA_OK;
    if (!heliacal_inputs(site, atmosphere, observer, model, in, err, st))
        return st;
    return guarded(err, [&] {
        Engine::ObjectRef ref;
        if (prometheia_status s = to_ref(engine, *object, ref, err); s != PROMETHEIA_OK)
            return s;
        auto r = heliacal::next(engine->engine, ref, jd_start_tt, in.site, in.atm, in.observer,
                                heliacal::Event(event), in.options);
        if (!r)
            return report(err, r.error());
        const heliacal::Found& f = r.value();
        out->event = event;
        out->model = model ? model->kind : PROMETHEIA_VISIBILITY_SCHAEFER;
        out->jd_tt = f.jd_tt;
        out->jd_ut1 = f.jd_ut1;
        out->optimum_tt = f.optimum_tt;
        out->optimum_ut1 = f.optimum_ut1;
        out->end_tt = f.end_tt;
        out->end_ut1 = f.end_ut1;
        out->spread_before_days = f.spread_before_days;
        out->spread_after_days = f.spread_after_days;
        const heliacal::Sight& s = f.at_optimum;
        out->magnitude = s.scene.object_v;
        out->object_alt_deg = s.scene.object_alt_deg;
        out->object_az_deg = s.object_az_deg;
        out->sun_depression_deg = s.sun_depression_deg;
        out->sun_separation_deg = s.scene.sun_sep_deg;
        const bool schaefer = f.model == heliacal::Model::Schaefer;
        out->sky_brightness_nl = schaefer ? s.seen.sky_brightness_nl : std::nan("");
        out->extinction_k = schaefer ? s.seen.k.total : std::nan("");
        out->extinction_mag = schaefer ? s.seen.extinction_mag : std::nan("");
        out->limiting_magnitude = schaefer ? s.seen.limit_at_eye_mag : std::nan("");
        out->margin = s.margin;
        fill_echo(f.atmosphere, f.observer, f.options, out->atmosphere, out->observer,
                  out->options);
        return succeed(err);
    });
}

prometheia_status prometheia_visibility_at(prometheia_engine* engine,
                                           const prometheia_object* object, double jd_tt,
                                           const prometheia_site* site,
                                           const prometheia_air* atmosphere,
                                           const prometheia_observer* observer,
                                           const prometheia_visibility_model* model,
                                           prometheia_visibility* out, prometheia_error* err) {
    if (out)
        std::memset(out, 0, sizeof *out);
    if (!engine)
        return argument(err, "engine is NULL");
    if (!object)
        return argument(err, "object is NULL");
    if (!out)
        return argument(err, "output is NULL");
    if (!std::isfinite(jd_tt))
        return argument(err, "jd is not finite");
    HeliacalInputs in;
    prometheia_status st = PROMETHEIA_OK;
    if (!heliacal_inputs(site, atmosphere, observer, model, in, err, st))
        return st;
    return guarded(err, [&] {
        Engine::ObjectRef ref;
        if (prometheia_status s = to_ref(engine, *object, ref, err); s != PROMETHEIA_OK)
            return s;
        auto r = heliacal::sight_at(engine->engine, ref, jd_tt, in.site, in.atm, in.observer,
                                    in.options);
        if (!r)
            return report(err, r.error());
        const heliacal::Sight& s = r.value();
        const double nan = std::nan("");
        const bool schaefer = in.options.model == heliacal::Model::Schaefer;
        out->jd_tt = s.jd_tt;
        out->jd_ut1 = s.jd_ut1;
        out->magnitude = s.scene.object_v;
        out->object_alt_deg = s.scene.object_alt_deg;
        out->object_az_deg = s.object_az_deg;
        out->sun_alt_deg = s.scene.sun_alt_deg;
        out->sun_separation_deg = s.scene.sun_sep_deg;
        out->sky_brightness_nl = schaefer ? s.seen.sky_brightness_nl : nan;
        out->extinction_k = schaefer ? s.seen.k.total : nan;
        out->extinction_rayleigh = schaefer ? s.seen.k.rayleigh : nan;
        out->extinction_ozone = schaefer ? s.seen.k.ozone : nan;
        out->extinction_aerosol = schaefer ? s.seen.k.aerosol : nan;
        out->extinction_mag = schaefer ? s.seen.extinction_mag : nan;
        out->limiting_magnitude_outside = schaefer ? s.seen.limit_outside_mag : nan;
        out->limiting_magnitude = schaefer ? s.seen.limit_at_eye_mag : nan;
        out->margin = schaefer && s.scene.object_alt_deg < 0.0 ? -INFINITY : s.margin;
        out->visible = out->margin > 0.0 ? 1 : 0;
        out->night_vision = schaefer && s.seen.night_vision ? 1 : 0;
        return succeed(err);
    });
}

double prometheia_limiting_magnitude(double sky_brightness_nl,
                                     const prometheia_observer* observer) {
    if (!(sky_brightness_nl > 0.0) || !std::isfinite(sky_brightness_nl))
        return std::nan("");
    heliacal::Observer o;
    if (observer) {
        o.age_years = observer->age_years;
        o.snellen_ratio = observer->snellen_ratio;
        o.binocular = observer->binocular != 0;
    }
    if (!(o.age_years > 0.0) || !(o.snellen_ratio > 0.0))
        return std::nan("");
    return heliacal::limiting_magnitude(sky_brightness_nl, o);
}

} // extern "C"

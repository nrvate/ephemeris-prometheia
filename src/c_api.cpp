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
#include <limits>
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
#include "prometheia/segments.hpp"
#include "prometheia/time.hpp"
#include "segment_roots.hpp"

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

// ---- Segments ---------------------------------------------------------------

// The layout's arrays live here; layout points into them and never changes
// after prometheia_segments_fit returns.
struct prometheia_segments {
    prometheia_segments_layout layout{};
    std::vector<double> bounds, mid, half, coef, dcoef;
    std::vector<int> degree;
    prometheia_options opts{}; // as fitted: where the coordinates are
};

namespace {

constexpr int kSegmentStride = PROMETHEIA_SEGMENTS_MAX_DEGREE + 1;
// The fit aims below the tolerance asked: between its check points a fit can
// land slightly worse than it measured (Venus 1% over at 0.01", docs/C_API.md).
constexpr double kSegmentTargetFraction = 0.8;
// Closer than this to the observer, a direction is noise (docs/C_API.md).
constexpr double kSegmentMinDistanceAu = 1e-9;
// How far inside a cut a piece's fit stops (docs/C_API.md, "Steps").
constexpr double kSegmentCutGap = 1e-8;

// Whether two fits' coordinates are the same: the centre, frame, coords and
// zodiac they were fitted in. The corrections (light time, deflection,
// aberration) are the caller's choice and may differ.
bool same_place(const prometheia_options& a, const prometheia_options& b) {
    if (a.center != b.center || a.frame != b.frame || a.coords != b.coords ||
        a.sidereal != b.sidereal || a.precession != b.precession)
        return false;
    if (a.sidereal != PROMETHEIA_SIDEREAL_TROPICAL && a.sidereal_plane != b.sidereal_plane)
        return false;
    if (a.sidereal == PROMETHEIA_SIDEREAL_USER &&
        (a.sidereal_epoch_jd != b.sidereal_epoch_jd ||
         a.sidereal_ayanamsa_deg != b.sidereal_ayanamsa_deg))
        return false;
    if (a.center == PROMETHEIA_CENTER_TOPOCENTRIC &&
        (a.site_lon_deg != b.site_lon_deg || a.site_lat_deg != b.site_lat_deg ||
         a.site_height_m != b.site_height_m))
        return false;
    if (a.center == PROMETHEIA_CENTER_BODY && a.center_body != b.center_body)
        return false;
    return true;
}

// One term of a root search: handles back to back, inside the window.
prometheia_status check_term(const prometheia_segments* const* hs, int n,
                             const prometheia_options& place, double from, double to,
                             prometheia_error* err) {
    for (int h = 0; h < n; ++h) {
        if (!hs[h])
            return argument(err, "segments roots: a handle is NULL");
        if (!same_place(hs[h]->opts, place))
            return argument(err, "segments roots: the handles were fitted in different "
                                 "coordinates (centre, frame, coords or zodiac)");
        if (h > 0 && hs[h - 1]->layout.jd_tt_end != hs[h]->layout.jd_tt_start)
            return argument(err, "segments roots: a term's handles must be back to back, "
                                 "each ending where the next starts");
    }
    if (!(from >= hs[0]->layout.jd_tt_start && to <= hs[n - 1]->layout.jd_tt_end))
        return report(err, PROMETHEIA_ERROR_COVERAGE,
                      "segments roots: the window is outside the handles' span");
    return PROMETHEIA_OK;
}

// Clenshaw's recurrence over n coefficients, as segments::Segment::position.
double clenshaw(const double* c, int n, double u) {
    double b1 = 0.0, b2 = 0.0;
    for (int k = n - 1; k >= 1; --k) {
        const double b = 2.0 * u * b1 - b2 + c[k];
        b2 = b1;
        b1 = b;
    }
    return u * b1 - b2 + c[0];
}

// Where the engine's answer steps inside (from, to): where a second planetary
// file takes over. That is the only step source today (docs/SEGMENTS.md):
// a feature that adds records valid for a range of dates must add their
// boundaries here in the same change. The observer's reads hand over at the instant the engine
// names another file as the source, found to adjacent doubles by bisection
// from each span edge (the edges are TDB; the engine's own TT -> TDB decides
// the instant). The body's reads hand over one light time later. Files with
// the same description are cuts of one DE and give the same numbers, so a
// handover between them is no step.
std::vector<double> segment_cuts(Engine& e, const Engine::ObjectRef& ref, const CalcOptions& o,
                                 double from, double to) {
    std::vector<double> cuts;
    const std::vector<Engine::EphemerisSpan> spans = e.ephemeris_spans();
    if (spans.size() < 2)
        return cuts;
    CalcOptions geometric;
    geometric.center = Center::Barycentric;
    geometric.light_time = geometric.deflection = geometric.aberration = false;
    geometric.speed = geometric.sigma = false;
    const auto source = [&](double jd, std::string& out) {
        auto c = e.calc(PROMETHEIA_EARTH, jd, geometric);
        if (!c)
            return false;
        out = std::string(c.value().provenance.source);
        return true;
    };
    for (const Engine::EphemerisSpan& sp : spans) {
        for (double edge_tdb : {sp.first_jd_tdb, sp.last_jd_tdb}) {
            const double edge = time::tt_from_tdb(edge_tdb);
            double lo = edge - 1e-6, hi = edge + 1e-6;
            if (!(hi > from && lo < to))
                continue;
            std::string s_lo, s_hi, s_mid;
            if (!source(lo, s_lo) || !source(hi, s_hi) || s_lo == s_hi)
                continue;
            while (std::nextafter(lo, hi) < hi) {
                const double mid = lo + (hi - lo) / 2;
                if (!source(mid, s_mid))
                    break;
                (s_mid == s_lo ? lo : hi) = mid;
            }
            cuts.push_back(hi); // the first instant the new file answers
            auto c = e.calc_object(ref, hi, o);
            if (c && c.value().provenance.light_time_days > 0.0)
                cuts.push_back(hi + c.value().provenance.light_time_days);
        }
    }
    cuts.erase(
        std::remove_if(cuts.begin(), cuts.end(), [&](double t) { return !(t > from && t < to); }),
        cuts.end());
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    return cuts;
}

} // namespace

extern "C" {

prometheia_status prometheia_segments_fit(prometheia_engine* engine,
                                          const prometheia_object* object, double jd_tt_start,
                                          double jd_tt_end, const prometheia_options* opts,
                                          double tolerance_arcsec, prometheia_segments** out,
                                          prometheia_error* err) {
    if (out)
        *out = nullptr;
    if (!engine)
        return argument(err, "engine is NULL");
    if (!object)
        return argument(err, "object is NULL");
    if (!out)
        return argument(err, "out is NULL");
    if (!(tolerance_arcsec > 0.0) || !std::isfinite(tolerance_arcsec))
        return argument(err, "segments: the tolerance must be positive and finite");
    if (!std::isfinite(jd_tt_start) || !std::isfinite(jd_tt_end) || !(jd_tt_end > jd_tt_start))
        return argument(err, "segments: the span must be finite and end after it starts");
    return guarded(err, [&] {
        Engine::ObjectRef ref;
        if (prometheia_status st = to_ref(engine, *object, ref, err); st != PROMETHEIA_OK)
            return st;
        CalcOptions o;
        if (opts) {
            if (prometheia_status st = translate(*opts, o, err); st != PROMETHEIA_OK)
                return st;
        }
        o.speed = true; // the fit's rate residual is measured against the engine's rates
        o.sigma = false;
        Engine& e = engine->engine;
        const segments::Sampler sampler = [&](double jd, double p[3], double v[3]) -> Result<void> {
            auto c = e.calc_object(ref, jd, o);
            if (!c)
                return c.error();
            if (!(c.value().pos.dist_au >= kSegmentMinDistanceAu)) {
                return make_error(
                    ErrorCode::ArgumentError,
                    "segments: the object is at the observer, so it has no direction");
            }
            std::copy_n(c.value().pos.xyz_au, 3, p);
            std::copy_n(c.value().pos.vel_au_day, 3, v);
            return {};
        };
        segments::FitOptions fo;
        fo.target_err_arcsec = kSegmentTargetFraction * tolerance_arcsec;
        fo.max_degree = PROMETHEIA_SEGMENTS_MAX_DEGREE;
        const size_t budget = fo.max_segments;

        // Each piece between two cuts is fitted on its own, so no segment
        // straddles a step; the cuts become bounds exactly.
        std::vector<double> edges = {jd_tt_start};
        for (double c : segment_cuts(e, ref, o, jd_tt_start, jd_tt_end))
            edges.push_back(c);
        edges.push_back(jd_tt_end);
        auto h = std::make_unique<prometheia_segments>();
        std::vector<segments::Segment> all;
        double worst = 0.0;
        bool met = true;
        for (size_t k = 0; k + 1 < edges.size(); ++k) {
            fo.max_segments = budget - all.size();
            if (fo.max_segments == 0)
                return argument(err, ("segments: the span needs more than " +
                                      std::to_string(budget) + " segments")
                                         .c_str());
            // A piece is fitted kSegmentCutGap inside each cut, so no sample
            // lands across a step whose instant is known only to a few ulps
            // (the body's, one light time after the observer's). Its segments
            // still own the instants up to the cut, a gap's width of
            // extrapolation.
            const double a = k == 0 ? edges[k] : edges[k] + kSegmentCutGap;
            const double b = k + 2 == edges.size() ? edges[k + 1] : edges[k + 1] - kSegmentCutGap;
            auto fit = b > a ? segments::fit(sampler, a, b, fo)
                             : segments::fit(sampler, edges[k], edges[k + 1], fo);
            if (!fit)
                return report(err, fit.error());
            h->bounds.push_back(edges[k]);
            for (size_t i = 1; i < fit.value().segments.size(); ++i) {
                const segments::Segment& sg = fit.value().segments[i];
                h->bounds.push_back(sg.mid_jd_tt - sg.half_span_days);
            }
            worst = std::max(worst, fit.value().worst_err_arcsec);
            met = met && fit.value().met_target;
            for (segments::Segment& sg : fit.value().segments)
                all.push_back(std::move(sg));
        }
        h->bounds.push_back(jd_tt_end);
        if (!met) {
            const std::string m = "segments: no fit within " + std::to_string(tolerance_arcsec) +
                                  "\" (aiming at " + std::to_string(fo.target_err_arcsec) +
                                  "\"): the worst segment measured " + std::to_string(worst) +
                                  "\" at the shortest span and highest degree";
            return argument(err, m.c_str());
        }

        const size_t n = all.size();
        h->mid.resize(n);
        h->half.resize(n);
        h->degree.resize(n);
        h->coef.assign(3 * kSegmentStride * n, 0.0);
        h->dcoef.assign(3 * kSegmentStride * n, 0.0);
        double err_rel_dist = 0.0, err_rate = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const segments::Segment& sg = all[i];
            h->mid[i] = sg.mid_jd_tt;
            h->half[i] = sg.half_span_days;
            h->degree[i] = sg.degree;
            const std::vector<double>* comp[3] = {&sg.x, &sg.y, &sg.z};
            for (size_t c = 0; c < 3; ++c) {
                std::copy(comp[c]->begin(), comp[c]->end(), &h->coef[(3 * i + c) * kSegmentStride]);
                const std::vector<double> d = segments::derivative(*comp[c]);
                std::copy(d.begin(), d.end(), &h->dcoef[(3 * i + c) * kSegmentStride]);
            }
            err_rel_dist = std::max(err_rel_dist, sg.err_rel_dist);
            err_rate = std::max(err_rate, sg.err_rate_arcsec_per_day);
        }
        prometheia_segments_layout& l = h->layout;
        l.count = int(n);
        l.stride = kSegmentStride;
        l.jd_tt_start = jd_tt_start;
        l.jd_tt_end = jd_tt_end;
        l.bounds_jd_tt = h->bounds.data();
        l.mid_jd_tt = h->mid.data();
        l.half_span_days = h->half.data();
        l.degree = h->degree.data();
        l.coef = h->coef.data();
        l.dcoef = h->dcoef.data();
        l.tolerance_arcsec = tolerance_arcsec;
        l.err_arcsec = worst;
        l.err_rel_dist = err_rel_dist;
        l.err_rate_arcsec_per_day = err_rate;
        l.coords = opts ? opts->coords : PROMETHEIA_COORDS_ECLIPTIC;
        if (opts)
            h->opts = *opts;
        else
            prometheia_options_init(&h->opts);
        *out = h.release();
        return succeed(err);
    });
}

prometheia_status prometheia_object_coverage(prometheia_engine* engine,
                                             const prometheia_object* object,
                                             const prometheia_options* opts, double* first_jd_tt,
                                             double* last_jd_tt, prometheia_error* err) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (first_jd_tt)
        *first_jd_tt = nan;
    if (last_jd_tt)
        *last_jd_tt = nan;
    if (!engine)
        return argument(err, "engine is NULL");
    if (!object)
        return argument(err, "object is NULL");
    if (!first_jd_tt || !last_jd_tt)
        return argument(err, "first_jd_tt or last_jd_tt is NULL");
    return guarded(err, [&] {
        Engine::ObjectRef ref;
        if (prometheia_status st = to_ref(engine, *object, ref, err); st != PROMETHEIA_OK)
            return st;
        CalcOptions o;
        if (opts) {
            if (prometheia_status st = translate(*opts, o, err); st != PROMETHEIA_OK)
                return st;
        }
        Engine& e = engine->engine;
        // Every answer needs the planetary files, so the instants answered lie
        // within their spans. Spans that overlap or touch make one piece;
        // between pieces nothing is answered, so the gaps are known exactly.
        const std::vector<Engine::EphemerisSpan> spans = e.ephemeris_spans();
        if (spans.empty())
            return report(err, PROMETHEIA_ERROR_COVERAGE, "coverage: no planetary file is loaded");
        std::vector<std::pair<double, double>> pieces;
        for (const Engine::EphemerisSpan& sp : spans)
            pieces.emplace_back(time::tt_from_tdb(sp.first_jd_tdb),
                                time::tt_from_tdb(sp.last_jd_tdb));
        std::sort(pieces.begin(), pieces.end());
        size_t merged = 0;
        for (size_t i = 1; i < pieces.size(); ++i) {
            if (pieces[i].first <= pieces[merged].second)
                pieces[merged].second = std::max(pieces[merged].second, pieces[i].second);
            else
                pieces[++merged] = pieces[i];
        }
        pieces.resize(merged + 1);
        Error last_error = make_error(ErrorCode::CoverageError, "coverage: never answered");
        const auto answered = [&](double jd) {
            auto c = e.calc_object(ref, jd, o);
            if (!c)
                last_error = c.error();
            return c.ok();
        };
        // Probe each piece; the object must be answered in one of them only.
        constexpr int kProbes = 64;
        double probe[kProbes + 1];
        bool ok[kProbes + 1];
        int first = -1, last = -1;
        size_t found = pieces.size();
        for (size_t p = 0; p < pieces.size(); ++p) {
            const auto [lo, hi] = pieces[p];
            double at[kProbes + 1];
            bool hit[kProbes + 1];
            int f = -1, l = -1;
            for (int k = 0; k <= kProbes; ++k) {
                at[k] = k == kProbes ? hi : lo + (hi - lo) * k / kProbes;
                hit[k] = answered(at[k]);
                if (hit[k]) {
                    if (f < 0)
                        f = k;
                    l = k;
                }
            }
            if (f < 0)
                continue;
            if (found != pieces.size())
                return report(err, PROMETHEIA_ERROR_COVERAGE,
                              "coverage: the object is answered in more than one interval (the "
                              "loaded planetary files leave a gap)");
            found = p;
            first = f;
            last = l;
            std::copy(at, at + kProbes + 1, probe);
            std::copy(hit, hit + kProbes + 1, ok);
        }
        if (first < 0)
            return report(err, last_error);
        for (int k = first; k <= last; ++k)
            if (!ok[k])
                return report(err, PROMETHEIA_ERROR_COVERAGE,
                              "coverage: the object is not answered across one interval");
        // Each edge to adjacent doubles, between the last refused probe and
        // the first answered one (or the files' own edge, if answered).
        const auto edge = [&](double bad, double good) {
            while (true) {
                const double mid = bad + (good - bad) / 2;
                if (mid == bad || mid == good)
                    return good;
                (answered(mid) ? good : bad) = mid;
            }
        };
        *first_jd_tt = first == 0 ? probe[0] : edge(probe[first - 1], probe[first]);
        *last_jd_tt = last == kProbes ? probe[kProbes] : edge(probe[last + 1], probe[last]);
        return succeed(err);
    });
}

prometheia_status prometheia_segments_eval(const prometheia_segments* segments, double jd_tt,
                                           prometheia_segments_point* out, prometheia_error* err) {
    if (out)
        std::memset(out, 0, sizeof *out);
    if (!segments)
        return argument(err, "segments is NULL");
    if (!out)
        return argument(err, "out is NULL");
    const prometheia_segments_layout& l = segments->layout;
    if (!(jd_tt >= l.jd_tt_start && jd_tt <= l.jd_tt_end))
        return report(err, PROMETHEIA_ERROR_COVERAGE, "segments: the instant is outside the span");
    // The segment i with bounds[i] <= jd_tt < bounds[i + 1]; the last owns the end.
    const double* first = l.bounds_jd_tt + 1;
    const double* last = l.bounds_jd_tt + l.count;
    const int i = int(std::upper_bound(first, last, jd_tt) - first);
    const double u = (jd_tt - l.mid_jd_tt[i]) / l.half_span_days[i];
    const int n = l.degree[i] + 1;
    double p[3], v[3];
    for (int c = 0; c < 3; ++c) {
        const size_t at = size_t(3 * i + c) * size_t(l.stride);
        p[c] = clenshaw(l.coef + at, n, u);
        v[c] = clenshaw(l.dcoef + at, n, u) / l.half_span_days[i];
    }
    constexpr double kR = 180.0 / 3.14159265358979323846;
    const double x = p[0], y = p[1], z = p[2], vx = v[0], vy = v[1], vz = v[2];
    const double rho2 = x * x + y * y;
    const double r = std::sqrt(rho2 + z * z);
    const double rho = std::sqrt(rho2);
    double lon = std::atan2(y, x) * kR;
    if (lon < 0.0)
        lon += 360.0;
    out->lon_deg = lon;
    out->lat_deg = std::atan2(z, rho) * kR;
    out->dist_au = r;
    out->lon_speed = (x * vy - y * vx) / rho2 * kR;
    out->lat_speed = (vz * rho2 - z * (x * vx + y * vy)) / (r * r * rho) * kR;
    out->dist_speed = (x * vx + y * vy + z * vz) / r;
    for (int c = 0; c < 3; ++c) {
        out->xyz_au[c] = p[c];
        out->vel_au_day[c] = v[c];
    }
    return succeed(err);
}

const prometheia_segments_layout*
prometheia_segments_get_layout(const prometheia_segments* segments) {
    return segments ? &segments->layout : nullptr;
}

void prometheia_segments_close(prometheia_segments* segments) {
    delete segments;
}

prometheia_status prometheia_segments_roots_sum(const prometheia_segments_sum* f,
                                                double jd_tt_start, double jd_tt_end,
                                                double tol_days, prometheia_segments_root* out,
                                                int capacity, int* count, double* jd_tt_resume,
                                                int* unresolved, prometheia_error* err) {
    if (count)
        *count = 0;
    if (unresolved)
        *unresolved = 0;
    if (jd_tt_resume)
        *jd_tt_resume = std::numeric_limits<double>::quiet_NaN();
    if (!f)
        return argument(err, "f is NULL");
    if (!count || !jd_tt_resume)
        return argument(err, "count or jd_tt_resume is NULL");
    if (capacity < 0 || (capacity > 0 && !out))
        return argument(err, "segments roots: capacity is negative, or out is NULL");
    if (!f->terms || f->nterms < 1 || f->nterms > PROMETHEIA_SEGMENTS_MAX_TERMS)
        return argument(err, "segments roots: terms is NULL, or nterms is outside "
                             "1..PROMETHEIA_SEGMENTS_MAX_TERMS");
    if (f->component < PROMETHEIA_SEGMENTS_LON || f->component > PROMETHEIA_SEGMENTS_LAT_SPEED)
        return argument(err, "segments roots: unknown component");
    if (!std::isfinite(jd_tt_start) || !std::isfinite(jd_tt_end) || !(jd_tt_end > jd_tt_start))
        return argument(err, "segments roots: the window must be finite and end after it starts");
    if (!(tol_days > 0.0) || !std::isfinite(tol_days))
        return argument(err, "segments roots: tol_days must be positive and finite");
    if (!std::isfinite(f->target) || !std::isfinite(f->target_rate) ||
        (f->target_rate != 0.0 && !std::isfinite(f->target_jd_tt)) || !std::isfinite(f->modulus))
        return argument(err, "segments roots: a field is not finite");
    // Terms naming the same handles are one term, their coefficients
    // summed; a term whose coefficient is then 0 is dropped. A sum with
    // nothing left is identically flat, and refused before any search.
    prometheia_segments_term terms[PROMETHEIA_SEGMENTS_MAX_TERMS];
    int n = 0;
    for (int i = 0; i < f->nterms; ++i) {
        const prometheia_segments_term& t = f->terms[i];
        if (!t.handles || t.count < 1 || !t.handles[0])
            return argument(err, "segments roots: a term needs at least one handle");
        if (!std::isfinite(t.c))
            return argument(err, "segments roots: a field is not finite");
        int same = -1;
        for (int j = 0; j < n && same < 0; ++j)
            if (terms[j].count == t.count &&
                std::equal(t.handles, t.handles + t.count, terms[j].handles))
                same = j;
        if (same >= 0)
            terms[same].c += t.c;
        else
            terms[n++] = t;
    }
    n = int(std::remove_if(terms, terms + n,
                           [](const prometheia_segments_term& t) { return t.c == 0.0; }) -
            terms);
    if (n == 0)
        return argument(err, "segments roots: every coefficient is 0, once terms naming the "
                             "same handles are combined");
    double modulus = f->modulus;
    if (f->component == PROMETHEIA_SEGMENTS_LON) {
        if (modulus < 0.0)
            return argument(err, "segments roots: the modulus is negative");
        if (modulus == 0.0)
            modulus = 360.0;
        // A turn of any longitude must move f by whole moduli.
        for (int i = 0; i < n; ++i) {
            const double turns = 360.0 * std::fabs(terms[i].c) / modulus;
            if (std::fabs(turns - std::round(turns)) > 1e-9 * std::max(1.0, turns))
                return argument(err, "segments roots: 360 c / modulus must be a whole number "
                                     "for every term");
        }
    } else if (modulus != 0.0) {
        return argument(err, "segments roots: only a longitude takes a modulus");
    }
    const prometheia_options& place = terms[0].handles[0]->opts;
    for (int i = 0; i < n; ++i)
        if (prometheia_status st =
                check_term(terms[i].handles, terms[i].count, place, jd_tt_start, jd_tt_end, err);
            st != PROMETHEIA_OK)
            return st;
    segment_roots::Function fn;
    fn.terms = terms;
    fn.nterms = n;
    fn.component = f->component;
    fn.target = f->target;
    fn.target_rate = f->target_rate;
    fn.target_jd_tt = f->target_jd_tt;
    fn.modulus = modulus;
    int limits = 0;
    *count = segment_roots::find(fn, jd_tt_start, jd_tt_end, tol_days, out, capacity, jd_tt_resume,
                                 &limits);
    if (unresolved)
        *unresolved = limits;
    return succeed(err);
}

prometheia_status prometheia_segments_roots(const prometheia_segments_function* f,
                                            double jd_tt_start, double jd_tt_end, double tol_days,
                                            prometheia_segments_root* out, int capacity, int* count,
                                            double* jd_tt_resume, int* unresolved,
                                            prometheia_error* err) {
    if (count)
        *count = 0;
    if (unresolved)
        *unresolved = 0;
    if (jd_tt_resume)
        *jd_tt_resume = std::numeric_limits<double>::quiet_NaN();
    if (!f)
        return argument(err, "f is NULL");
    if (f->nb < 0 || (f->nb > 0 && !f->b))
        return argument(err, "segments roots: b needs nb handles");
    const prometheia_segments_term terms[2] = {{f->a, f->na, f->ca}, {f->b, f->nb, f->cb}};
    prometheia_segments_sum sum{};
    sum.terms = terms;
    sum.nterms = f->nb > 0 ? 2 : 1;
    sum.component = f->component;
    sum.target = f->target;
    sum.target_rate = f->target_rate;
    sum.target_jd_tt = f->target_jd_tt;
    sum.modulus = f->modulus;
    return prometheia_segments_roots_sum(&sum, jd_tt_start, jd_tt_end, tol_days, out, capacity,
                                         count, jd_tt_resume, unresolved, err);
}

} // extern "C"

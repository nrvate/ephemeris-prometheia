// SPDX-License-Identifier: GPL-2.0-or-later
#include "session.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

#include "objects.hpp"
#include "prometheia/stars.hpp"
#include "prometheia/time.hpp"
#include "sha256.hpp"

namespace prometheia::server {
namespace {

using Clock = std::chrono::steady_clock;

// One block of row work: the span between blocks is where a CANCEL is read
// (3.4). Small enough that a CANCEL waits milliseconds, large enough that
// the clock reads and the ΔT reinstall cost nothing next to the rows.
constexpr int kComputeSliceMs = 2;
// The lookup budget this server offers (WELCOME's A.3 0x0010).
constexpr uint16_t kLookupMax = 1024;
// The star source the LOOKUP answer names; the compiled-in catalog.
constexpr const char* kStarSource = "prometheia stars (BSC5, Hipparcos 1991.25, Messier)";
constexpr const char* kCatalogSource = "prometheia small-body catalogs";

bool equals_ignore_case(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(unsigned(x)) == std::tolower(unsigned(y));
           });
}

// ---- Delta T adapters (3.5: the table, the one value, else the model) -----

class ConstantDeltaT final : public time::DeltaTModel {
public:
    explicit ConstantDeltaT(double seconds) : seconds_(seconds) {}
    double delta_t_seconds(double) const override { return seconds_; }

private:
    double seconds_;
};

class TableDeltaT final : public time::DeltaTModel {
public:
    explicit TableDeltaT(const eph::Request& req) : req_(req) {}
    double delta_t_seconds(double jd_tt) const override {
        eph::Time t;
        t.jd1 = jd_tt;
        return req_.DeltaTOf(t);
    }

private:
    const eph::Request& req_;
};

// The delta T a row used, in seconds, in the order 3.5 gives.
double delta_t_used(const eph::Request& q, const eph::Time& t) {
    if (!q.deltaTTable.empty()) {
        return q.DeltaTOf(t);
    }
    if (!eph::IsCanonicalNaN(q.deltaTSec)) {
        return q.deltaTSec;
    }
    const double jd = t.jd1 + t.jd2;
    return time::delta_t(q.timeScale == eph::kTimeUT1 ? time::jd_tt_from_ut1(jd) : jd);
}

// A row instant in TT, by the request's own time scale and ΔT ownership: the
// same conversion the rows' computes use, applied to the grid's ends.
double jd_tt_of(const eph::Request& q, const eph::Time& t) {
    const double jd = t.jd1 + t.jd2;
    switch (q.timeScale) {
    case eph::kTimeTT:
        return jd;
    case eph::kTimeTDB:
        return time::tt_from_tdb(jd);
    default:
        return jd + delta_t_used(q, t) / 86400.0;
    }
}

// ---- Profile planning ------------------------------------------------------

struct ProfilePlan {
    CalcOptions opts;
    bool rectangular = false;
    uint32_t columns = 0;
};

// Resolves every profile of a request, or says why one cannot be served.
// The header's parser has already refused what the registry does not know;
// what is left to refuse is a value this engine does not serve (3.4: ERROR
// 11, never a per-object error -- the profile is part of the question).
std::optional<std::string> plan_profiles(const eph::Request& q, std::vector<ProfilePlan>& out) {
    out.clear();
    out.reserve(q.profiles.size());
    for (const eph::Profile& pf : q.profiles) {
        ProfilePlan p;
        switch (pf.observer) {
        case eph::kObsGeo:
            p.opts.center = Center::Geocentric;
            break;
        case eph::kObsTopo:
            p.opts.center = Center::Topocentric;
            p.opts.site.lon_rad = pf.siteLonEastDeg * (std::acos(-1.0) / 180.0);
            p.opts.site.lat_rad = pf.siteLatDeg * (std::acos(-1.0) / 180.0);
            p.opts.site.height_m = pf.siteHeightM;
            break;
        case eph::kObsHelio:
            p.opts.center = Center::Heliocentric;
            break;
        case eph::kObsBary:
            p.opts.center = Center::Barycentric;
            break;
        default:
            p.opts.center = Center::Body;
            p.opts.center_body = pf.observerBody;
            break;
        }
        p.opts.coords = pf.plane == eph::kPlaneEquator ? Coords::Equatorial : Coords::Ecliptic;
        switch (pf.frame) {
        case eph::kFrameTrueOfDate:
            p.opts.frame = Frame::TrueOfDate;
            break;
        case eph::kFrameMeanOfDate:
            p.opts.frame = Frame::MeanOfDate;
            break;
        case eph::kFrameJ2000:
            p.opts.frame = Frame::J2000;
            break;
        default:
            p.opts.frame = Frame::ICRF;
            break;
        }
        p.opts.light_time = (pf.corrections & eph::kCorrLightTime) != 0;
        p.opts.deflection = (pf.corrections & eph::kCorrDeflection) != 0;
        p.opts.aberration = (pf.corrections & eph::kCorrAberration) != 0;
        p.opts.speed = pf.speeds != 0;
        p.opts.sigma = (pf.columns & eph::kColSigma) != 0;
        if (pf.siderealPlane != eph::kSidPlaneDate) {
            return "a sidereal plane this engine does not serve (only the ecliptic of date)";
        }
        if (pf.zodiac.empty()) {
            p.opts.sidereal = SiderealMode::Tropical;
        } else if (pf.zodiac == "fagan-bradley") {
            p.opts.sidereal = SiderealMode::FaganBradley;
        } else if (pf.zodiac == "lahiri") {
            p.opts.sidereal = SiderealMode::Lahiri;
        } else if (pf.zodiac == "user") {
            p.opts.sidereal = SiderealMode::User;
            p.opts.sidereal_epoch_jtdb = pf.anchorEpoch.Sum();
            p.opts.sidereal_ayanamsa_deg = pf.anchorAyanamsaDeg;
        } else {
            return "the zodiac '" + pf.zodiac + "' is not served by this engine";
        }
        p.rectangular = pf.form == eph::kFormRectangular;
        p.columns = pf.columns;
        out.push_back(std::move(p));
    }
    // The precession model (REQUEST TLV 0x0003) applies to every profile.
    Precession precession = Precession::IAU2006;
    bool fUnknownPrecession = false;
    for (const eph::Tlv& e : q.ext) {
        if (e.tag != eph::kReqTagPrecession) {
            continue;
        }
        std::string token;
        if (!eph::TlvStr8(e, &token)) {
            continue;
        }
        if (token == "vondrak2011") {
            precession = Precession::Vondrak2011;
        } else if (token != "iau2006") {
            fUnknownPrecession = true; // falls back, and the answer says so
        }
    }
    for (ProfilePlan& p : out) {
        p.opts.precession = precession;
    }
    return fUnknownPrecession ? std::optional<std::string>("") : std::nullopt;
}

// ---- corrApplied (3.4): structural availability, never the request's mask --

// Light time never applies to a catalog star (positions are directions of
// arrival); deflection is skipped at the Sun's centre, and for the Sun's own
// light, whose path passes through it; aberration runs for every observer,
// contributing ~0 at the barycentre. Orbit points take all three as
// conventions (3.5a). The classification is pinned by unit tests.
uint8_t corr_applied(eph::ObjKind kind, const CalcOptions& o, bool object_is_sun) {
    const bool sun_observer =
        o.center == Center::Heliocentric || (o.center == Center::Body && o.center_body == 10);
    const uint8_t deflection = (!sun_observer && !object_is_sun) ? eph::kCorrDeflection : 0;
    if (kind == eph::kObjStar) {
        return deflection | eph::kCorrAberration;
    }
    return deflection | eph::kCorrAberration | eph::kCorrLightTime;
}

// ---- engine failure -> A.17 -------------------------------------------------

// 3.9a: errors by meaning, not convenience; 3.8: the text names no instant.
eph::ObjErr obj_err_of(const Error& e) {
    const std::string& m = e.message;
    if (m.find("is ambiguous") != std::string::npos) {
        return eph::kOErrAmbiguous;
    }
    if (m.find("are undefined") != std::string::npos ||
        m.find("is undefined") != std::string::npos) {
        return eph::kOErrUndefinedPoint;
    }
    if (m.find("integration failed") != std::string::npos) {
        return eph::kOErrNumerical;
    }
    switch (e.code) {
    case ErrorCode::NotFound:
        return eph::kOErrUnknownBody;
    case ErrorCode::IoError:
    case ErrorCode::FormatError:
    case ErrorCode::CorruptionError:
        return eph::kOErrDataMissing;
    case ErrorCode::ArgumentError:
        if (m.find("coverage") != std::string::npos ||
            m.find("perturbing masses") != std::string::npos) {
            return eph::kOErrCoverage;
        }
        return eph::kOErrUnsupported;
    default:
        return eph::kOErrInternal;
    }
}

std::string clean_err_text(std::string text) {
    // The one engine message that carries the instant it failed at.
    if (text.find("at JD ") != std::string::npos) {
        return "outside the ephemeris's time coverage";
    }
    return text;
}

// ---- the segments span and its refusals -------------------------------------

// What a representation = 1 REQUEST asks, after every refusal that costs no
// work. The grid's first and last instants bound the span; the ladder
// quantises the target downward only; the client's degree hint caps the fit.
struct SegmentsParams {
    double jd_from_tt = 0.0;
    double jd_to_tt = 0.0;
    size_t rung = 1; // 0.1"
    double target_arcsec = 0.1;
    int min_degree = 6;
    int max_degree = kSegMaxDegree;
};

std::optional<std::pair<eph::ErrCode, std::string>>
check_segments(const eph::Request& q, const ServerConfig& cfg, SegmentsParams& out) {
    const double a = jd_tt_of(q, q.RowTime(0));
    const double b = jd_tt_of(q, q.RowTime(q.nTime - 1));
    out.jd_from_tt = std::min(a, b);
    out.jd_to_tt = std::max(a, b);
    if (!(out.jd_to_tt > out.jd_from_tt)) {
        // One instant, or a grid degenerate to it: the cell covering it.
        out.jd_to_tt = out.jd_from_tt + 1e-9;
    }
    if (!(out.jd_to_tt - out.jd_from_tt <= double(cfg.max_seg_span_days))) {
        return std::make_pair(eph::kErrLimits,
                              "the span is over WELCOME's segMaxSpanDays bound of " +
                                  std::to_string(cfg.max_seg_span_days) + " days");
    }
    out.target_arcsec = double(q.segTargetErrArcsec);
    if (out.target_arcsec < kFinestErrArcsec) {
        char text[160];
        std::snprintf(text, sizeof text,
                      "the finest error this server fits is %g arcsec; asking finer would be "
                      "served coarser",
                      kFinestErrArcsec);
        return std::make_pair(eph::kErrLimits, text);
    }
    out.rung = ladder_rung(out.target_arcsec);
    out.max_degree =
        q.maxDegreeHint ? std::min(int(q.maxDegreeHint), kSegMaxDegree) : kSegMaxDegree;
    out.max_degree = std::max(1, out.max_degree);
    out.min_degree = std::min(6, out.max_degree);
    return std::nullopt;
}

// The cell cache keys: everything that identifies the fit except the span
// and the rung, appended as raw bytes so no field can collide by spelling.
//
// The body key names the resolved object and every option that shapes the
// fit: observer (and site, and observing body), plane, frame, corrections,
// precession, and the ΔT the sampler converts TT instants by for a UT1
// request -- a cell fitted under one ΔT must never serve another. The zodiac
// is deliberately absent: coefficients are tropical whatever the zodiac, and
// the ayanamsa travels as its own series.
std::string seg_key_prefix(const ServerConfig& cfg, const eph::Request& q,
                           const ResolvedObject& obj, const CalcOptions& o) {
    std::string k = cfg.dataset_id;
    k.push_back('\0');
    const auto u8 = [&k](uint64_t v) { k.append(reinterpret_cast<const char*>(&v), 1); };
    const auto raw = [&k](const void* p, size_t n) {
        k.append(reinterpret_cast<const char*>(p), n);
    };
    u8(uint8_t(obj.kind));
    raw(&obj.naif_id, sizeof obj.naif_id);
    raw(&obj.star_index, sizeof obj.star_index);
    u8(uint8_t(obj.point));
    u8(obj.elements == OrbitElements::Mean ? 0 : 1);
    u8(uint8_t(o.center));
    raw(&o.center_body, sizeof o.center_body);
    raw(&o.site.lon_rad, sizeof o.site.lon_rad);
    raw(&o.site.lat_rad, sizeof o.site.lat_rad);
    raw(&o.site.height_m, sizeof o.site.height_m);
    u8(uint8_t(o.coords));
    u8(uint8_t(o.frame));
    u8((o.light_time ? 1 : 0) | (o.deflection ? 2 : 0) | (o.aberration ? 4 : 0));
    u8(uint8_t(o.precession));
    if (!q.deltaTTable.empty()) {
        // The table's bytes, hashed: a key per cell carries the digest, not
        // the table.
        eph::Tlv tlv;
        eph::EncodeDeltaTTable(q.deltaTTable, &tlv);
        Sha256 hash;
        hash.update(reinterpret_cast<const uint8_t*>(tlv.value.data()), tlv.value.size());
        const std::string digest = hash.hex();
        k.append(digest.data(), 16);
    } else if (!eph::IsCanonicalNaN(q.deltaTSec)) {
        raw(&q.deltaTSec, sizeof q.deltaTSec);
    } else {
        k.push_back('M'); // the server's model, named in WELCOME
    }
    k.append("\0seg1", 5);
    return k;
}

// The ayanamsa series key: the zodiac (and its anchor), the frame and the
// precession its drift is measured in. No ΔT: the series is a function of TT.
std::string ayan_key_prefix(const ServerConfig& cfg, const CalcOptions& o) {
    std::string k = cfg.dataset_id;
    k.push_back('\0');
    const auto u8 = [&k](uint64_t v) { k.append(reinterpret_cast<const char*>(&v), 1); };
    const auto raw = [&k](const void* p, size_t n) {
        k.append(reinterpret_cast<const char*>(p), n);
    };
    k.append("ayan", 4);
    u8(uint8_t(int(o.sidereal)));
    if (o.sidereal == SiderealMode::User) {
        raw(&o.sidereal_epoch_jtdb, sizeof o.sidereal_epoch_jtdb);
        raw(&o.sidereal_ayanamsa_deg, sizeof o.sidereal_ayanamsa_deg);
    }
    u8(uint8_t(o.frame));
    u8(uint8_t(o.precession));
    k.append("\0aya1", 5);
    return k;
}

// The ayanamsa of a profile at a TT instant: any sidereal calc carries it in
// its result; the Sun is always present. The ayanamsa is a frame quantity --
// it does not depend on the observer -- so the sampler always observes from
// the geocentre, which every engine serves (a heliocentric Sun would be the
// observer asking about itself).
segments::ScalarSampler ayan_sampler_for(Engine& engine, CalcOptions opts) {
    opts.center = Center::Geocentric;
    opts.site = {};
    opts.center_body = 0;
    opts.coords = Coords::Ecliptic;
    opts.speed = false;
    opts.sigma = false;
    return [&engine, opts](double jd_tt, double& value) -> Result<void> {
        auto r = engine.calc(body::kSun, jd_tt, opts);
        if (!r) {
            return r.error();
        }
        if (!r.value().ayanamsa_deg) {
            return make_error(ErrorCode::ArgumentError, "no ayanamsa for this profile");
        }
        value = *r.value().ayanamsa_deg;
        return {};
    };
}

// ---- WELCOME ----------------------------------------------------------------

void build_welcome(std::vector<uint8_t>& payload, const ServerConfig& cfg, uint8_t version) {
    eph::Welcome w;
    w.protoSession = version;
    w.caps = eph::kCapF32 | eph::kCapInstantLists | eph::kCapLookup | eph::kCapDeepSky |
             eph::kCapCancel | eph::kCapPriority | eph::kCapSegments;
    w.maxObjs = cfg.max_objs;
    w.maxRows = cfg.max_rows;
    w.maxChunkRows = cfg.max_chunk_rows;
    w.maxPayload = cfg.max_payload;
    w.maxCells = cfg.max_cells;
    w.maxProfiles = cfg.max_profiles;
    w.serverName = cfg.server_name;
    w.engine = cfg.engine;
    w.datasetId = cfg.dataset_id;

    eph::Capabilities c;
    c.kinds = (1u << eph::kObjBody) | (1u << eph::kObjOrbitPoint) | (1u << eph::kObjStar) |
              (1u << eph::kObjDesignation);
    c.observers = (1u << eph::kObsGeo) | (1u << eph::kObsTopo) | (1u << eph::kObsHelio) |
                  (1u << eph::kObsBary) | (1u << eph::kObsBody);
    c.planes = (1u << eph::kPlaneEcliptic) | (1u << eph::kPlaneEquator);
    c.forms = (1u << eph::kFormSpherical) | (1u << eph::kFormRectangular);
    c.frames = (1u << eph::kFrameTrueOfDate) | (1u << eph::kFrameMeanOfDate) |
               (1u << eph::kFrameJ2000) | (1u << eph::kFrameIcrf);
    // What the engine can honour, per observer (3.5a): the Sun's centre
    // cannot be deflected; everything else can.
    c.corrMasks = {
        {(1u << eph::kObsGeo) | (1u << eph::kObsTopo) | (1u << eph::kObsBody), eph::kCorrMask},
        {(1u << eph::kObsHelio), eph::kCorrLightTime | eph::kCorrAberration},
        {(1u << eph::kObsBary), eph::kCorrMask}};
    c.orbitPoints = (1u << eph::kPtAscNode) | (1u << eph::kPtDescNode) | (1u << eph::kPtPeri) |
                    (1u << eph::kPtApo);
    c.orbitMethods = (1u << eph::kMethMean) | (1u << eph::kMethOsculating);
    c.columns = eph::kColMask;
    c.zodiacs = {"fagan-bradley", "lahiri", "user"};
    c.siderealPlanes = 1u << eph::kSidPlaneDate;
    c.timeScales = (1u << eph::kTimeUT1) | (1u << eph::kTimeTT) | (1u << eph::kTimeTDB);
    c.deltaTModel = kDeltaTModelName;
    c.lookupMax = kLookupMax;
    // The kinds this server fits (3.4): bodies, orbit points of the mean and
    // osculating elements except the Moon's osculating apsides, and the
    // designations that resolve to bodies. Stars have no distance to fit;
    // the hypotheticals and polynomial elements are not served at all.
    c.fSegments = true;
    c.segMaxDegree = uint8_t(kSegMaxDegree);
    c.segMaxPerObject = uint32_t(kSegMaxPerObject);
    c.segMinErrArcsec = float(kFinestErrArcsec);
    c.segKinds = (1u << eph::kObjBody) | (1u << eph::kObjOrbitPoint) | (1u << eph::kObjDesignation);
    c.segMaxSpanDays = cfg.max_seg_span_days;
    eph::EncodeCapabilities(c, &w.caps_);
    // The deep-sky catalogues kind 2 resolves (A.3 0x000B); EncodeCapabilities
    // does not know this one, so it goes in beside the others, ascending.
    {
        eph::Tlv cat;
        cat.tag = eph::kCapTagCatalogs;
        std::vector<uint8_t> cb;
        eph::Writer cw(&cb);
        cw.u16(1);
        cw.str8("messier");
        cw.str8("BSC5 + Hipparcos (1991.25), compiled-in");
        cat.value.assign(reinterpret_cast<const char*>(cb.data()), cb.size());
        w.caps_.push_back(std::move(cat));
        std::sort(w.caps_.begin(), w.caps_.end(),
                  [](const eph::Tlv& a, const eph::Tlv& b) { return a.tag < b.tag; });
    }
    eph::EncodeWelcome(&payload, w);
}

} // namespace

// ---- ResultCache -----------------------------------------------------------

std::shared_ptr<const Answer> ResultCache::get(const std::string& key) {
    const auto it = map_.find(key);
    if (it == map_.end()) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->second;
}

void ResultCache::put(const std::string& key, std::shared_ptr<const Answer> answer) {
    const size_t charge = answer->bytes() + 2 * key.size();
    if (charge > budget_ || map_.count(key)) {
        return;
    }
    while (used_ + charge > budget_ && !lru_.empty()) {
        const auto& victim = lru_.back();
        used_ -= victim.second->bytes() + 2 * victim.first.size();
        map_.erase(victim.first);
        lru_.pop_back();
    }
    lru_.emplace_front(key, std::move(answer));
    map_.emplace(key, lru_.begin());
    used_ += charge;
}

// ---- LoopContext -----------------------------------------------------------

LoopContext::LoopContext(Engine engine, const ServerConfig& config, Limits* limits,
                         Metrics* metrics)
    : engine_(std::move(engine)), config_(config), cache_(config.cache_bytes),
      seg_cache_(config.seg_cache_bytes), ayan_cache_(config.seg_cache_bytes), limits_(limits),
      metrics_(metrics ? metrics : &own_metrics_) {}

std::shared_ptr<const Answer> LoopContext::answer(const eph::Request& req,
                                                  std::string_view question) {
    std::string key = config_.dataset_id;
    key.push_back('\0');
    key.append(question);
    if (auto hit = cache_.get(key)) {
        ++metrics_->cache_hits;
        return hit;
    }
    const auto t0 = Clock::now();
    auto computed = compute(req);
    finish(key, computed, uint64_t(req.objs.size()) * req.nTime,
           std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    return computed;
}

void LoopContext::finish(const std::string& key, const std::shared_ptr<const Answer>& answer,
                         uint64_t cells, double ms) {
    metrics_->computed(cells, ms);
    cache_.put(key, answer);
}

// ---- the resumable computes --------------------------------------------------

// One samples answer being computed a block of rows at a time. Construction
// resolves the objects (cheap); the rows happen in run_until, which installs
// the request's ΔT model for its own slice and restores the engine after, so
// slices of different requests interleave safely on the loop's one engine.
class SamplesComputer {
public:
    SamplesComputer(LoopContext& ctx, eph::Request req) : ctx_(&ctx), req_(std::move(req)) {
        const uint32_t n_obj = uint32_t(req_.objs.size());
        ans_ = std::make_shared<Answer>();
        ans_->n_obj = n_obj;
        ans_->n_time = req_.nTime;
        plan_profiles(req_, plans_); // on_request has already refused what fails here
        for (const ProfilePlan& p : plans_) {
            ans_->columns_present |= p.columns;
        }
        n_cols_ = 6 + eph::PopCount(ans_->columns_present);
        ans_->cols.assign(size_t(n_obj) * req_.nTime * size_t(n_cols_),
                          std::numeric_limits<double>::quiet_NaN());
        ans_->meta.assign(n_obj, eph::Meta{});
        objects_.assign(n_obj, Slot{});
        for (uint32_t o = 0; o < n_obj; ++o) {
            Slot& t = objects_[o];
            t.kind = eph::ObjKind(req_.objs[o].kind);
            const ProfilePlan& plan = plans_[req_.objs[o].profile];
            auto resolved = resolve_object(req_.objs[o], ctx_->engine());
            if (!resolved) {
                t.why = clean_err_text(resolved.error().message);
                t.code = obj_err_of(resolved.error());
                continue;
            }
            t.obj = std::move(resolved).value();
            t.resolved = true;
            // A body observer with observerBody equal to the object (3.5).
            if (t.kind != eph::kObjStar && plan.opts.center == Center::Body &&
                plan.opts.center_body == t.obj.naif_id) {
                t.why = "the observer is the object";
                t.code = eph::kOErrUnsupported;
                t.resolved = false;
            }
        }
    }

    // Rows until `until`; at least one row a call, so every slice makes
    // progress. True when the answer is whole.
    bool run_until(Clock::time_point until) {
        const auto t0 = Clock::now();
        ConstantDeltaT constant(req_.deltaTSec);
        TableDeltaT table(req_);
        if (!req_.deltaTTable.empty()) {
            ctx_->engine().set_delta_t_model(&table);
        } else if (!eph::IsCanonicalNaN(req_.deltaTSec)) {
            ctx_->engine().set_delta_t_model(&constant);
        }
        struct Restore {
            Engine& engine;
            ~Restore() { engine.set_delta_t_model(nullptr); }
        } restore{ctx_->engine()};

        const uint32_t n_obj = ans_->n_obj;
        bool first_of_slice = true;
        for (; next_row_ < ans_->n_time; ++next_row_) {
            if (!first_of_slice && Clock::now() >= until) {
                break;
            }
            first_of_slice = false;
            const eph::Time t = req_.RowTime(next_row_);
            const double jd = t.jd1 + t.jd2;
            const double dtd =
                (ans_->columns_present & eph::kColDeltaT) ? delta_t_used(req_, t) : 0.0;
            for (uint32_t o = 0; o < n_obj; ++o) {
                Slot& s = objects_[o];
                if (!s.resolved) {
                    continue; // rows stay NaN
                }
                const ProfilePlan& plan = plans_[req_.objs[o].profile];
                double* row = ans_->cols.data() +
                              (size_t(o) * ans_->n_time + size_t(next_row_)) * size_t(n_cols_);
                auto res = calc_at(ctx_->engine(), s.obj, jd, req_.timeScale, plan.opts);
                if (!res) {
                    if (s.first_failed_row == eph::kRowNone) {
                        s.first_failed_row = next_row_;
                        s.first_err = clean_err_text(res.error().message);
                        s.code = obj_err_of(res.error());
                    }
                    continue;
                }
                ++s.rows_ok;
                const CalcResult& cr = res.value();
                if (s.source_idx < 0) {
                    auto it =
                        std::find(ans_->sources.begin(), ans_->sources.end(), cr.provenance.source);
                    if (it == ans_->sources.end()) {
                        ans_->sources.emplace_back(cr.provenance.source);
                        it = ans_->sources.end() - 1;
                    }
                    s.source_idx = int(it - ans_->sources.begin());
                }
                if (cr.sigma_arcsec) {
                    s.any_sigma = true;
                }
                const bool speeds = plan.opts.speed;
                if (plan.rectangular) {
                    for (int i = 0; i < 3; ++i) {
                        row[i] = cr.pos.xyz_au[i];
                        row[3 + i] = speeds ? cr.pos.vel_au_day[i] : 0.0;
                    }
                } else {
                    const bool no_dist = s.obj.no_parallax;
                    row[0] = cr.pos.lon_deg;
                    row[1] = cr.pos.lat_deg;
                    row[2] = no_dist ? 0.0 : cr.pos.dist_au;
                    row[3] = speeds ? cr.pos.lon_speed : 0.0;
                    row[4] = speeds ? cr.pos.lat_speed : 0.0;
                    row[5] = speeds && !no_dist ? cr.pos.dist_speed : 0.0;
                }
                uint32_t extra = ans_->columns_present;
                int k = 6;
                for (uint32_t bit = 0; bit < 4; ++bit) {
                    if (!(extra & (1u << bit))) {
                        continue;
                    }
                    switch (1u << bit) {
                    case eph::kColSigma:
                        row[k++] = cr.sigma_arcsec.value_or(0.0);
                        break;
                    case eph::kColAyanamsa:
                        row[k++] = cr.ayanamsa_deg.value_or(0.0);
                        break;
                    case eph::kColLightTime:
                        row[k++] = cr.provenance.light_time_days;
                        break;
                    case eph::kColDeltaT:
                        row[k++] = dtd;
                        break;
                    default:
                        break;
                    }
                }
            }
        }
        ms_ += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        if (next_row_ < ans_->n_time) {
            return false;
        }
        fill_meta();
        return true;
    }

    std::shared_ptr<const Answer> take() { return std::move(ans_); }
    uint64_t cells() const { return uint64_t(req_.objs.size()) * req_.nTime; }
    double ms() const { return ms_; }

private:
    struct Slot {
        ResolvedObject obj;
        eph::ObjKind kind = eph::kObjBody;
        bool resolved = false;
        std::string why; // set: every row fails with this reason
        eph::ObjErr code = eph::kOErrNone;
        uint32_t rows_ok = 0;
        uint32_t first_failed_row = eph::kRowNone;
        std::string first_err;
        int source_idx = -1;
        bool any_sigma = false;
    };

    // META's facts are about the whole answer (3.4), so they are written
    // once, at the end.
    void fill_meta() {
        for (uint32_t o = 0; o < ans_->n_obj; ++o) {
            const Slot& s = objects_[o];
            const ProfilePlan& plan = plans_[req_.objs[o].profile];
            eph::Meta& m = ans_->meta[o];
            m.rowsOk = int32_t(s.rows_ok);
            m.errCode = s.why.empty() && s.first_err.empty() ? eph::kOErrNone : s.code;
            m.sourceIdx = s.source_idx >= 0 ? uint8_t(s.source_idx) : eph::kSourceNone;
            if (!plan.opts.speed) {
                m.flags |= eph::kMetaNoSpeeds;
            }
            if (s.obj.no_parallax && s.resolved) {
                m.flags |= eph::kMetaNoDistance;
            }
            if (s.rows_ok > 0 && s.rows_ok < ans_->n_time) {
                m.flags |= eph::kMetaPartial;
            }
            if (s.any_sigma) {
                m.flags |= eph::kMetaHasSigma;
            }
            m.corrApplied =
                s.resolved ? corr_applied(eph::ObjKind(req_.objs[o].kind), plan.opts, s.obj.is_sun)
                           : 0;
            m.resolvedNaif = s.resolved && s.kind != eph::kObjStar ? s.obj.naif_id : eph::kNaifNone;
            m.firstFailedRow = s.first_failed_row;
            m.name = s.resolved ? s.obj.name : "";
            m.errText = !s.why.empty() ? s.why : s.first_err;
        }
    }

    LoopContext* ctx_;
    eph::Request req_;
    std::vector<ProfilePlan> plans_;
    std::vector<Slot> objects_;
    std::shared_ptr<Answer> ans_;
    uint32_t next_row_ = 0;
    int n_cols_ = 6;
    double ms_ = 0.0;
};

// One segments answer, fitted a cell at a time per object: the cancellation
// granularity is one lattice cell, and the cell cache makes the second
// client's overlapping span free. Objects resolve lazily, so an unfittable
// one costs one resolution and no sampler calls.
class SegmentsComputer {
public:
    SegmentsComputer(LoopContext& ctx, eph::Request req, SegmentsParams params)
        : ctx_(&ctx), req_(std::move(req)), params_(params) {
        plan_profiles(req_, plans_);
        ans_ = std::make_shared<SegAnswer>();
        ans_->n_obj = uint32_t(req_.objs.size());
        ans_->meta.assign(ans_->n_obj, eph::Meta{});
        ans_->segs.assign(ans_->n_obj, std::vector<eph::Segment>{});
        slots_.assign(ans_->n_obj, Slot{});
        first_cell_ = Lattice::cell_of(params_.jd_from_tt);
        last_cell_ = Lattice::cell_of(params_.jd_to_tt);
        cell_ = first_cell_;
        // The ayanamsa series of every sidereal profile, fitted over the
        // same cells at the same rung (3.4).
        for (uint8_t p = 0; p < req_.profiles.size(); ++p) {
            if (req_.profiles[p].zodiac.empty()) {
                continue;
            }
            AyanTask task;
            task.profile = p;
            task.prefix = ayan_key_prefix(ctx.config(), plans_[p].opts);
            task.sampler = ayan_sampler_for(ctx.engine(), plans_[p].opts);
            task.cell = first_cell_;
            ayan_.push_back(std::move(task));
        }
    }

    bool run_until(Clock::time_point until) {
        out_of_time_ = false; // a slice that ran out leaves the walk to be resumed
        const auto t0 = Clock::now();
        ConstantDeltaT constant(req_.deltaTSec);
        TableDeltaT table(req_);
        if (!req_.deltaTTable.empty()) {
            ctx_->engine().set_delta_t_model(&table);
        } else if (!eph::IsCanonicalNaN(req_.deltaTSec)) {
            ctx_->engine().set_delta_t_model(&constant);
        }
        struct Restore {
            Engine& engine;
            ~Restore() { engine.set_delta_t_model(nullptr); }
        } restore{ctx_->engine()};

        bool did = false;
        while (obj_index_ < slots_.size() && !out_of_time_) {
            Slot& s = slots_[obj_index_];
            if (!s.started) {
                start_object(s);
                did = true;
                if (!s.servable) {
                    finish_object(s);
                    ++obj_index_;
                    continue;
                }
            }
            while (cell_ <= last_cell_) {
                if (did && Clock::now() >= until) {
                    out_of_time_ = true;
                    break;
                }
                SegmentRequest one;
                one.jd_from_tt = Lattice::cell_start(cell_);
                one.jd_to_tt = Lattice::cell_end(cell_);
                one.target_err_arcsec = params_.target_arcsec;
                one.min_degree = params_.min_degree;
                one.max_degree = params_.max_degree;
                one.max_cells = 1;
                auto got = segments_for(ctx_->seg_cache(), s.prefix, one, s.sampler);
                if (!got) {
                    s.failed = true;
                    s.code = obj_err_of(got.error());
                    s.why = clean_err_text(got.error().message);
                    break;
                }
                append(s.out, got.value());
                s.sampler_calls += got.value().sampler_calls;
                ++cell_;
                did = true;
                if (s.out.size() > kSegMaxPerObject) {
                    s.failed = true;
                    s.code = eph::kOErrUnsupported;
                    s.why = "the span needs more segments per object than this server fits";
                    break;
                }
            }
            if (out_of_time_) {
                break;
            }
            finish_object(s);
            ++obj_index_;
            cell_ = first_cell_;
        }
        // The ayanamsa series, after the objects, a cell at a time.
        while (ayan_index_ < ayan_.size() && !out_of_time_) {
            AyanTask& task = ayan_[ayan_index_];
            while (task.cell <= last_cell_) {
                if (did && Clock::now() >= until) {
                    out_of_time_ = true;
                    break;
                }
                SegmentRequest one;
                one.jd_from_tt = Lattice::cell_start(task.cell);
                one.jd_to_tt = Lattice::cell_end(task.cell);
                one.target_err_arcsec = params_.target_arcsec;
                one.min_degree = std::min(3, params_.max_degree);
                one.max_degree = params_.max_degree;
                one.max_cells = 1;
                // The rung in the ayanamsa's own units: degrees.
                auto got = scalar_series_for(ctx_->ayan_cache(), task.prefix, one,
                                             kErrorLadder[params_.rung] / 3600.0, task.sampler);
                if (!got) {
                    fatal_ = std::make_pair(eph::kErrInternal,
                                            "the ayanamsa series could not be fitted: " +
                                                got.error().message);
                    break;
                }
                for (const segments::ScalarSegment& g : got.value().segments) {
                    eph::AyanSeg wire;
                    wire.mid.jd1 = g.mid_jd_tt;
                    wire.halfSpanDays = g.half_span_days;
                    wire.degree = uint8_t(g.degree);
                    wire.errArcsec = float(g.err_value * 3600.0);
                    wire.coef = g.c;
                    task.segs.push_back(std::move(wire));
                }
                ++task.cell;
                did = true;
            }
            if (fatal_) {
                break;
            }
            if (!out_of_time_) {
                ++ayan_index_;
            }
        }
        ms_ += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        // A fatal ends the compute with the answer unassemblable; nothing
        // more can be done, so the slice reports completion and the session
        // turns the fatal into the request's ERROR.
        return fatal_.has_value() || (obj_index_ >= slots_.size() && ayan_index_ >= ayan_.size());
    }

    std::shared_ptr<const SegAnswer> take() {
        for (AyanTask& task : ayan_) {
            eph::AyanSeries series;
            series.profile = task.profile;
            series.segs = std::move(task.segs);
            ans_->ayan.push_back(std::move(series));
        }
        return std::move(ans_);
    }

    // Set when the answer cannot be assembled at all: the ayanamsa series a
    // sidereal profile owes would not fit.
    const std::optional<std::pair<eph::ErrCode, std::string>>& fatal() const { return fatal_; }
    double ms() const { return ms_; }

private:
    struct Slot {
        ResolvedObject obj;
        eph::ObjKind kind = eph::kObjBody;
        bool started = false;
        bool servable = false;
        bool failed = false;
        std::string why;
        eph::ObjErr code = eph::kOErrNone;
        std::string prefix;
        segments::Sampler sampler;
        std::vector<eph::Segment> out;
        size_t sampler_calls = 0;
        int source_idx = -1;
    };
    struct AyanTask {
        uint8_t profile = 0;
        std::string prefix;
        segments::ScalarSampler sampler;
        long long cell = 0;
        std::vector<eph::AyanSeg> segs;
    };

    void start_object(Slot& s) {
        const eph::Object& spec = req_.objs[obj_index_];
        s.started = true;
        s.kind = eph::ObjKind(spec.kind);
        // The capability names the kinds this server fits (3.4); anything
        // else is a per-object error, as is the Moon's osculating apsis,
        // which swings degrees a day on purpose.
        const bool fitted_kind = spec.kind == eph::kObjBody || spec.kind == eph::kObjOrbitPoint ||
                                 spec.kind == eph::kObjDesignation;
        const bool lunar_osculating = spec.kind == eph::kObjOrbitPoint &&
                                      spec.method == eph::kMethOsculating && spec.naif == 301;
        auto resolved = resolve_object(spec, ctx_->engine());
        if (!fitted_kind) {
            s.why = "segments are not fitted for this kind of object";
            s.code = eph::kOErrUnsupported;
            return;
        }
        if (lunar_osculating) {
            s.why = "the Moon's osculating points swing degrees a day; they are not fitted";
            s.code = eph::kOErrUnsupported;
            return;
        }
        if (!resolved) {
            s.why = clean_err_text(resolved.error().message);
            s.code = obj_err_of(resolved.error());
            return;
        }
        s.obj = std::move(resolved).value();
        const ProfilePlan& plan = plans_[spec.profile];
        if (plan.opts.center == Center::Body && plan.opts.center_body == s.obj.naif_id &&
            s.kind != eph::kObjStar) {
            s.why = "the observer is the object";
            s.code = eph::kOErrUnsupported;
            return;
        }
        // Coefficients are tropical in the profile's frame; the zodiac rides
        // as its own series, so the sampler never applies it (3.4).
        CalcOptions sampler_opts = plan.opts;
        sampler_opts.sidereal = SiderealMode::Tropical;
        s.sampler = sampler_for(ctx_->engine(), s.obj, sampler_opts);
        s.prefix = seg_key_prefix(ctx_->config(), req_, s.obj, plan.opts);
        s.servable = true;
    }

    void finish_object(Slot& s) {
        eph::Meta& m = ans_->meta[obj_index_];
        m.firstFailedRow = eph::kRowNone;
        m.name = s.servable ? s.obj.name : "";
        if (!s.servable || s.failed) {
            m.errCode = s.code;
            m.errText = s.why;
            m.sourceIdx = eph::kSourceNone;
            ans_->segs[obj_index_].clear(); // an object that failed serves none
            return;
        }
        m.rowsOk = int32_t(s.out.size());
        ans_->segs[obj_index_] = std::move(s.out);
        m.corrApplied =
            corr_applied(s.kind, plans_[req_.objs[obj_index_].profile].opts, s.obj.is_sun);
        m.resolvedNaif = s.kind != eph::kObjStar ? s.obj.naif_id : eph::kNaifNone;
        // One probe at the span's middle names the source the fits came from.
        const double mid = 0.5 * (params_.jd_from_tt + params_.jd_to_tt);
        if (auto probe = calc_at(ctx_->engine(), s.obj, mid, req_.timeScale,
                                 plans_[req_.objs[obj_index_].profile].opts)) {
            const std::string source(probe.value().provenance.source);
            auto it = std::find(ans_->sources.begin(), ans_->sources.end(), source);
            if (it == ans_->sources.end()) {
                ans_->sources.push_back(source);
                it = ans_->sources.end() - 1;
            }
            m.sourceIdx = uint8_t(it - ans_->sources.begin());
        }
    }

    static void append(std::vector<eph::Segment>& out, const SegmentAnswer& cell) {
        for (const segments::Segment& g : cell.segments) {
            eph::Segment wire;
            wire.mid.jd1 = g.mid_jd_tt;
            wire.halfSpanDays = g.half_span_days;
            wire.degree = uint8_t(g.degree);
            wire.errArcsec = float(g.err_arcsec);
            wire.errRelDist = float(g.err_rel_dist);
            wire.errRateArcsecPerDay = float(g.err_rate_arcsec_per_day);
            wire.coef.reserve(3 * (size_t(g.degree) + 1));
            wire.coef.insert(wire.coef.end(), g.x.begin(), g.x.end());
            wire.coef.insert(wire.coef.end(), g.y.begin(), g.y.end());
            wire.coef.insert(wire.coef.end(), g.z.begin(), g.z.end());
            out.push_back(std::move(wire));
        }
    }

    LoopContext* ctx_;
    eph::Request req_;
    SegmentsParams params_;
    std::vector<ProfilePlan> plans_;
    std::shared_ptr<SegAnswer> ans_;
    std::vector<Slot> slots_;
    std::vector<AyanTask> ayan_;
    long long first_cell_ = 0, last_cell_ = 0, cell_ = 0;
    size_t obj_index_ = 0;
    size_t ayan_index_ = 0;
    bool out_of_time_ = false;
    std::optional<std::pair<eph::ErrCode, std::string>> fatal_;
    double ms_ = 0.0;
};

std::shared_ptr<const Answer> LoopContext::compute(const eph::Request& req) {
    SamplesComputer computer(*this, req);
    while (!computer.run_until(Clock::now() + std::chrono::seconds(10))) {
    }
    return computer.take();
}

// ---- Session ---------------------------------------------------------------

Session::~Session() = default;
Session::Stream::~Stream() = default;

void Session::send(uint16_t type, uint32_t request_id, const std::vector<uint8_t>& payload) {
    control_.emplace_back();
    eph::WriteEnvelope(&control_.back(), type, request_id, payload.size(),
                       version_ ? version_ : eph::kProtoVersion);
    control_.back().insert(control_.back().end(), payload.begin(), payload.end());
}

void Session::send_error(uint32_t request_id, eph::ErrCode code, uint16_t flags, uint32_t retry_ms,
                         const std::string& text) {
    eph::Error e;
    e.code = code;
    e.flags = flags;
    e.retryAfterMs = retry_ms;
    e.text = text;
    std::vector<uint8_t> payload;
    eph::EncodeError(&payload, e);
    ctx_.metrics().error(code);
    send(eph::kMsgError, request_id, payload);
}

bool Session::on_message(std::string_view message, bool binary) {
    if (!binary) {
        send_error(0, eph::kErrMalformed, eph::kErrFlagClosing, 0,
                   "the protocol is binary frames only");
        return false;
    }
    if (message.size() < eph::kEnvelopeSize) {
        send_error(0, eph::kErrMalformed, eph::kErrFlagClosing, 0,
                   "message shorter than the envelope");
        return false;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(message.data());
    eph::Envelope env{};
    std::string why;
    const eph::Outcome o = eph::ParseEnvelope(bytes, message.size(), &env, &why);
    if (env.version < eph::kProtoMin) {
        // 3.3.5: an older client, refused in its own layout (versions 2-3).
        eph::LegacyError le;
        le.requestId = env.requestId;
        le.code = eph::kErrVersion;
        le.text = "this server speaks protocol 4 and up";
        std::vector<uint8_t> payload;
        eph::EncodeLegacyError(&payload, le);
        control_.emplace_back();
        eph::WriteEnvelope(&control_.back(), eph::kMsgError, env.requestId, payload.size(),
                           env.version);
        control_.back().insert(control_.back().end(), payload.begin(), payload.end());
        return false;
    }
    if (o != eph::kOk) {
        send_error(env.requestId, eph::kErrMalformed, eph::kErrFlagClosing, 0, why);
        return false;
    }
    if (env.flags & eph::kEnvFlagZstd) {
        // Not advertised; a client that sends it anyway broke 3.6.4.
        send_error(env.requestId, eph::kErrMalformed, 0, 0, "compressed payloads are not served");
        return true;
    }
    const uint32_t bound = version_ ? ctx_.config().max_payload : eph::kPreSessionMaxPayload;
    if (env.payloadLen > bound) {
        send_error(env.requestId, eph::kErrLimits, 0, 0,
                   "payload over the advertised maximum (" + std::to_string(bound) + " bytes)");
        return true;
    }
    if (eph::CheckRequestId(env, &why) != eph::kOk) {
        send_error(env.requestId, eph::kErrMalformed, 0, 0, why);
        return true;
    }
    const uint8_t* payload = bytes + eph::kEnvelopeSize;

    switch (env.type) {
    case eph::kMsgHello: {
        eph::Hello h;
        if (eph::ParseHello(payload, env.payloadLen, &h, &why) != eph::kOk) {
            send_error(0, eph::kErrMalformed, 0, 0, "malformed HELLO: " + why);
            return true;
        }
        const uint8_t session = uint8_t(std::min<uint32_t>(h.protoMax, eph::kProtoVersion));
        if (session < std::max<uint32_t>(h.protoMin, eph::kProtoMin)) {
            // 3.3.3: no overlap; `session` is >= 4 here, so it is the version.
            version_ = session;
            send_error(0, eph::kErrVersion, eph::kErrFlagClosing, 0,
                       "no protocol version in common (this server: 4)");
            return false;
        }
        // The established session, fixed by the first HELLO (3.3.7).
        if (version_ == 0) {
            version_ = session;
        }
        if (Limits* limits = ctx_.limits()) {
            if (limits->token_known(h.token)) {
                budget_key_ = "t:" + h.token;
            } else if (limits->require_token()) {
                send_error(0, eph::kErrToken, eph::kErrFlagClosing, 0,
                           h.token.empty() ? "this server requires a token" : "unknown token");
                return false;
            }
        }
        ++ctx_.metrics().hellos;
        std::vector<uint8_t> welcome;
        build_welcome(welcome, ctx_.config(), version_);
        send(eph::kMsgWelcome, 0, welcome);
        return true;
    }
    case eph::kMsgPing:
        send(eph::kMsgPong, env.requestId, {});
        return true;
    case eph::kMsgPong:
        return true;
    case eph::kMsgRequest:
        if (version_ == 0) {
            send_error(env.requestId, eph::kErrMalformed, eph::kErrFlagClosing, 0,
                       "REQUEST before HELLO");
            return false;
        }
        return on_request(env, payload, env.payloadLen);
    case eph::kMsgLookup:
        if (version_ == 0) {
            send_error(env.requestId, eph::kErrMalformed, eph::kErrFlagClosing, 0,
                       "LOOKUP before HELLO");
            return false;
        }
        return on_lookup(env, payload, env.payloadLen);
    case eph::kMsgCancel:
        if (version_ == 0) {
            send_error(env.requestId, eph::kErrMalformed, eph::kErrFlagClosing, 0,
                       "CANCEL before HELLO");
            return false;
        }
        on_cancel(env.requestId);
        return true;
    case eph::kMsgWelcome:
    case eph::kMsgData:
    case eph::kMsgLookupResult:
    case eph::kMsgSegData:
        send_error(env.requestId, eph::kErrMalformed, 0, 0, "that message is server-to-client");
        return true;
    default:
        send_error(env.requestId, eph::kErrUnknownType, 0, 0, "unknown message type");
        return true;
    }
}

bool Session::on_request(const eph::Envelope& env, const uint8_t* payload, size_t len) {
    eph::Request q;
    std::string why;
    bool ignored_ext = false;
    switch (eph::ParseRequest(payload, len, &q, &why, &ignored_ext)) {
    case eph::kOk:
        break;
    case eph::kMalformed:
        send_error(env.requestId, eph::kErrMalformed, 0, 0, "malformed REQUEST: " + why);
        return true;
    default:
        send_error(env.requestId, eph::kErrUnsupported, 0, 0, "unsupported REQUEST: " + why);
        return true;
    }
    const ServerConfig& cfg = ctx_.config();
    const auto over = [&](const char* what, auto got, auto bound) {
        send_error(env.requestId, eph::kErrLimits, 0, 0,
                   std::string(what) + " " + std::to_string(got) + " is over WELCOME's bound " +
                       std::to_string(bound));
        return true;
    };
    if (q.objs.size() > cfg.max_objs) {
        return over("objects", q.objs.size(), cfg.max_objs);
    }
    if (q.nTime > cfg.max_rows) {
        return over("rows", q.nTime, cfg.max_rows);
    }
    if (uint64_t(q.objs.size()) * q.nTime > cfg.max_cells) {
        return over("cells", uint64_t(q.objs.size()) * q.nTime, cfg.max_cells);
    }
    if (q.profiles.size() > cfg.max_profiles) {
        return over("profiles", q.profiles.size(), cfg.max_profiles);
    }
    std::vector<ProfilePlan> plans;
    if (auto why_not = plan_profiles(q, plans)) {
        if (!why_not->empty()) {
            send_error(env.requestId, eph::kErrUnsupported, 0, 0, *why_not);
            return true;
        }
        ignored_ext = true; // an unknown precession model fell back
    }
    // The pins (3.5): answered only from the named snapshot.
    for (const eph::Tlv& e : q.ext) {
        std::string pin;
        if (!eph::TlvStr8(e, &pin)) {
            continue;
        }
        if (e.tag == eph::kReqTagEphemerisPin && pin != cfg.ephemeris_name) {
            send_error(env.requestId, eph::kErrSource, 0, 0,
                       "the request pins an ephemeris this server does not serve");
            return true;
        }
        if (e.tag == eph::kReqTagDatasetPin && pin != cfg.dataset_id) {
            send_error(env.requestId, eph::kErrSource, 0, 0,
                       "the request pins a dataset this server does not serve");
            return true;
        }
        if (e.tag == eph::kReqTagCatalogPin &&
            std::find(cfg.catalog_names.begin(), cfg.catalog_names.end(), pin) ==
                cfg.catalog_names.end()) {
            send_error(env.requestId, eph::kErrSource, 0, 0,
                       "the request pins a catalog this server does not serve");
            return true;
        }
    }
    if (streams_.size() >= cfg.max_queued_answers) {
        send_error(env.requestId, eph::kErrBusy, eph::kErrFlagRetryable, 250,
                   "too many answers computed and not yet read on this connection");
        return true;
    }
    const uint64_t cells = uint64_t(q.objs.size()) * q.nTime;
    if (Limits* limits = ctx_.limits()) {
        // Charged whether the answer is cached or not: which it will be is
        // not known yet.
        if (const double wait = limits->charge(budget_key_, cells); wait > 0.0) {
            char text[160];
            // Rounded up to the tenth: "0.0 s" for a 12 ms wait reads as "now".
            std::snprintf(text, sizeof text, "rate limited: %u cells a second; ask again in %.1f s",
                          limits->config().cells_per_sec, std::ceil(wait * 10.0) / 10.0);
            send_error(env.requestId, eph::kErrRateLimited, eph::kErrFlagRetryable,
                       uint32_t(std::ceil(wait * 1000.0)), text);
            return true;
        }
    }

    Stream s;
    s.request_id = env.requestId;
    s.priority = q.priority;
    s.precision = q.precision;
    s.segments = q.representation == 1;
    const uint32_t hint = q.chunkRows ? q.chunkRows : cfg.max_chunk_rows;
    s.chunk_rows = std::max<uint32_t>(1, std::min(hint, cfg.max_chunk_rows));
    s.ignored_ext = ignored_ext;
    if (!s.segments) {
        std::string key = cfg.dataset_id;
        key.push_back('\0');
        key.append(reinterpret_cast<const char*>(payload) + q.questionOffset,
                   len - q.questionOffset);
        if (auto hit = ctx_.cache().get(key)) {
            ++ctx_.metrics().cache_hits;
            s.answer = std::move(hit);
        } else {
            s.cache_key = std::move(key);
            s.computing = std::make_unique<SamplesComputer>(ctx_, std::move(q));
        }
    } else {
        SegmentsParams params;
        if (auto refusal = check_segments(q, cfg, params)) {
            send_error(env.requestId, refusal->first, 0, 0, refusal->second);
            return true;
        }
        s.fitting = std::make_unique<SegmentsComputer>(ctx_, std::move(q), params);
    }
    ++ctx_.metrics().requests;
    streams_.push_back(std::move(s));
    return true;
}

bool Session::on_lookup(const eph::Envelope& env, const uint8_t* payload, size_t len) {
    eph::Lookup l;
    std::string why;
    if (eph::ParseLookup(payload, len, &l, &why) != eph::kOk) {
        send_error(env.requestId, eph::kErrMalformed, 0, 0, "malformed LOOKUP: " + why);
        return true;
    }
    if (l.maxMatches > kLookupMax) {
        send_error(env.requestId, eph::kErrLimits, 0, 0,
                   "the lookup budget is over the advertised maximum");
        return true;
    }
    Engine& engine = ctx_.engine();
    eph::LookupResult lr;
    lr.sources = {kCatalogSource, kStarSource};
    bool truncated = false;
    int budget = l.maxMatches;
    for (const std::string& query : l.queries) {
        std::vector<eph::Match> ms;
        // Small bodies: exact designations and names through the engine's
        // index. Prefix search over 1.5M catalog names is not offered
        // (nothing less than an index serves it, and none is built).
        auto spkid = engine.lookup(query);
        if (spkid.ok()) {
            auto names = engine.names(spkid.value());
            eph::Match m;
            m.quality = names.ok() && !names.value().name.empty() &&
                                equals_ignore_case(query, names.value().name)
                            ? 0
                            : 1;
            m.sourceIdx = 0;
            m.obj.kind = eph::kObjBody;
            m.obj.naif = spkid.value();
            if (names.ok()) {
                m.canonicalName =
                    names.value().name.empty() ? names.value().designation : names.value().name;
                m.designation = names.value().designation;
            } else {
                m.canonicalName = "SPK-ID " + std::to_string(spkid.value());
            }
            ms.push_back(std::move(m));
        }
        // Stars, when asked for (flag bit 2), prefix when asked for (bit 0);
        // stars::lookup is best-first already.
        if (l.flags & 4) {
            for (const stars::Match& sm : stars::lookup(query, 512, (l.flags & 1) != 0)) {
                eph::Match m;
                const std::string canonical = stars::at(sm.index).name();
                m.quality = sm.quality == stars::MatchQuality::Prefix
                                ? 2
                                : (equals_ignore_case(query, canonical) ? 0 : 1);
                m.sourceIdx = 1;
                m.obj.kind = eph::kObjStar;
                m.obj.name = canonical;
                m.canonicalName = canonical;
                ms.push_back(std::move(m));
            }
        }
        std::stable_sort(ms.begin(), ms.end(), [](const eph::Match& a, const eph::Match& b) {
            return a.quality < b.quality;
        });
        if (int(ms.size()) > budget) {
            ms.resize(budget < 0 ? 0 : size_t(budget));
            truncated = true;
        }
        budget -= int(ms.size());
        lr.queries.push_back(std::move(ms));
    }
    lr.flags = truncated ? 1 : 0;
    std::vector<uint8_t> out;
    eph::EncodeLookupResult(&out, lr);
    send(eph::kMsgLookupResult, env.requestId, out);
    return true;
}

void Session::on_cancel(uint32_t request_id) {
    for (auto it = streams_.begin(); it != streams_.end(); ++it) {
        if (it->request_id != request_id) {
            continue;
        }
        // 3.4: unknown ids and requests answered completely get nothing; one
        // still being answered is stopped -- dropping the stream drops the
        // compute, which is what stops the work -- and its unsent chunks are
        // discarded with it. A cancelled request caches nothing: the samples
        // cache is only written by a compute that ran to its last row.
        const bool fully_sent = it->sent_all();
        streams_.erase(it);
        if (!fully_sent) {
            ++ctx_.metrics().cancels;
            send_error(request_id, eph::kErrCancelled, 0, 0, "cancelled");
        }
        return;
    }
}

Session::Stream* Session::pick(bool computing) {
    Stream* best = nullptr;
    for (Stream& s : streams_) {
        const bool ready =
            computing ? (s.computing != nullptr || s.fitting != nullptr) : s.sendable();
        if (ready && (!best || s.priority < best->priority)) {
            best = &s;
        }
    }
    return best;
}

bool Session::pending() const {
    if (!control_.empty()) {
        return true;
    }
    for (const Stream& s : streams_) {
        if (s.sendable()) {
            return true;
        }
    }
    return false;
}

bool Session::has_work() const {
    for (const Stream& s : streams_) {
        if (s.computing || s.fitting) {
            return true;
        }
    }
    return false;
}

void Session::work() {
    Stream* s = pick(true);
    if (!s) {
        return;
    }
    const auto until = Clock::now() + std::chrono::milliseconds(kComputeSliceMs);
    if (s->fitting) {
        if (!s->fitting->run_until(until)) {
            return;
        }
        if (s->fitting->fatal()) {
            // The answer cannot be assembled (the ayanamsa series a sidereal
            // profile owes would not fit); the request fails as a whole.
            const auto [code, text] = *s->fitting->fatal();
            const uint32_t id = s->request_id;
            streams_.erase(std::find_if(streams_.begin(), streams_.end(),
                                        [&](const Stream& x) { return &x == s; }));
            send_error(id, code, 0, 0, text);
            return;
        }
        s->seg_answer = s->fitting->take();
        s->fitting.reset();
        return;
    }
    if (!s->computing->run_until(until)) {
        return;
    }
    auto answer = s->computing->take();
    ctx_.finish(s->cache_key, answer, s->computing->cells(), s->computing->ms());
    s->answer = std::move(answer);
    s->computing.reset();
}

bool Session::next(std::vector<uint8_t>& out) {
    if (!control_.empty()) {
        out = std::move(control_.front());
        control_.pop_front();
        return true;
    }
    Stream* sp = pick(false);
    if (!sp) {
        return false;
    }
    Stream& s = *sp;
    if (!s.segments) {
        const Answer& a = *s.answer;
        const uint32_t rows = std::min(s.chunk_rows, a.n_time - s.next_row);
        const int n_cols = 6 + eph::PopCount(a.columns_present);

        eph::DataChunk d;
        d.chunkIndex = s.chunk_index;
        d.iTime = s.next_row;
        d.nRows = rows;
        d.totalRows = a.n_time;
        d.precision = s.precision;
        d.flags = uint8_t((s.next_row + rows >= a.n_time ? eph::kChunkLast : 0) |
                          (s.ignored_ext ? eph::kChunkIgnoredExt : 0) |
                          (s.chunk_index == 0 ? eph::kChunkMeta : 0));
        d.nObj = uint16_t(a.n_obj);
        d.columnsPresent = a.columns_present;
        if (s.chunk_index == 0) {
            d.sources = a.sources;
            d.meta = a.meta;
        }
        d.values.reserve(size_t(a.n_obj) * rows * n_cols);
        for (uint32_t o = 0; o < a.n_obj; ++o) {
            const double* begin = a.cols.data() + (size_t(o) * a.n_time + s.next_row) * n_cols;
            d.values.insert(d.values.end(), begin, begin + size_t(rows) * n_cols);
        }
        std::vector<uint8_t> payload;
        eph::EncodeData(&payload, d);
        out.clear();
        eph::WriteEnvelope(&out, eph::kMsgData, s.request_id, payload.size(), version_);
        out.insert(out.end(), payload.begin(), payload.end());
        s.next_row += rows;
        ++s.chunk_index;
        if (s.next_row >= a.n_time) {
            streams_.erase(std::find_if(streams_.begin(), streams_.end(),
                                        [&](const Stream& x) { return &x == &s; }));
        }
        return true;
    }

    // SEGDATA: objects in chunks under the payload bound, all metadata and
    // the ayanamsa series on chunk 0 (rowsOk counts segments; META and the
    // series are whole-answer facts, as DATA's are).
    const SegAnswer& a = *s.seg_answer;
    const size_t cap = std::min<size_t>(ctx_.config().max_payload, 1u << 20);
    size_t bytes = 64 + (s.chunk_index == 0 ? 256 : 0);
    if (s.chunk_index == 0) {
        for (const std::string& src : a.sources) {
            bytes += src.size() + 1;
        }
        bytes += size_t(a.n_obj) * 128;
        for (const eph::AyanSeries& series : a.ayan) {
            bytes += 6;
            for (const eph::AyanSeg& g : series.segs) {
                bytes += 32 + 8 * (size_t(g.degree) + 1);
            }
        }
    }
    uint32_t count = 0;
    for (uint32_t i = s.next_obj; i < a.n_obj; ++i) {
        size_t need = 4;
        for (const eph::Segment& g : a.segs[i]) {
            need += 40 + 24 * (size_t(g.degree) + 1);
        }
        if (count && bytes + need > cap) {
            break;
        }
        bytes += need;
        ++count;
    }
    eph::SegDataChunk d;
    d.chunkIndex = s.chunk_index;
    d.nObj = uint16_t(a.n_obj);
    d.iObj = s.next_obj;
    d.nObjChunk = uint16_t(count);
    d.flags = uint8_t((s.next_obj + count >= a.n_obj ? eph::kChunkLast : 0) |
                      (s.ignored_ext ? eph::kChunkIgnoredExt : 0) |
                      (s.chunk_index == 0 ? eph::kChunkMeta : 0));
    if (s.chunk_index == 0) {
        d.sources = a.sources;
        d.meta = a.meta;
        d.ayan = a.ayan;
    }
    d.segs.assign(a.segs.begin() + s.next_obj, a.segs.begin() + s.next_obj + count);
    std::vector<uint8_t> payload;
    eph::EncodeSegData(&payload, d);
    out.clear();
    eph::WriteEnvelope(&out, eph::kMsgSegData, s.request_id, payload.size(), version_);
    out.insert(out.end(), payload.begin(), payload.end());
    s.next_obj += count;
    ++s.chunk_index;
    if (s.next_obj >= a.n_obj) {
        streams_.erase(std::find_if(streams_.begin(), streams_.end(),
                                    [&](const Stream& x) { return &x == &s; }));
    }
    return true;
}

} // namespace prometheia::server

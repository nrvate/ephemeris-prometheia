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

#include "objects.hpp"
#include "prometheia/stars.hpp"
#include "prometheia/time.hpp"

namespace prometheia::server {
namespace {

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

// ---- WELCOME ----------------------------------------------------------------

void build_welcome(std::vector<uint8_t>& payload, const ServerConfig& cfg, uint8_t version) {
    eph::Welcome w;
    w.protoSession = version;
    w.caps = eph::kCapF32 | eph::kCapInstantLists | eph::kCapLookup | eph::kCapDeepSky;
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
    : engine_(std::move(engine)), config_(config), cache_(config.cache_bytes), limits_(limits),
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
    const auto t0 = std::chrono::steady_clock::now();
    auto computed = compute(req);
    metrics_->computed(
        uint64_t(req.objs.size()) * req.nTime,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    cache_.put(key, computed);
    return computed;
}

std::shared_ptr<const Answer> LoopContext::compute(const eph::Request& req) {
    auto ans = std::make_shared<Answer>();
    const uint32_t n_obj = uint32_t(req.objs.size());
    const uint32_t n_time = req.nTime;
    ans->n_obj = n_obj;
    ans->n_time = n_time;

    std::vector<ProfilePlan> plans;
    plan_profiles(req, plans); // on_request has already refused what fails here
    for (const ProfilePlan& p : plans) {
        ans->columns_present |= p.columns;
    }
    const int n_cols = 6 + eph::PopCount(ans->columns_present);
    ans->cols.assign(size_t(n_obj) * n_time * n_cols, std::numeric_limits<double>::quiet_NaN());
    ans->meta.assign(n_obj, eph::Meta{});

    // The request's delta T, when it owns it (3.5): installed on the engine
    // for the compute and restored after. The engine's own model serves the
    // canonical NaN.
    ConstantDeltaT constant(req.deltaTSec);
    TableDeltaT table(req);
    if (!req.deltaTTable.empty()) {
        engine_.set_delta_t_model(&table);
    } else if (!eph::IsCanonicalNaN(req.deltaTSec)) {
        engine_.set_delta_t_model(&constant);
    }
    struct Restore {
        Engine& engine;
        ~Restore() { engine.set_delta_t_model(nullptr); }
    } restore{engine_};

    // What each object resolved to, and how its rows went.
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
    std::vector<Slot> objects(n_obj);
    for (uint32_t o = 0; o < n_obj; ++o) {
        Slot& t = objects[o];
        t.kind = eph::ObjKind(req.objs[o].kind);
        const ProfilePlan& plan = plans[req.objs[o].profile];
        auto resolved = resolve_object(req.objs[o], engine_);
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

    // Time-major: every object at one instant before the next instant, so
    // the engine's per-instant work (observer, Sun, frames, nutation) is
    // shared across the objects. The answer does not depend on the order.
    for (uint32_t r = 0; r < n_time; ++r) {
        const eph::Time t = req.RowTime(r);
        const double jd = t.jd1 + t.jd2;
        const double dtd = (ans->columns_present & eph::kColDeltaT) ? delta_t_used(req, t) : 0.0;
        for (uint32_t o = 0; o < n_obj; ++o) {
            Slot& t2 = objects[o];
            if (!t2.resolved) {
                continue; // rows stay NaN
            }
            const ProfilePlan& plan = plans[req.objs[o].profile];
            double* row = ans->cols.data() + (size_t(o) * n_time + size_t(r)) * size_t(n_cols);
            auto res = calc_at(engine_, t2.obj, jd, req.timeScale, plan.opts);
            if (!res) {
                if (t2.first_failed_row == eph::kRowNone) {
                    t2.first_failed_row = r;
                    t2.first_err = clean_err_text(res.error().message);
                    t2.code = obj_err_of(res.error());
                }
                continue;
            }
            ++t2.rows_ok;
            const CalcResult& cr = res.value();
            if (t2.source_idx < 0) {
                auto it = std::find(ans->sources.begin(), ans->sources.end(), cr.provenance.source);
                if (it == ans->sources.end()) {
                    ans->sources.emplace_back(cr.provenance.source);
                    it = ans->sources.end() - 1;
                }
                t2.source_idx = int(it - ans->sources.begin());
            }
            if (cr.sigma_arcsec) {
                t2.any_sigma = true;
            }
            const bool speeds = plan.opts.speed;
            if (plan.rectangular) {
                for (int i = 0; i < 3; ++i) {
                    row[i] = cr.pos.xyz_au[i];
                    row[3 + i] = speeds ? cr.pos.vel_au_day[i] : 0.0;
                }
            } else {
                const bool no_dist = t2.obj.no_parallax;
                row[0] = cr.pos.lon_deg;
                row[1] = cr.pos.lat_deg;
                row[2] = no_dist ? 0.0 : cr.pos.dist_au;
                row[3] = speeds ? cr.pos.lon_speed : 0.0;
                row[4] = speeds ? cr.pos.lat_speed : 0.0;
                row[5] = speeds && !no_dist ? cr.pos.dist_speed : 0.0;
            }
            uint32_t extra = ans->columns_present;
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

    for (uint32_t o = 0; o < n_obj; ++o) {
        const Slot& t = objects[o];
        const ProfilePlan& plan = plans[req.objs[o].profile];
        eph::Meta& m = ans->meta[o];
        m.rowsOk = int32_t(t.rows_ok);
        m.errCode = t.why.empty() && t.first_err.empty() ? eph::kOErrNone : t.code;
        m.sourceIdx = t.source_idx >= 0 ? uint8_t(t.source_idx) : eph::kSourceNone;
        if (plan.opts.speed == false) {
            m.flags |= eph::kMetaNoSpeeds;
        }
        if (t.obj.no_parallax && t.resolved) {
            m.flags |= eph::kMetaNoDistance;
        }
        if (t.rows_ok > 0 && t.rows_ok < n_time) {
            m.flags |= eph::kMetaPartial;
        }
        if (t.any_sigma) {
            m.flags |= eph::kMetaHasSigma;
        }
        m.corrApplied =
            t.resolved ? corr_applied(eph::ObjKind(req.objs[o].kind), plan.opts, t.obj.is_sun) : 0;
        m.resolvedNaif = t.resolved && t.kind != eph::kObjStar ? t.obj.naif_id : eph::kNaifNone;
        m.firstFailedRow = t.first_failed_row;
        m.name = t.resolved ? t.obj.name : "";
        m.errText = !t.why.empty() ? t.why : t.first_err;
    }
    return ans;
}

// ---- Session ---------------------------------------------------------------

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
        // CANCEL is not advertised yet (it means nothing to a server that
        // computes a request whole); a client must not have sent it.
        send_error(env.requestId, eph::kErrUnsupported, 0, 0,
                   "CANCEL is not served by this server");
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
    if (q.representation == 1) {
        // Segments arrive with 4c; until then they are refused, and the
        // segments capability stays dark (3.9a.2).
        send_error(env.requestId, eph::kErrUnsupported, 0, 0,
                   "segments are not served by this server");
        return true;
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
    s.precision = q.precision;
    const uint32_t hint = q.chunkRows ? q.chunkRows : cfg.max_chunk_rows;
    s.chunk_rows = std::max<uint32_t>(1, std::min(hint, cfg.max_chunk_rows));
    s.ignored_ext = ignored_ext;
    try {
        s.answer = ctx_.answer(
            q, std::string_view(reinterpret_cast<const char*>(payload) + q.questionOffset,
                                len - q.questionOffset));
    } catch (const std::bad_alloc&) {
        send_error(env.requestId, eph::kErrInternal, 0, 0, "out of memory computing this request");
        return true;
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

bool Session::next(std::vector<uint8_t>& out) {
    if (!control_.empty()) {
        out = std::move(control_.front());
        control_.pop_front();
        return true;
    }
    if (streams_.empty()) {
        return false;
    }
    Stream& s = streams_.front();
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
        streams_.pop_front();
    }
    return true;
}

} // namespace prometheia::server

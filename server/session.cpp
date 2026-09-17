// SPDX-License-Identifier: GPL-2.0-or-later
#include "session.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

#include "prometheia/stars.hpp"

namespace prometheia::server {
namespace {

constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
constexpr size_t kDeliveryFieldsSize = 5; // RequestFixed's trailing precision + chunkRows

// What the request-wide fields ask for, resolved through the wire map once
// per request. `why` is set when no object of the request can be answered.
struct Plan {
    CalcOptions opts;
    bool time_tt = false;
    bool xyz = false;
    bool radians = false;
    std::string why;
};

Plan plan_of(const eph::Request& req, const WireMap& map) {
    Plan p;
    p.opts.sigma = false;
    p.opts.speed = true; // the columns always carry rates
    p.time_tt = (req.iflag & eph::kIflagTimeTT) != 0;
    // A central body: a nonzero center, or center 0 with kIflagCenter. Its
    // number is a wire body id like any object's.
    const bool centred = req.center != 0 || (req.iflag & eph::kIflagCenter);
    std::optional<WireBody> center_body;
    if (centred) {
        if (req.center < 0 || !(center_body = map.body(uint32_t(req.center)))) {
            p.why = "center " + std::to_string(req.center) + " has no wire-map entry";
            return p;
        }
    }
    bool helio = false, bary = false, topo = false, sidereal = false;
    bool j2000 = false, icrs = false, no_nutation = false;
    const auto low = uint32_t(req.iflag & 0xFFFFFFFFu);
    for (unsigned bit = 0; bit < 32; ++bit) {
        if (!(low & (1u << bit))) {
            continue;
        }
        switch (map.flag(bit)) {
        case FlagMeaning::None:
            p.why = "iflag bit " + std::to_string(bit) + " has no wire-map entry";
            return p;
        case FlagMeaning::Speed:
        case FlagMeaning::Ignore:
            break;
        case FlagMeaning::Heliocentric:
            helio = true;
            break;
        case FlagMeaning::Barycentric:
            bary = true;
            break;
        case FlagMeaning::Topocentric:
            topo = true;
            break;
        case FlagMeaning::Equatorial:
            p.opts.coords = Coords::Equatorial;
            break;
        case FlagMeaning::J2000:
            j2000 = true;
            break;
        case FlagMeaning::Icrs:
            icrs = true;
            break;
        case FlagMeaning::NoNutation:
            no_nutation = true;
            break;
        case FlagMeaning::TruePosition:
            p.opts.light_time = false;
            break;
        case FlagMeaning::NoAberration:
            p.opts.aberration = false;
            break;
        case FlagMeaning::NoDeflection:
            p.opts.deflection = false;
            break;
        case FlagMeaning::Astrometric:
            p.opts.aberration = p.opts.deflection = false;
            break;
        case FlagMeaning::Sidereal:
            sidereal = true;
            break;
        case FlagMeaning::Xyz:
            p.xyz = true;
            break;
        case FlagMeaning::Radians:
            p.radians = true;
            break;
        }
    }
    if (int(helio) + int(bary) + int(topo) + int(centred) > 1) {
        p.why = "more than one observer (helio, bary, topo, center)";
        return p;
    }
    p.opts.center = helio     ? Center::Heliocentric
                    : bary    ? Center::Barycentric
                    : topo    ? Center::Topocentric
                    : centred ? Center::Body
                              : Center::Geocentric;
    if (centred) {
        p.opts.center_body = center_body->naif_id;
    }
    if (topo) {
        p.opts.site.lon_rad = req.topoLon * kDeg2Rad;
        p.opts.site.lat_rad = req.topoLat * kDeg2Rad;
        p.opts.site.height_m = req.topoElv;
    }
    p.opts.frame = icrs          ? Frame::ICRF
                   : j2000       ? Frame::J2000
                   : no_nutation ? Frame::MeanOfDate
                                 : Frame::TrueOfDate;
    if (sidereal) {
        const std::optional<SiderealMode> zodiac = map.sidereal(req.sidMode);
        if (!zodiac) {
            p.why = "sidereal mode " + std::to_string(req.sidMode) + " has no wire-map entry";
            return p;
        }
        p.opts.sidereal = *zodiac;
        p.opts.sidereal_epoch_jtdb = req.sidT0;
        p.opts.sidereal_ayanamsa_deg = req.sidAyanOff;
    }
    return p;
}

void fill_nan(double* row) {
    std::fill(row, row + eph::kColsPerObj, std::numeric_limits<double>::quiet_NaN());
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

LoopContext::LoopContext(Engine engine, const WireMap& map, const ServerConfig& config,
                         Limits* limits, Metrics* metrics)
    : engine_(std::move(engine)), map_(map), config_(config), cache_(config.cache_bytes),
      limits_(limits), metrics_(metrics ? metrics : &own_metrics_) {}

std::shared_ptr<const Answer> LoopContext::answer(const eph::Request& req,
                                                  std::string_view payload) {
    const std::string key(payload.substr(0, payload.size() - kDeliveryFieldsSize));
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
    const auto n_obj = uint32_t(req.objs.size());
    const uint32_t n_time = req.nTime;
    ans->n_obj = n_obj;
    ans->n_time = n_time;
    ans->cols.assign(size_t(n_obj) * n_time * eph::kColsPerObj, 0.0);
    ans->meta.assign(size_t(n_obj) * eph::kDataMetaSize, 0);

    const Plan plan = plan_of(req, map_);
    const auto flags = int32_t(uint32_t(req.iflag & 0xFFFFFFFFu));

    // What each object resolves to, and how its rows went.
    struct Object {
        std::string why; // set: every row fails with this reason
        int naif_id = 0;
        bool star = false;        // a catalog star; naif_id holds its index
        bool orbit_point = false; // a node or apsis of naif_id
        OrbitPoint point = OrbitPoint::AscendingNode;
        OrbitElements elements = OrbitElements::Osculating;
        std::string name, first_error;
        bool any_ok = false;
    };
    std::vector<Object> objects(n_obj);
    for (uint32_t o = 0; o < n_obj; ++o) {
        const eph::ObjSpec& obj = req.objs[o];
        Object& t = objects[o];
        t.why = plan.why;
        if (!t.why.empty()) {
            continue;
        }
        std::optional<WireBody> body;
        if (obj.kind == eph::kObjStar) {
            // Any name or designation the catalog knows (docs/STARS.md).
            auto found = stars::find(obj.name);
            if (!found) {
                t.why = found.error().message;
            } else {
                t.star = true;
                t.naif_id = int(found.value());
                t.name = stars::at(found.value()).name();
            }
        } else if (!(body = map_.body(obj.id))) {
            t.why = "body " + std::to_string(obj.id) + " has no wire-map entry";
        } else {
            t.naif_id = body->naif_id;
            t.name = body->name.empty() ? "SPK-ID " + std::to_string(body->naif_id) : body->name;
            if (obj.kind == eph::kObjNodAps) {
                // parseRequest has checked point 1-4 and method 0-1.
                static constexpr OrbitPoint kPoints[] = {
                    OrbitPoint::AscendingNode, OrbitPoint::DescendingNode, OrbitPoint::Perihelion,
                    OrbitPoint::Aphelion};
                static constexpr const char* kSuffix[] = {" asc. node", " desc. node",
                                                          " perihelion", " aphelion"};
                t.orbit_point = true;
                t.point = kPoints[obj.point - eph::kPntNorthNode];
                t.elements =
                    obj.method == eph::kNodOscu ? OrbitElements::Osculating : OrbitElements::Mean;
                t.name += kSuffix[obj.point - eph::kPntNorthNode];
            }
        }
    }

    // Time-major: every object at one instant before the next instant, so
    // the engine's per-instant work (observer, Sun, frames, nutation) is
    // shared across the objects. The answer does not depend on the order.
    for (uint32_t r = 0; r < n_time; ++r) {
        // Row r's instant, exactly as Astrolog's client computes it.
        const double jd = req.jdStart + double(uint64_t(r) * uint64_t(req.stepSeconds)) / 86400.0;
        for (uint32_t o = 0; o < n_obj; ++o) {
            Object& t = objects[o];
            double* row = ans->cols.data() + (size_t(o) * n_time + r) * eph::kColsPerObj;
            if (!t.why.empty()) {
                fill_nan(row);
                continue;
            }
            auto res =
                t.star ? (plan.time_tt ? engine_.calc_star(size_t(t.naif_id), jd, plan.opts)
                                       : engine_.calc_star_ut(size_t(t.naif_id), jd, plan.opts))
                : t.orbit_point
                    ? (plan.time_tt
                           ? engine_.calc_orbit_point(t.naif_id, t.point, t.elements, jd, plan.opts)
                           : engine_.calc_orbit_point_ut(t.naif_id, t.point, t.elements, jd,
                                                         plan.opts))
                    : (plan.time_tt ? engine_.calc(t.naif_id, jd, plan.opts)
                                    : engine_.calc_ut(t.naif_id, jd, plan.opts));
            if (!res) {
                fill_nan(row);
                if (t.first_error.empty()) {
                    t.first_error = res.error().message;
                }
                continue;
            }
            t.any_ok = true;
            const Position& p = res.value().pos;
            if (plan.xyz) {
                row[0] = p.xyz_au[0];
                row[1] = p.xyz_au[1];
                row[2] = p.xyz_au[2];
                row[3] = p.vel_au_day[0];
                row[4] = p.vel_au_day[1];
                row[5] = p.vel_au_day[2];
            } else {
                const double k = plan.radians ? kDeg2Rad : 1.0;
                row[0] = p.lon_deg * k;
                row[1] = p.lat_deg * k;
                row[2] = p.dist_au;
                row[3] = p.lon_speed * k;
                row[4] = p.lat_speed * k;
                row[5] = p.dist_speed;
            }
        }
    }
    for (uint32_t o = 0; o < n_obj; ++o) {
        const Object& t = objects[o];
        const std::string& serr = !t.why.empty() ? t.why : t.first_error;
        eph::writeDataMeta(ans->meta.data() + size_t(o) * eph::kDataMetaSize, t.any_ok ? flags : -1,
                           t.any_ok ? flags : 0, serr.empty() ? nullptr : serr.c_str(),
                           t.any_ok ? t.name.c_str() : nullptr);
    }
    return ans;
}

// ---- Session ---------------------------------------------------------------

void Session::send(uint16_t type, uint32_t request_id, const uint8_t* payload, size_t len) {
    // Every message in the session's version once HELLO has fixed one.
    control_.push_back(eph::makeMessage(type, request_id, payload, len, 0,
                                        version_ ? version_ : eph::kProtoVersion));
}

void Session::send_error(uint32_t request_id, int32_t code, const std::string& text) {
    std::vector<uint8_t> payload(sizeof(eph::ErrorWire) + text.size() + 1);
    eph::putU32(payload.data(), request_id);
    eph::putI32(payload.data() + 4, code);
    std::copy(text.begin(), text.end(), payload.begin() + sizeof(eph::ErrorWire));
    ctx_.metrics().error(code);
    send(eph::kMsgError, request_id, payload.data(), payload.size());
}

bool Session::on_message(std::string_view message, bool binary) {
    if (!binary) {
        send_error(0, eph::kErrBad, "the protocol is binary frames only");
        return true;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(message.data());
    if (message.size() < eph::kEnvelopeSize) {
        send_error(0, eph::kErrBad, "message shorter than the envelope");
        return true;
    }
    const auto too_old = [this](unsigned client) {
        char text[160];
        std::snprintf(text, sizeof text,
                      "this client speaks protocol %u; this server needs %u to %u", client,
                      unsigned(eph::kProtoMin), unsigned(eph::kProtoVersion));
        return std::string(text);
    };
    eph::Envelope env{};
    if (!eph::parseEnvelope(bytes, &env)) {
        uint8_t old = 0;
        if (eph::envelopeVersionBelowMin(bytes, &old)) {
            // Refused in the client's own version, which it can read.
            version_ = old;
            send_error(0, eph::kErrVersion, too_old(old));
            return false;
        }
        send_error(0, eph::kErrBad, "bad magic or protocol version");
        return true;
    }
    if (env.flags & ~eph::kEnvFlagMask) {
        send_error(env.requestId, eph::kErrBad, "unknown envelope flag bits");
        return true;
    }
    if (env.flags & eph::kEnvFlagZstd) {
        send_error(env.requestId, eph::kErrBad, "compressed payloads are not supported");
        return true;
    }
    if (env.payloadLen > eph::kMaxPayload ||
        eph::kEnvelopeSize + size_t(env.payloadLen) != message.size()) {
        send_error(env.requestId, eph::kErrBad, "payload length mismatch");
        return true;
    }
    const uint8_t* payload = bytes + eph::kEnvelopeSize;

    switch (env.type) {
    case eph::kMsgHello: {
        eph::Hello hello;
        if (!eph::parseHello(payload, env.payloadLen, &hello)) {
            send_error(env.requestId, eph::kErrBad, "malformed HELLO");
            return true;
        }
        if (hello.protoVersion < eph::kProtoMin) {
            version_ = env.version;
            send_error(env.requestId, eph::kErrVersion, too_old(hello.protoVersion));
            return false;
        }
        // The lower of the two ends' highest versions, fixed by the first
        // HELLO; every HELLO is answered.
        if (version_ == 0) {
            version_ = uint8_t(std::min<uint32_t>(hello.protoVersion, eph::kProtoVersion));
        }
        if (Limits* limits = ctx_.limits()) {
            if (limits->token_known(hello.token)) {
                budget_key_ = "t:" + hello.token;
            } else if (limits->require_token()) {
                send_error(env.requestId, eph::kErrToken,
                           hello.token.empty() ? "this server requires a token" : "unknown token");
                return false;
            }
        }
        ++ctx_.metrics().hellos;
        uint8_t welcome[sizeof(eph::WelcomeWire) + 256];
        uint32_t len = 0;
        // swissephVersion: 0, this is not the Swiss Ephemeris.
        eph::buildWelcome(welcome, eph::kCapFloat32, 0, ctx_.config().max_cells, kServerVersion,
                          &len, version_);
        send(eph::kMsgWelcome, env.requestId, welcome, len);
        return true;
    }
    case eph::kMsgRequest:
        if (version_ == 0) {
            send_error(env.requestId, eph::kErrBad, "REQUEST before HELLO");
            return false;
        }
        return on_request(env, payload);
    case eph::kMsgPing:
        send(eph::kMsgPong, env.requestId, nullptr, 0);
        return true;
    case eph::kMsgPong:
        return true;
    default:
        send_error(env.requestId, eph::kErrUnknown, "unknown message type");
        return true;
    }
}

bool Session::on_request(const eph::Envelope& env, const uint8_t* payload) {
    eph::Request req;
    switch (eph::parseRequest(payload, env.payloadLen, &req)) {
    case eph::kParseOk:
        break;
    case eph::kParseBad:
        send_error(env.requestId, eph::kErrBad, "malformed REQUEST");
        return true;
    case eph::kParseLimits:
        send_error(env.requestId, eph::kErrLimits, "REQUEST exceeds the limits WELCOME advertised");
        return true;
    }
    const uint64_t cells = uint64_t(req.objs.size()) * req.nTime;
    if (cells > ctx_.config().max_cells) {
        send_error(env.requestId, eph::kErrLimits,
                   "REQUEST asks " + std::to_string(cells) + " cells; WELCOME's bound is " +
                       std::to_string(ctx_.config().max_cells));
        return true;
    }
    if (streams_.size() >= ctx_.config().max_queued_answers) {
        send_error(env.requestId, eph::kErrLimits,
                   "too many answers computed and not yet read on this connection");
        return true;
    }
    if (Limits* limits = ctx_.limits()) {
        // Charged whether the answer is cached or not: which it will be is
        // not known yet.
        if (const double wait = limits->charge(budget_key_, cells); wait > 0.0) {
            char text[160];
            // Rounded up to the tenth: "0.0 s" for a 12 ms wait reads as "now".
            std::snprintf(text, sizeof text, "rate limited: %u cells a second; ask again in %.1f s",
                          limits->config().cells_per_sec, std::ceil(wait * 10.0) / 10.0);
            send_error(env.requestId, eph::kErrRateLimited, text);
            return true;
        }
    }
    Stream s;
    s.request_id = env.requestId;
    s.precision = req.precision;
    const uint32_t hint = req.chunkRows ? req.chunkRows : eph::kMaxChunkRows;
    s.chunk_rows = std::max<uint32_t>(1, std::min(hint, eph::kMaxChunkRows));
    try {
        s.answer = ctx_.answer(
            req, std::string_view(reinterpret_cast<const char*>(payload), env.payloadLen));
    } catch (const std::bad_alloc&) {
        send_error(env.requestId, eph::kErrInternal, "out of memory computing this request");
        return true;
    }
    ++ctx_.metrics().requests;
    streams_.push_back(std::move(s));
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
    const size_t len = eph::dataPayloadSize(a.n_obj, rows, s.precision);
    out.assign(eph::kEnvelopeSize + len, 0);
    size_t written = 0;
    eph::writeDataChunk(out.data() + eph::kEnvelopeSize, len, s.chunk_index, s.next_row, rows,
                        a.n_time, a.n_obj, s.precision, a.meta.data(), a.cols.data(), &written);
    eph::writeEnvelope(out.data(), eph::kMsgData, s.request_id, 0, uint32_t(written), version_);
    s.next_row += rows;
    ++s.chunk_index;
    if (s.next_row >= a.n_time) {
        streams_.pop_front();
    }
    return true;
}

} // namespace prometheia::server

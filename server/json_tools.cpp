// SPDX-License-Identifier: GPL-2.0-or-later
#include "json_tools.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>

#include "objects.hpp"
#include "prometheia/hypotheticals.hpp"
#include "prometheia/stars.hpp"
#include "prometheia/time.hpp"
#include "zodiacs.hpp"

namespace prometheia::server::jsontools {
namespace {

// A topocentric site's height above the ellipsoid, metres (docs/JSON_API.md).
constexpr double kSiteMinHeightM = -12000.0, kSiteMaxHeightM = 100000.0;

// The first year of UTC with integer leap seconds.
constexpr int kUtcFromYear = 1972;

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// ---------------------------------------------------------------------------
// Names an agent uses, and what they mean.

struct Planet {
    const char* name;
    int naif;
};
// Mars to Pluto are their system barycentres: the JPL planetary files carry no
// other (docs/ENGINE.md), and a chart asks for exactly this.
constexpr Planet kPlanets[] = {{"sun", 10},    {"moon", 301},  {"mercury", 199}, {"venus", 299},
                               {"earth", 399}, {"mars", 4},    {"jupiter", 5},   {"saturn", 6},
                               {"uranus", 7},  {"neptune", 8}, {"pluto", 9}};

std::optional<int> planet_naif(std::string_view name) {
    const std::string n = lower(name);
    for (const Planet& p : kPlanets)
        if (n == p.name)
            return p.naif;
    return std::nullopt;
}

// The Moon's orbit points by the names astrologers give them.
struct LunarPoint {
    const char* name;
    uint8_t point;  // A.13
    uint8_t method; // A.14: 0 mean, 1 osculating, 2 interpolated ("natural")
};
constexpr LunarPoint kLunarPoints[] = {
    {"mean node", 0, 0},         {"north node", 0, 0},          {"mean north node", 0, 0},
    {"true node", 0, 1},         {"true north node", 0, 1},     {"south node", 1, 0},
    {"mean south node", 1, 0},   {"true south node", 1, 1},     {"lilith", 3, 0},
    {"black moon lilith", 3, 0}, {"mean lilith", 3, 0},         {"mean apogee", 3, 0},
    {"true lilith", 3, 1},       {"osculating lilith", 3, 1},   {"osculating apogee", 3, 1},
    {"mean perigee", 2, 0},      {"osculating perigee", 2, 1},  {"natural apogee", 3, 2},
    {"natural lilith", 3, 2},    {"interpolated apogee", 3, 2}, {"natural perigee", 2, 2},
    {"priapus", 2, 2},           {"interpolated perigee", 2, 2}};

struct PointName {
    const char* name;
    uint8_t point;
};
constexpr PointName kPointNames[] = {{"ascending-node", 0}, {"descending-node", 1},
                                     {"perihelion", 2},     {"perigee", 2},
                                     {"aphelion", 3},       {"apogee", 3}};

// ---------------------------------------------------------------------------
// Arguments.

// A bad argument, with the sentence that says which.
struct Bad {
    std::string message;
};

// ISO 8601 date and time, UTC or with an offset, to TT (the leap-second
// table; a date alone is 00:00 UTC). Years may be signed and wider than four
// digits.
std::optional<double> parse_utc(const std::string& s, Bad& bad) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sign = 1, oh = 0, om = 0;
    double sec = 0.0;
    size_t i = 0;
    const auto digits = [&](int n, int& out) {
        out = 0;
        for (int k = 0; k < n; ++k, ++i) {
            if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i])))
                return false;
            out = out * 10 + (s[i] - '0');
        }
        return true;
    };
    int ysign = 1;
    if (i < s.size() && (s[i] == '-' || s[i] == '+'))
        ysign = s[i++] == '-' ? -1 : 1;
    size_t ystart = i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
        y = y * 10 + (s[i++] - '0');
    const bool ok_date = i - ystart >= 4 && i < s.size() && s[i++] == '-' && digits(2, mo) &&
                         i < s.size() && s[i++] == '-' && digits(2, d);
    if (!ok_date) {
        bad.message = "a time must be ISO 8601, e.g. 1990-06-15T14:30:00+02:00";
        return std::nullopt;
    }
    y *= ysign;
    if (i < s.size() && (s[i] == 'T' || s[i] == ' ')) {
        ++i;
        if (!digits(2, h) || i >= s.size() || s[i++] != ':' || !digits(2, mi)) {
            bad.message = "a time must be ISO 8601, e.g. 1990-06-15T14:30:00+02:00";
            return std::nullopt;
        }
        if (i < s.size() && s[i] == ':') {
            ++i;
            size_t start = i;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.'))
                ++i;
            sec = std::atof(s.substr(start, i - start).c_str());
        }
        bool zoned = false;
        if (i < s.size() && s[i] == 'Z') {
            ++i;
            zoned = true;
        } else if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            zoned = true;
            sign = s[i++] == '-' ? -1 : 1;
            if (!digits(2, oh)) {
                bad.message = "a UTC offset must be +HH:MM";
                return std::nullopt;
            }
            if (i < s.size() && s[i] == ':')
                ++i;
            if (i < s.size() && !digits(2, om)) {
                bad.message = "a UTC offset must be +HH:MM";
                return std::nullopt;
            }
        }
        // A clock time with no zone was read as UTC until 2026-09-20, which
        // is the one wrong answer this surface could give without saying
        // anything: an agent holding "14:30 in Zurich" and forgetting the
        // offset got a chart two hours out, and the reply called it UTC. A
        // date alone stays 00:00 UTC -- that is a date, not a clock time.
        if (!zoned) {
            bad.message = "a clock time needs its UTC offset or a Z, e.g. "
                          "1990-06-15T14:30:00+02:00 or 1990-06-15T12:30:00Z; a local time "
                          "without one is ambiguous by up to a day";
            return std::nullopt;
        }
    }
    if (i != s.size() || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec >= 61.0) {
        bad.message = "a time must be ISO 8601, e.g. 1990-06-15T14:30:00+02:00";
        return std::nullopt;
    }
    // The wall clock less its offset is UTC; move it across the day through
    // a JD, then read it back (a leap second is only meaningful in Z).
    const int offset_min = sign * (oh * 60 + om);
    if (offset_min != 0) {
        const double jd = time::jd_from_ymdhms(y, mo, d, h, mi, sec) - offset_min / 1440.0;
        const time::Civil c = time::civil_from_jd(jd);
        const double frac = c.day - std::floor(c.day);
        y = c.year;
        mo = c.month;
        d = int(std::floor(c.day));
        const double secs = frac * 86400.0;
        h = int(secs / 3600.0);
        mi = int((secs - h * 3600.0) / 60.0);
        sec = secs - h * 3600.0 - mi * 60.0;
    }
    // Before 1972 there is no UTC with integer leap seconds: a civil clock
    // time then is read as UT1 (what UTC approximated, to under a second
    // from 1961), and reported back as "ut1", never "utc".
    if (y < kUtcFromYear) {
        if (sec >= 60.0) {
            bad.message = "there are no leap seconds before 1972";
            return std::nullopt;
        }
        return time::jd_tt_from_ut1(time::jd_from_ymdhms(y, mo, d, h, mi, sec));
    }
    auto tt = time::utc_to_tt(y, mo, d, h, mi, sec);
    if (!tt) {
        bad.message = tt.error().message;
        return std::nullopt;
    }
    return tt.value();
}

// A JD as ISO 8601 with a Z, to the millisecond: for UT1 before 1972.
std::string format_civil(double jd) {
    const double jdr = std::floor(jd * 86400000.0 + 0.5) / 86400000.0;
    const time::Civil c = time::civil_from_jd(jdr);
    const int day = int(std::floor(c.day));
    long long ms = std::llround((c.day - day) * 86400000.0);
    ms = std::min(ms, 86400000LL - 1);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s%04d-%02d-%02dT%02lld:%02lld:%02lld.%03lldZ",
                  c.year < 0 ? "-" : "", std::abs(c.year), c.month, day, ms / 3600000,
                  ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
    return buf;
}

std::string format_utc(double jd_tt) {
    auto u = time::tt_to_utc(jd_tt);
    if (!u)
        return {};
    const time::Utc& t = u.value();
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s%04d-%02d-%02dT%02d:%02d:%06.3fZ", t.year < 0 ? "-" : "",
                  std::abs(t.year), t.month, t.day, t.hour, t.minute, t.second);
    return buf;
}

// An instant as the replies give it: JD(TT), and UTC from 1972, or before it
// the UT1 a clock then kept.
Json time_of(double jd_tt) {
    const std::string utc = format_utc(jd_tt);
    if (!utc.empty())
        return {{"jd_tt", jd_tt}, {"utc", utc}};
    return {{"jd_tt", jd_tt}, {"ut1", format_civil(time::jd_ut1_from_tt(jd_tt))}};
}

// The instants a call asks for, TT.
std::optional<std::vector<double>> parse_times(const Json& a, const Limits& lim, Bad& bad) {
    const auto one = [&](const Json& t) -> std::optional<double> {
        if (t.is_string())
            return parse_utc(t.get<std::string>(), bad);
        if (t.is_object() && t.contains("utc") && t["utc"].is_string())
            return parse_utc(t["utc"].get<std::string>(), bad);
        if (t.is_object() && t.contains("jd_tt") && t["jd_tt"].is_number()) {
            const double jd = t["jd_tt"].get<double>();
            if (std::isfinite(jd))
                return jd;
        }
        if (t.is_object() && t.contains("jd_ut1") && t["jd_ut1"].is_number()) {
            const double jd = t["jd_ut1"].get<double>();
            if (std::isfinite(jd))
                return time::jd_tt_from_ut1(jd);
        }
        if (bad.message.empty())
            bad.message = "a time is an ISO 8601 UTC string, or {\"utc\": ...}, {\"jd_tt\": ...} "
                          "or {\"jd_ut1\": ...}";
        return std::nullopt;
    };
    std::vector<double> out;
    if (a.contains("time")) {
        auto t = one(a["time"]);
        if (!t)
            return std::nullopt;
        out.push_back(*t);
    } else if (a.contains("times") && a["times"].is_array()) {
        for (const Json& t : a["times"]) {
            auto v = one(t);
            if (!v)
                return std::nullopt;
            out.push_back(*v);
        }
    } else if (a.contains("series") && a["series"].is_object()) {
        const Json& g = a["series"];
        if (!g.contains("start") || !g.contains("step_days") || !g.contains("count") ||
            !g["step_days"].is_number() || !g["count"].is_number_integer()) {
            bad.message = "a series is {\"start\": time, \"step_days\": number, \"count\": n}";
            return std::nullopt;
        }
        auto start = one(g["start"]);
        if (!start)
            return std::nullopt;
        const long n = g["count"].get<long>();
        const double step = g["step_days"].get<double>();
        if (n < 1 || n > long(lim.max_times) || !std::isfinite(step)) {
            bad.message = "a series has 1 to " + std::to_string(lim.max_times) + " instants";
            return std::nullopt;
        }
        for (long k = 0; k < n; ++k)
            out.push_back(*start + double(k) * step);
    } else {
        bad.message = "give \"time\", \"times\" or \"series\"";
        return std::nullopt;
    }
    if (out.empty() || out.size() > lim.max_times) {
        bad.message = "1 to " + std::to_string(lim.max_times) + " instants";
        return std::nullopt;
    }
    return out;
}

// What one object of a call asked for, as the protocol's object spec.
struct Asked {
    Json asked;
    std::optional<eph::Object> spec; // empty: `why` says what was wrong
    std::string why, code;
};

// A body by name or number: a planet, then a catalog body.
std::optional<int> body_naif(Engine& engine, const Json& v) {
    if (v.is_number_integer())
        return v.get<int>();
    if (!v.is_string())
        return std::nullopt;
    const std::string n = v.get<std::string>();
    if (auto p = planet_naif(n))
        return p;
    auto found = engine.lookup(n);
    if (found)
        return found.value();
    return std::nullopt;
}

Asked parse_object(Engine& engine, const Json& o, size_t catalogs) {
    Asked a;
    a.asked = o;
    eph::Object spec{};
    if (o.is_string()) {
        // A bare name: a planet, a lunar point by its astrological name, a
        // hypothetical token, a star, a catalog body, in that order. The answer
        // says what it resolved to.
        const std::string n = o.get<std::string>();
        const std::string ln = lower(n);
        if (auto p = planet_naif(n)) {
            spec.kind = eph::kObjBody;
            spec.naif = *p;
        } else if (auto lp = std::find_if(std::begin(kLunarPoints), std::end(kLunarPoints),
                                          [&](const LunarPoint& q) { return ln == q.name; });
                   lp != std::end(kLunarPoints)) {
            spec.kind = eph::kObjOrbitPoint;
            spec.naif = 301;
            spec.point = lp->point;
            spec.method = lp->method;
        } else if (engine.hypothetical(ln)) {
            spec.kind = eph::kObjHypothetical;
            spec.name = ln;
        } else if (stars::find(n)) {
            spec.kind = eph::kObjStar;
            spec.name = n;
        } else if (engine.lookup(n)) {
            spec.kind = eph::kObjDesignation;
            spec.name = n;
        } else {
            a.why = catalogs != 0
                        ? "no planet, point, hypothetical body, star or catalog body has that name"
                        // Without a catalog every asteroid name lands here, and
                        // an agent told only "no ... catalog body has that name"
                        // will try spellings forever.
                        : "no planet, point, hypothetical body or star has that name, and this "
                          "server has no small-body catalog loaded (capabilities says so under "
                          "\"asteroids\")";
            a.code = "unknown-name";
            return a;
        }
        a.spec = spec;
        return a;
    }
    if (!o.is_object()) {
        a.why = "an object is a name, or {\"body\"|\"star\"|\"asteroid\"|\"hypothetical\"|"
                "\"naif\"|\"point\": ...}";
        a.code = "invalid-arguments";
        return a;
    }
    if (o.contains("naif") && o["naif"].is_number_integer()) {
        spec.kind = eph::kObjBody;
        spec.naif = o["naif"].get<int>();
    } else if (o.contains("body")) {
        auto n = body_naif(engine, o["body"]);
        if (!n) {
            a.why = "no body has that name";
            a.code = "unknown-name";
            return a;
        }
        spec.kind = eph::kObjBody;
        spec.naif = *n;
    } else if (o.contains("star") && o["star"].is_string()) {
        spec.kind = eph::kObjStar;
        spec.name = o["star"].get<std::string>();
    } else if (o.contains("asteroid")) {
        if (o["asteroid"].is_number_integer()) {
            spec.kind = eph::kObjDesignation;
            spec.name = std::to_string(o["asteroid"].get<long>());
        } else if (o["asteroid"].is_string()) {
            spec.kind = eph::kObjDesignation;
            spec.name = o["asteroid"].get<std::string>();
        }
    } else if (o.contains("hypothetical") && o["hypothetical"].is_string()) {
        spec.kind = eph::kObjHypothetical;
        spec.name = lower(o["hypothetical"].get<std::string>());
    } else if (o.contains("point") && o["point"].is_string()) {
        const std::string p = lower(o["point"].get<std::string>());
        auto pn = std::find_if(std::begin(kPointNames), std::end(kPointNames),
                               [&](const PointName& q) { return p == q.name; });
        if (pn == std::end(kPointNames)) {
            a.why = "a point is ascending-node, descending-node, perihelion (perigee) or "
                    "aphelion (apogee)";
            a.code = "invalid-arguments";
            return a;
        }
        auto of = body_naif(engine, o.contains("of") ? o["of"] : Json("Moon"));
        if (!of) {
            a.why = "no body has the name given as \"of\"";
            a.code = "unknown-name";
            return a;
        }
        const std::string m = o.contains("method") && o["method"].is_string()
                                  ? lower(o["method"].get<std::string>())
                                  : std::string("mean");
        if (m == "osculating-barycentric" || m == "focal-point" ||
            (m == "interpolated" && (*of != 301 || pn->point < 2))) {
            // A.14 names them; this engine computes mean and osculating
            // orbits, and the interpolated one for the Moon's apsides only,
            // and says so rather than answer with another.
            a.why = "the " + m +
                    " method is not served for this point; this engine serves mean and "
                    "osculating, and interpolated for the Moon's apogee and perigee";
            a.code = "unsupported";
            return a;
        }
        if (m != "mean" && m != "osculating" && m != "interpolated") {
            a.why = "a point's method is mean, osculating, interpolated, osculating-barycentric "
                    "or focal-point";
            a.code = "invalid-arguments";
            return a;
        }
        spec.kind = eph::kObjOrbitPoint;
        spec.naif = *of;
        spec.point = pn->point;
        spec.method = m == "mean" ? 0 : m == "osculating" ? 1 : 2;
    } else {
        a.why = "an object is a name, or {\"body\"|\"star\"|\"asteroid\"|\"hypothetical\"|"
                "\"naif\"|\"point\": ...}";
        a.code = "invalid-arguments";
        return a;
    }
    a.spec = spec;
    return a;
}

// Options, the same CalcOptions the binary server builds from a profile.
struct Options {
    CalcOptions opts;
    std::string frame_words, zodiac_token;
    bool sidereal = false;
    // What the answer says about where it was seen from. Provenance named
    // the frame, the corrections and the zodiac but never the observer, so
    // a topocentric answer and a geocentric one were indistinguishable
    // except by their numbers -- and an answer that travels (to Astrolog,
    // to a file, to another agent) loses the request that made it.
    std::string observer_words;
    Json observer_detail; // the site, or the body at whose centre
    // The other two arguments that moved the answer without appearing in it:
    // the sidereal plane (1.02 degrees of latitude between "date" and
    // "invariable") and the precession model (6 mas at 1600).
    std::string plane_token, precession_token;
    bool of_date = false; // precession entered the answer through the frame
};

std::optional<Options> parse_options(Engine& engine, const Json& a, Bad& bad) {
    Options out;
    CalcOptions& o = out.opts;
    const auto str = [&](const char* key, const char* dflt) {
        return a.contains(key) && a[key].is_string() ? lower(a[key].get<std::string>())
                                                     : std::string(dflt);
    };
    const std::string obs = str("observer", "geocentric");
    out.observer_words = obs;
    if (obs == "geocentric") {
        o.center = Center::Geocentric;
    } else if (obs == "heliocentric") {
        o.center = Center::Heliocentric;
    } else if (obs == "barycentric") {
        o.center = Center::Barycentric;
    } else if (obs == "topocentric") {
        const Json site = a.contains("site") ? a["site"] : Json::object();
        if (!site.is_object() || !site.contains("lon_deg") || !site.contains("lat_deg") ||
            !site["lon_deg"].is_number() || !site["lat_deg"].is_number()) {
            bad.message = "a topocentric observer needs \"site\": {\"lon_deg\", \"lat_deg\", "
                          "\"height_m\"}";
            return std::nullopt;
        }
        const double lon = site["lon_deg"].get<double>(), lat = site["lat_deg"].get<double>();
        const double h = site.contains("height_m") && site["height_m"].is_number()
                             ? site["height_m"].get<double>()
                             : (site.contains("height_m") ? NAN : 0.0);
        // A site is a place on or above the Earth: from the deepest trench to
        // the Karman line. Beyond it the "site" is a spacecraft, and far
        // beyond, light time from it runs centuries back: a small body was
        // then integrated that far before the call failed (30 s of CPU at
        // 4e19 m, fuzz_json, 2026-09-18).
        if (!(std::fabs(lon) <= 180.0) || !(std::fabs(lat) <= 90.0) ||
            !(h >= kSiteMinHeightM && h <= kSiteMaxHeightM)) {
            bad.message = "a site's longitude is within ±180°, its latitude within ±90°, and its "
                          "height_m within -12000..100000";
            return std::nullopt;
        }
        o.center = Center::Topocentric;
        o.site = {lon * 3.14159265358979323846 / 180.0, lat * 3.14159265358979323846 / 180.0, h};
        out.observer_detail = {{"lon_deg", lon}, {"lat_deg", lat}, {"height_m", h}};
    } else if (obs == "body") {
        // A.5's fifth observer: the centre of a body ("Jupiter as seen from
        // Io" is {"observer": "body", "center": "Io"}).
        auto c = a.contains("center") ? body_naif(engine, a["center"]) : std::nullopt;
        if (!c) {
            bad.message = "a body observer needs \"center\": a body's name or NAIF id";
            return std::nullopt;
        }
        o.center = Center::Body;
        o.center_body = *c;
        out.observer_detail = {{"naif", *c}};
    } else {
        bad.message = "the observer is geocentric, topocentric, heliocentric, barycentric or body";
        return std::nullopt;
    }
    const std::string frame = str("frame", "true-of-date");
    out.of_date = frame == "true-of-date" || frame == "mean-of-date";
    if (frame == "true-of-date") {
        o.frame = Frame::TrueOfDate;
        out.frame_words = "true equator/ecliptic and equinox of date";
    } else if (frame == "mean-of-date") {
        o.frame = Frame::MeanOfDate;
        out.frame_words = "mean equator/ecliptic and equinox of date";
    } else if (frame == "j2000") {
        o.frame = Frame::J2000;
        out.frame_words = "mean equator/ecliptic and equinox of J2000.0";
    } else if (frame == "icrf") {
        o.frame = Frame::ICRF;
        out.frame_words = "ICRF axes";
    } else {
        bad.message = "the frame is true-of-date, mean-of-date, j2000 or icrf";
        return std::nullopt;
    }
    const std::string coords = str("coordinates", "ecliptic");
    if (coords == "ecliptic") {
        o.coords = Coords::Ecliptic;
    } else if (coords == "equatorial") {
        o.coords = Coords::Equatorial;
    } else {
        bad.message = "coordinates are ecliptic or equatorial";
        return std::nullopt;
    }
    // Corrections: a name or a list of names.
    if (a.contains("corrections") && a["corrections"].is_array()) {
        o.light_time = o.deflection = o.aberration = false;
        for (const Json& c : a["corrections"]) {
            const std::string n = c.is_string() ? lower(c.get<std::string>()) : std::string();
            if (n == "light-time") {
                o.light_time = true;
            } else if (n == "gravitational-deflection" || n == "deflection") {
                o.deflection = true;
            } else if (n == "aberration") {
                o.aberration = true;
            } else {
                bad.message = "corrections are light-time, gravitational-deflection and "
                              "aberration";
                return std::nullopt;
            }
        }
    } else {
        const std::string c = str("corrections", "apparent");
        if (c == "apparent") {
            o.light_time = o.deflection = o.aberration = true;
        } else if (c == "astrometric") {
            o.light_time = true;
            o.deflection = o.aberration = false;
        } else if (c == "geometric") {
            o.light_time = o.deflection = o.aberration = false;
        } else {
            bad.message = "corrections are apparent, astrometric, geometric, or a list of "
                          "light-time, gravitational-deflection and aberration";
            return std::nullopt;
        }
    }
    if (a.contains("rates") && !a["rates"].is_boolean()) {
        bad.message = "rates is true or false";
        return std::nullopt;
    }
    o.speed = a.contains("rates") ? a["rates"].get<bool>() : true;
    const std::string prec = str("precession", "iau2006");
    out.precession_token = prec;
    if (prec == "iau2006") {
        o.precession = Precession::IAU2006;
    } else if (prec == "vondrak2011") {
        o.precession = Precession::Vondrak2011;
    } else {
        bad.message = "the precession is iau2006 or vondrak2011";
        return std::nullopt;
    }
    // The zodiac: tropical, an A.11 token, or a user anchor.
    if (a.contains("zodiac") && a["zodiac"].is_object() && a["zodiac"].contains("user")) {
        const Json& u = a["zodiac"]["user"];
        if (!u.is_object() || !u.contains("epoch_jd_tt") || !u.contains("ayanamsa_deg") ||
            !u["epoch_jd_tt"].is_number() || !u["ayanamsa_deg"].is_number()) {
            bad.message = "a user zodiac is {\"user\": {\"epoch_jd_tt\", \"ayanamsa_deg\"}} (the "
                          "MEAN ayanamsha at that TT epoch)";
            return std::nullopt;
        }
        o.sidereal = SiderealMode::User;
        o.sidereal_epoch_jtdb = u["epoch_jd_tt"].get<double>();
        o.sidereal_ayanamsa_deg = u["ayanamsa_deg"].get<double>();
        out.zodiac_token = "user";
        out.sidereal = true;
    } else {
        const std::string z = str("zodiac", "tropical");
        if (z != "tropical") {
            auto t = std::find_if(std::begin(kZodiacTokens), std::end(kZodiacTokens),
                                  [&](const ZodiacToken& q) { return z == q.token; });
            if (t == std::end(kZodiacTokens)) {
                bad.message = "that zodiac is not served; capabilities lists the zodiacs";
                return std::nullopt;
            }
            o.sidereal = t->mode;
            out.zodiac_token = z;
            out.sidereal = true;
        }
    }
    const std::string plane = str("sidereal_plane", "date");
    out.plane_token = plane;
    if (plane == "date") {
        o.sidereal_plane = SiderealPlane::EclipticOfDate;
    } else if (plane == "anchor" || plane == "invariable") {
        if (!out.sidereal) {
            bad.message = "a sidereal plane needs a sidereal zodiac";
            return std::nullopt;
        }
        if (plane == "anchor" && defined_at_instant(o.sidereal)) {
            bad.message = "the " + out.zodiac_token +
                          " zodiac is defined at the instant and has no anchor epoch, so no "
                          "anchor plane; it is counted along date or invariable";
            return std::nullopt;
        }
        o.sidereal_plane =
            plane == "anchor" ? SiderealPlane::EclipticOfAnchor : SiderealPlane::Invariable;
        out.frame_words = plane == "anchor"
                              ? "the mean ecliptic and equinox of the zodiac's anchor "
                                "epoch"
                              : "the invariable plane of the solar system";
    } else {
        bad.message = "the sidereal plane is date, anchor or invariable";
        return std::nullopt;
    }
    return out;
}

// ---------------------------------------------------------------------------
// What an answer says about itself.

// The corrections an answer actually carries: light time never for a star
// (positions are directions of arrival), deflection not at or of the Sun.
Json corrections_applied(const ResolvedObject& obj, const CalcOptions& o) {
    Json out = Json::array();
    const bool sun_observer = o.center == Center::Heliocentric ||
                              (o.center == Center::Body && o.center_body == body::kSun);
    if (o.light_time && obj.kind != ResolvedObject::Kind::Star)
        out.push_back("light-time");
    if (o.deflection && !sun_observer && !obj.is_sun)
        out.push_back("gravitational-deflection");
    if (o.aberration)
        out.push_back("aberration");
    return out;
}

// Measured accuracy, from the documents that measured it (never a promise).
Json accuracy(const ResolvedObject& obj) {
    switch (obj.kind) {
    case ResolvedObject::Kind::Star:
        return {{"statement", "apparent place agrees with ERFA (the IAU SOFA algorithms) to 0.34 "
                              "mas; the catalog is within 0.56\" of the FK5 over 1900-2100"},
                {"doc", "docs/STARS.md"}};
    case ResolvedObject::Kind::Hypothetical:
    case ResolvedObject::Kind::Elements:
        return {{"statement", "Keplerian positions from published elements; the Hamburg points "
                              "match swetest to 0.00076\""},
                {"doc", "docs/HYPOTHETICALS.md"}};
    case ResolvedObject::Kind::OrbitPoint:
        return {{"statement", "a point of the orbit, a convention rather than a body: its "
                              "definition (mean or osculating, the node of date) is in the doc"},
                {"doc", "docs/ORBIT-POINTS.md"}};
    case ResolvedObject::Kind::Body:
        break;
    }
    if (obj.naif_id >= 1000 || (obj.naif_id >= 2000000 && obj.naif_id < 3000000))
        return {{"statement",
                 "integrated from JPL SBDB elements: 0.04\" rms within 10 years of the "
                 "element epoch against JPL Horizons (0.005\" with JPL's asteroid "
                 "perturbers); sigma_arcsec where the catalog carries a covariance"},
                {"doc", "docs/VALIDATION.md"}};
    return {{"statement", "JPL planetary ephemeris; positions agree with JPL Horizons to 6 µas"},
            {"doc", "docs/VALIDATION.md"}};
}

Json kind_word(const ResolvedObject& obj) {
    switch (obj.kind) {
    case ResolvedObject::Kind::Body:
        return "body";
    case ResolvedObject::Kind::Star:
        return "star";
    case ResolvedObject::Kind::OrbitPoint:
        return "orbit-point";
    case ResolvedObject::Kind::Hypothetical:
        return "hypothetical";
    case ResolvedObject::Kind::Elements:
        return "elements";
    }
    return "body";
}

std::string error_code(const Error& e) {
    const std::string& m = e.message;
    if (m.find("coverage") != std::string::npos)
        return "outside-coverage";
    if (m.find("numerical failure") != std::string::npos ||
        m.find("integration failed") != std::string::npos)
        return "numerical-failure";
    if (m.find("ambiguous") != std::string::npos)
        return "ambiguous-name";
    if (e.code == ErrorCode::NotFound)
        return "unknown-name";
    if (e.code == ErrorCode::IoError || e.code == ErrorCode::FormatError ||
        e.code == ErrorCode::CorruptionError)
        return "data-unavailable";
    return "unsupported";
}

// ---------------------------------------------------------------------------
// The tools.

// Everything a tool reads. A key outside its list was ignored in silence
// until 2026-09-20, so an agent asking for "houses" or "aspects" -- neither
// of which this engine serves -- got positions back and no hint that half
// its request had gone nowhere, and a mistyped "prefix" on lookup quietly
// matched exactly and answered nothing. Same rule as a name: never a silent
// guess.
constexpr const char* kPositionsKeys[] = {
    "time",  "times",       "series",      "objects", "observer",       "site",       "center",
    "frame", "coordinates", "corrections", "zodiac",  "sidereal_plane", "precession", "rates"};

// True (and fills `err`) when the arguments carry a key the tool does not
// read. `reads` is the sentence that tells the caller what it does read.
template <size_t N>
bool has_unknown_key(const Json& a, const char* const (&keys)[N], const char* tool,
                     const char* reads, ToolError* err) {
    if (!a.is_object())
        return false;
    for (const auto& kv : a.items())
        if (std::find_if(std::begin(keys), std::end(keys),
                         [&](const char* k) { return kv.key() == k; }) == std::end(keys)) {
            *err = {"invalid-arguments",
                    "\"" + kv.key() + "\" is not an argument of " + tool + "; " + reads};
            return true;
        }
    return false;
}

// The same, for a tool that reads nothing at all.
bool has_any_key(const Json& a, const char* tool, ToolError* err) {
    if (!a.is_object() || a.empty())
        return false;
    *err = {"invalid-arguments", "\"" + a.items().begin().key() + "\" is not an argument of " +
                                     tool + "; it takes none"};
    return true;
}

Result<Json> positions(Engine& engine, const Context& ctx, const Json& a, ToolError* err) {
    Bad bad;
    if (has_unknown_key(a, kPositionsKeys, "positions",
                        "it reads time/times/series, objects, observer (site, center), frame, "
                        "coordinates, corrections, zodiac, sidereal_plane, precession and rates",
                        err))
        return make_error(ErrorCode::ArgumentError, err->message);
    auto times = parse_times(a, ctx.limits, bad);
    if (!times) {
        *err = {"invalid-arguments", bad.message};
        return make_error(ErrorCode::ArgumentError, bad.message);
    }
    auto options = parse_options(engine, a, bad);
    if (!options) {
        *err = {"invalid-arguments", bad.message};
        return make_error(ErrorCode::ArgumentError, bad.message);
    }
    if (!a.contains("objects") || !a["objects"].is_array() || a["objects"].empty() ||
        a["objects"].size() > ctx.limits.max_objects) {
        const std::string m =
            "\"objects\" is a list of 1 to " + std::to_string(ctx.limits.max_objects) + " objects";
        *err = {"invalid-arguments", m};
        return make_error(ErrorCode::ArgumentError, m);
    }
    const CalcOptions& o = options->opts;
    Json results = Json::array();
    for (const Json& item : a["objects"]) {
        Json r;
        Asked asked = parse_object(engine, item, ctx.catalogs);
        r["object"] = {{"asked", item}};
        if (!asked.spec) {
            r["error"] = {{"code", asked.code}, {"message", asked.why}};
            results.push_back(r);
            continue;
        }
        auto resolved = resolve_object(*asked.spec, engine);
        if (!resolved) {
            r["error"] = {{"code", error_code(resolved.error())},
                          {"message", resolved.error().message}};
            results.push_back(r);
            continue;
        }
        const ResolvedObject& obj = resolved.value();
        r["object"]["resolved"] = obj.name;
        r["object"]["kind"] = kind_word(obj);
        if (obj.kind == ResolvedObject::Kind::Body || obj.kind == ResolvedObject::Kind::OrbitPoint)
            r["object"]["naif"] = obj.naif_id;
        Json rows = Json::array();
        std::string source;
        std::optional<Error> failure;
        for (double jd : *times) {
            auto c = calc_at(engine, obj, jd, eph::kTimeTT, o);
            if (!c) {
                failure = c.error();
                break;
            }
            const CalcResult& cr = c.value();
            if (source.empty())
                source = std::string(cr.provenance.source);
            Json row;
            row["time"] = time_of(jd);
            const bool ecl = o.coords == Coords::Ecliptic;
            row[ecl ? "longitude_deg" : "right_ascension_deg"] = cr.pos.lon_deg;
            row[ecl ? "latitude_deg" : "declination_deg"] = cr.pos.lat_deg;
            row["distance_au"] = obj.no_parallax ? Json(nullptr) : Json(cr.pos.dist_au);
            if (o.speed) {
                row["rates"] = {
                    {ecl ? "longitude_deg_per_day" : "right_ascension_deg_per_day",
                     cr.pos.lon_speed},
                    {ecl ? "latitude_deg_per_day" : "declination_deg_per_day", cr.pos.lat_speed},
                    {"distance_au_per_day",
                     obj.no_parallax ? Json(nullptr) : Json(cr.pos.dist_speed)}};
            }
            if (cr.ayanamsa_deg)
                row["ayanamsa_deg"] = *cr.ayanamsa_deg;
            if (cr.sigma_arcsec)
                row["sigma_arcsec"] = *cr.sigma_arcsec;
            if (o.light_time && obj.kind != ResolvedObject::Kind::Star)
                row["light_time_days"] = cr.provenance.light_time_days;
            rows.push_back(row);
        }
        if (failure) {
            r["error"] = {{"code", error_code(*failure)}, {"message", failure->message}};
            if (!rows.empty())
                r["rows"] = rows; // the instants before the failure
            results.push_back(r);
            continue;
        }
        r["rows"] = rows;
        Json prov = {{"source", source},
                     {"observer", options->observer_words},
                     {"corrections", corrections_applied(obj, o)},
                     {"frame", options->frame_words},
                     {"coordinates", o.coords == Coords::Ecliptic ? "ecliptic" : "equatorial"},
                     {"accuracy", accuracy(obj)}};
        if (!options->observer_detail.is_null())
            prov[options->observer_words == "body" ? "center" : "site"] = options->observer_detail;
        if (options->sidereal)
            prov["zodiac"] = {{"token", options->zodiac_token},
                              {"plane", options->plane_token},
                              {"doc", "docs/FRAMES.md (zodiacs) and docs/ENGINE.md (ayanamshas)"}};
        // Only where it entered: a tropical answer in ICRF or J2000 axes is
        // the same number under either model, and naming one there would
        // claim a dependence the answer does not have.
        if (options->of_date || options->sidereal)
            prov["precession"] = options->precession_token;
        if (obj.no_parallax)
            prov["no_distance"] = true;
        r["provenance"] = prov;
        r["error"] = nullptr;
        results.push_back(r);
    }
    Json out = {{"engine", ctx.engine}, {"results", results}};
    if (!ctx.dataset.empty())
        out["dataset"] = ctx.dataset;
    return out;
}

constexpr const char* kLookupKeys[] = {"query", "prefix"};

Result<Json> lookup(Engine& engine, const Json& a, ToolError* err) {
    if (has_unknown_key(a, kLookupKeys, "lookup", "it reads query and prefix", err))
        return make_error(ErrorCode::ArgumentError, err->message);
    if (!a.contains("query") || !a["query"].is_string() || a["query"].get<std::string>().empty()) {
        *err = {"invalid-arguments", "\"query\" is a non-empty name"};
        return make_error(ErrorCode::ArgumentError, "no query");
    }
    const std::string q = a["query"].get<std::string>();
    const bool prefix = a.contains("prefix") && a["prefix"].is_boolean() && a["prefix"].get<bool>();
    const std::string lq = lower(q);
    // `prefix` reached the star index and nothing else until 2026-09-20, so
    // "node" and "Lili" found a star and never "true node" or "lilith": the
    // half-remembered name the flag exists for was the one case it could not
    // answer. Every name this server holds in a list is matched the same way
    // now. A catalog body is not in a list -- the index resolves a name, it
    // does not enumerate -- so it stays exact, and capabilities says so.
    // A prefix hit is the start of the name or the start of any word in it.
    // The star index anchors at the name's start, which suits a single token
    // like "Aldebaran"; these names are phrases whose distinguishing word is
    // usually last -- "mean node", "true node", "natural apogee" -- so an
    // anchored match answers "true" and never "node", which is the word the
    // agent remembers.
    const auto hit = [&](std::string_view name) {
        const std::string ln = lower(name);
        if (!prefix)
            return lq == ln;
        for (size_t at = 0; at != std::string::npos; at = ln.find(' ', at + 1))
            if (ln.compare(at == 0 ? 0 : at + 1, lq.size(), lq) == 0)
                return true;
        return false;
    };
    Json matches = Json::array();
    for (const Planet& p : kPlanets)
        if (hit(p.name))
            matches.push_back({{"object", {{"body", p.name}}}, {"kind", "body"}, {"naif", p.naif}});
    for (const LunarPoint& lp : kLunarPoints)
        if (hit(lp.name))
            matches.push_back({{"object", lp.name}, {"kind", "orbit-point"}, {"of", "Moon"}});
    for (const std::string& h : engine.hypothetical_tokens())
        if (hit(h))
            matches.push_back({{"object", {{"hypothetical", h}}}, {"kind", "hypothetical"}});
    for (const stars::Match& m : stars::lookup(q, 8, prefix)) {
        const stars::Object& s = stars::at(m.index);
        matches.push_back({{"object", {{"star", s.name()}}},
                           {"kind", "star"},
                           {"matched", m.matched},
                           {"designations", s.designations()}});
    }
    if (auto found = engine.lookup(q)) {
        Json m = {{"object", {{"asteroid", q}}}, {"kind", "body"}, {"naif", found.value()}};
        if (auto names = engine.names(found.value()))
            m["name"] = names.value().name.empty() ? names.value().designation : names.value().name;
        matches.push_back(m);
    }
    return Json{{"query", q}, {"matches", matches}};
}

Result<Json> capabilities(Engine& engine, const Context& ctx, const Json& a, ToolError* err) {
    if (has_any_key(a, "capabilities", err))
        return make_error(ErrorCode::ArgumentError, err->message);
    // Each zodiac with how it is defined and the planes it can be counted
    // along: one defined at the instant has no anchor epoch, so no "anchor"
    // plane (protocol v4 §3.5a).
    Json zodiacs = Json::array({{{"name", "tropical"},
                                 {"defined", "equinox of date"},
                                 {"sidereal_planes", Json::array()}}});
    const auto zodiac = [](const char* name, bool instant) {
        return Json{{"name", name},
                    {"defined", instant ? "at the instant" : "at an anchor epoch"},
                    {"sidereal_planes", instant ? Json::array({"date", "invariable"})
                                                : Json::array({"date", "anchor", "invariable"})}};
    };
    for (const ZodiacToken& z : kZodiacTokens)
        zodiacs.push_back(zodiac(z.token, defined_at_instant(z.mode)));
    zodiacs.push_back(zodiac("user", false));
    Json planets = Json::array();
    for (const Planet& p : kPlanets)
        planets.push_back({{"name", std::string(1, char(std::toupper(p.name[0]))) + (p.name + 1)},
                           {"naif", p.naif}});
    Json lunar = Json::array();
    for (const LunarPoint& lp : kLunarPoints)
        lunar.push_back(lp.name);
    Json out = {
        {"engine", ctx.engine},
        {"interfaces",
         "this JSON/MCP surface is a convenience for agents and scripts; bulk and high-speed data "
         "use the C++ library, the C API or prometheiad's binary protocol"},
        {"planets", planets},
        {"lunar_points", lunar},
        {"hypotheticals", engine.hypothetical_tokens()},
        {"stars", "the naked-eye sky, about 9,100 stars and the Messier objects, by name, Bayer "
                  "or Flamsteed designation, or HR/HD/HIP number"},
        // Whether a catalog is loaded is a property of this deployment, not
        // of the engine, and it is the difference between "Chiron is not
        // served here" and "Chiron is misspelled". Saying it here is what
        // lets an agent stop guessing.
        {"asteroids",
         {{"loaded", ctx.catalogs != 0},
          {"catalogs", ctx.catalogs},
          {"note", ctx.catalogs != 0
                       ? "catalog bodies by name, designation or number; matched exactly"
                       : "no small-body catalog is loaded on this server, so no asteroid name "
                         "resolves; it is served with --catalog"}}},
        {"zodiacs", zodiacs},
        {"frames", {"true-of-date", "mean-of-date", "j2000", "icrf"}},
        {"coordinates", {"ecliptic", "equatorial"}},
        {"observers", {"geocentric", "topocentric", "heliocentric", "barycentric", "body"}},
        {"corrections",
         {"apparent", "astrometric", "geometric", "light-time", "gravitational-deflection",
          "aberration"}},
        {"orbit_methods",
         {{"served", {"mean", "osculating", "interpolated (the Moon's apogee and perigee)"}},
          {"not_served", {"osculating-barycentric", "focal-point"}}}},
        {"precession", {"iau2006", "vondrak2011"}},
        {"limits", {{"max_objects", ctx.limits.max_objects}, {"max_times", ctx.limits.max_times}}}};
    if (!ctx.dataset.empty())
        out["dataset"] = ctx.dataset;
    return out;
}

constexpr const char* kConvertTimeKeys[] = {"time"};

Result<Json> convert_time(const Json& a, ToolError* err) {
    if (has_unknown_key(a, kConvertTimeKeys, "convert_time", "it reads time", err))
        return make_error(ErrorCode::ArgumentError, err->message);
    Bad bad;
    Limits one;
    one.max_times = 1;
    Json args = a;
    if (!args.contains("time")) {
        *err = {"invalid-arguments", "give \"time\": an ISO 8601 UTC string, or {\"jd_tt\"} or "
                                     "{\"jd_ut1\"}"};
        return make_error(ErrorCode::ArgumentError, "no time");
    }
    auto t = parse_times(args, one, bad);
    if (!t) {
        *err = {"invalid-arguments", bad.message};
        return make_error(ErrorCode::ArgumentError, bad.message);
    }
    const double tt = t->front();
    const double dt = time::delta_t(tt);
    Json out = time_of(tt);
    out.update(Json{{"jd_tt", tt},
                    {"jd_tdb", time::tdb_from_tt(tt)},
                    {"jd_ut1", time::jd_ut1_from_tt(tt)},
                    {"delta_t_s", dt},
                    {"notes", "TT-UT1 (delta T) is observed from 1657 to the present month, a "
                              "reconstruction before and a trend after (docs/TIME.md). Before "
                              "1972 a clock time is read as UT1, and given back as \"ut1\""}});
    return out;
}

Json schema_positions() {
    const Json time = {{"description", "ISO 8601 clock time with offset (e.g. "
                                       "\"1990-06-15T14:30:00+02:00\"; UTC from 1972, UT1 before), "
                                       "or {\"jd_tt\": n} or {\"jd_ut1\": n}"}};
    return {
        {"type", "object"},
        {"properties",
         {{"time", time},
          {"times", {{"type", "array"}, {"items", time}, {"description", "several instants"}}},
          {"series",
           {{"type", "object"},
            {"description", "evenly spaced instants"},
            {"properties",
             {{"start", time},
              {"step_days", {{"type", "number"}}},
              {"count", {{"type", "integer"}, {"minimum", 1}}}}}}},
          {"objects",
           {{"type", "array"},
            {"description", "names (\"Sun\", \"Mars\", \"true node\", \"Lilith\", \"Spica\", "
                            "\"Ceres\", \"cupido\") or {\"body\"|\"star\"|\"asteroid\"|"
                            "\"hypothetical\"|\"naif\": ...} or {\"point\": \"ascending-node\"|"
                            "\"descending-node\"|\"perihelion\"|\"aphelion\", \"of\": body, "
                            "\"method\": \"mean\"|\"osculating\"|\"interpolated\"} (the "
                            "last for the Moon's apogee and perigee: the natural apsides)"},
            {"minItems", 1}}},
          {"observer",
           {{"type", "string"},
            {"enum", {"geocentric", "topocentric", "heliocentric", "barycentric", "body"}},
            {"default", "geocentric"}}},
          {"center",
           {{"description", "for a body observer: the body's name or NAIF id (\"Jupiter\", 599)"}}},
          {"site",
           {{"type", "object"},
            {"description", "for a topocentric observer"},
            {"properties",
             {{"lon_deg", {{"type", "number"}}},
              {"lat_deg", {{"type", "number"}}},
              {"height_m",
               {{"type", "number"},
                {"minimum", kSiteMinHeightM},
                {"maximum", kSiteMaxHeightM}}}}}}},
          {"zodiac",
           {{"description", "\"tropical\" (default), a zodiac token (see capabilities), or "
                            "{\"user\": {\"epoch_jd_tt\", \"ayanamsa_deg\"}}"}}},
          {"sidereal_plane",
           {{"type", "string"},
            {"enum", {"date", "anchor", "invariable"}},
            {"default", "date"},
            {"description", "a zodiac defined at the instant has no anchor plane; capabilities "
                            "lists each zodiac's planes"}}},
          {"frame",
           {{"type", "string"},
            {"enum", {"true-of-date", "mean-of-date", "j2000", "icrf"}},
            {"default", "true-of-date"}}},
          {"coordinates",
           {{"type", "string"}, {"enum", {"ecliptic", "equatorial"}}, {"default", "ecliptic"}}},
          {"corrections",
           {{"description", "\"apparent\" (default), \"astrometric\", \"geometric\", or a list of "
                            "\"light-time\", \"gravitational-deflection\", \"aberration\""}}},
          {"rates", {{"type", "boolean"}, {"default", true}}},
          {"precession",
           {{"type", "string"}, {"enum", {"iau2006", "vondrak2011"}}, {"default", "iau2006"}}}}},
        {"required", {"objects"}}};
}

} // namespace

std::vector<Tool> tools() {
    return {
        {"positions", "Positions",
         "Positions of the Sun, Moon, planets, lunar points, stars, asteroids and hypothetical "
         "bodies at one instant or a short series: ecliptic or equatorial, tropical or sidereal, "
         "from any observer. Every answer names what each object resolved to, the corrections "
         "applied, the frame, and a measured accuracy statement.",
         schema_positions()},
        {"lookup",
         "Lookup",
         "What a name could mean: planets, lunar points, hypothetical bodies, stars (by name or "
         "designation) and catalog bodies.",
         {{"type", "object"},
          {"properties",
           {{"query", {{"type", "string"}}},
            {"prefix",
             {{"type", "boolean"},
              {"default", false},
              {"description", "match names beginning with the query: for a star, the start of "
                              "the name; for a planet, lunar point or hypothetical body, the "
                              "start of any word in it, so \"node\" finds \"true node\". A "
                              "catalog body is always matched exactly."}}}}},
          {"required", {"query"}}}},
        {"capabilities",
         "Capabilities",
         "What this engine answers: bodies, lunar points, hypothetical bodies, zodiacs, frames, "
         "observers, corrections and limits.",
         {{"type", "object"}, {"properties", Json::object()}}},
        {"convert_time",
         "Convert time",
         "One instant in UTC, TT, TDB and UT1, with delta T, using the leap-second table.",
         {{"type", "object"},
          {"properties",
           {{"time",
             {{"description", "ISO 8601 clock time (UTC from 1972, UT1 before), or "
                              "{\"jd_tt\"} or {\"jd_ut1\"}"}}}}},
          {"required", {"time"}}}},
    };
}

namespace {

Result<Json> call_checked(Engine& engine, const Context& ctx, std::string_view tool,
                          const Json& args, ToolError* err) {
    const Json a = args.is_object() ? args : Json::object();
    if (!args.is_object() && !args.is_null()) {
        *err = {"invalid-arguments", "the arguments are a JSON object"};
        return make_error(ErrorCode::ArgumentError, err->message);
    }
    if (tool == "positions")
        return positions(engine, ctx, a, err);
    if (tool == "lookup")
        return lookup(engine, a, err);
    if (tool == "capabilities")
        return capabilities(engine, ctx, a, err);
    if (tool == "convert_time")
        return convert_time(a, err);
    *err = {"unknown-tool", "no tool is called that"};
    return make_error(ErrorCode::NotFound, err->message);
}

} // namespace

Result<Json> call(Engine& engine, const Context& ctx, std::string_view tool, const Json& args,
                  ToolError* error) {
    ToolError scratch;
    ToolError* err = error ? error : &scratch;
    // The tools check each argument's type before reading it; this is the net
    // under a check that was missed, so a wrong type is the caller's error and
    // never an exception out of the server.
    try {
        return call_checked(engine, ctx, tool, args, err);
    } catch (const nlohmann::json::exception& e) {
        *err = {"invalid-arguments", std::string("an argument has the wrong type: ") + e.what()};
        return make_error(ErrorCode::ArgumentError, err->message);
    }
}

std::string llms_txt() {
    return R"(# Prometheia (JSON and MCP)

> A cleanroom astronomical ephemeris for astrology and astronomy: JPL DE440
> planets, the Moon's points, the naked-eye stars, asteroids from JPL's catalog,
> hypothetical bodies, tropical and sidereal zodiacs. This JSON/MCP surface is
> a convenience for agents; bulk data uses the C++ library, the C API or
> prometheiad's binary protocol.

## Tools

- positions: objects by name ("Sun", "Moon", "Mars", "true node", "Lilith",
  "Spica", "Ceres", "cupido") at "time" (ISO 8601 clock time with its offset
  or Z, e.g. "1990-06-15T14:30:00+02:00": UTC from 1972, UT1 before it, and
  the reply says which), "times" or "series". A clock time with no offset
  and no Z is refused -- a local time is ambiguous by up to a day, so send
  the offset or convert it yourself. A bare date is 00:00 UTC. Defaults are
  what a chart wants: apparent, geocentric, the ecliptic of date, tropical,
  with rates. Options: observer (topocentric with "site", or "body" with
  "center"), zodiac (e.g. "lahiri"), sidereal_plane, frame, coordinates,
  corrections, precession. An argument no tool here reads is refused by
  name rather than ignored -- this engine serves positions, not houses or
  aspects, and a mistyped option is a refusal, not a silent default.
- lookup: what a name could mean; with "prefix" it also matches a name by
  its start, or by the start of any word in it ("node" finds "true node").
- capabilities: what is served, and the limits. Ask it before an asteroid
  name: catalog bodies answer only when this server was given a catalog,
  and "asteroids" there says whether one is loaded.
- convert_time: UTC, TT, TDB, UT1 and delta T for one instant.

## Reading an answer

Each result names the object it resolved to (check it), its rows (units in
the field names), and provenance: source, observer (with the site or the
centre body that produced it), corrections applied, frame, zodiac (with the
plane it is counted along), the precession model where it entered, and a
measured accuracy statement with the document that measured it. An answer
carries where it was seen from, so it still says what it is once it has
travelled away from the request that asked for it. A
failure is a per-object error with a code (unknown-name, ambiguous-name,
outside-coverage, unsupported, numerical-failure) and a sentence; never a
guess.

## Conventions

- Longitudes in degrees [0, 360); rates per day; distances in AU.
- Mars to Pluto are their system barycentres (JPL's planetary files).
- "true node" is the osculating lunar node, "mean node" the mean one; "Lilith"
  is the mean lunar apogee, "true Lilith" the osculating one, "natural apogee"
  (or "natural Lilith") the interpolated one: between the Moon's actual
  apogee passages, within ~6 degrees of the mean. "natural perigee" (or
  "Priapus") likewise, within ~27 degrees. They are not opposite each other.
- A zodiac defined at the instant (true-citra, the galactic ones) has no
  anchor plane; capabilities lists each zodiac's planes.
- Times before 1657 or in the future carry delta T from a model (convert_time).
)";
}

} // namespace prometheia::server::jsontools

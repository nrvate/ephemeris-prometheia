// SPDX-License-Identifier: GPL-2.0-or-later
//
// The object registry (prometheia/registry.hpp, docs/REGISTRY.md).
#include "prometheia/registry.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <ios>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

#include "prometheia/catalog.hpp"
#include "prometheia/elements.hpp"
#include "prometheia/hypotheticals.hpp"
#include "prometheia/stars.hpp"

namespace prometheia::registry {

namespace {

constexpr double kRad2Deg = 180.0 / 3.14159265358979323846;
// IAU 2006 general precession in longitude at J2000 (Capitaine et al. 2003),
// 5028.796195"/century: what turns a sidereal rate into one in the ecliptic
// of date.
constexpr double kPrecessionDegPerDay = 5028.796195 / 3600.0 / 36525.0;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// The ephemeris bodies, in listing order. Mars to Pluto are their system
// barycentres, as everywhere in the engine (docs/ENGINE.md).
struct EphemerisBody {
    long naif;
    const char* name;
    Kind kind;
    bool iau; // the name is the IAU's
};
constexpr EphemerisBody kBodies[] = {
    {10, "Sun", Kind::Planet, true},      {301, "Moon", Kind::Moon, true},
    {199, "Mercury", Kind::Planet, true}, {299, "Venus", Kind::Planet, true},
    {399, "Earth", Kind::Planet, true},   {3, "Earth-Moon barycentre", Kind::Planet, false},
    {4, "Mars", Kind::Planet, true},      {5, "Jupiter", Kind::Planet, true},
    {6, "Saturn", Kind::Planet, true},    {7, "Uranus", Kind::Planet, true},
    {8, "Neptune", Kind::Planet, true},   {9, "Pluto", Kind::Planet, true},
};

const EphemerisBody* ephemeris_body(long naif) {
    for (const EphemerisBody& b : kBodies)
        if (b.naif == naif)
            return &b;
    return nullptr;
}

// The Moon's orbit points by the names astrologers give them: the first
// name of each point is the one info() reports, the rest are aliases.
struct LunarPoint {
    OrbitPoint point;
    OrbitElements method;
    const char* names[5];
};
constexpr LunarPoint kLunarPoints[] = {
    {OrbitPoint::AscendingNode,
     OrbitElements::Mean,
     {"mean node", "north node", "mean north node", nullptr}},
    {OrbitPoint::AscendingNode,
     OrbitElements::Osculating,
     {"true node", "true north node", nullptr}},
    {OrbitPoint::DescendingNode, OrbitElements::Mean, {"mean south node", "south node", nullptr}},
    {OrbitPoint::DescendingNode, OrbitElements::Osculating, {"true south node", nullptr}},
    {OrbitPoint::Aphelion,
     OrbitElements::Mean,
     {"mean apogee", "lilith", "black moon lilith", "mean lilith", nullptr}},
    {OrbitPoint::Aphelion,
     OrbitElements::Osculating,
     {"osculating apogee", "true lilith", "osculating lilith", nullptr}},
    {OrbitPoint::Aphelion,
     OrbitElements::Interpolated,
     {"natural apogee", "natural lilith", "interpolated apogee", nullptr}},
    {OrbitPoint::Perihelion, OrbitElements::Mean, {"mean perigee", nullptr}},
    {OrbitPoint::Perihelion, OrbitElements::Osculating, {"osculating perigee", nullptr}},
    {OrbitPoint::Perihelion,
     OrbitElements::Interpolated,
     {"natural perigee", "priapus", "interpolated perigee", nullptr}},
};

const LunarPoint* lunar_point(OrbitPoint p, OrbitElements m) {
    for (const LunarPoint& lp : kLunarPoints)
        if (lp.point == p && lp.method == m)
            return &lp;
    return nullptr;
}

constexpr const char* kPointTokens[] = {"ascending-node", "descending-node", "perihelion",
                                        "aphelion"};
constexpr const char* kMethodTokens[] = {"mean", "osculating", "interpolated"};

std::optional<OrbitPoint> point_from(std::string_view s) {
    for (int i = 0; i < 4; ++i)
        if (s == kPointTokens[i])
            return OrbitPoint(i);
    if (s == "perigee")
        return OrbitPoint::Perihelion;
    if (s == "apogee")
        return OrbitPoint::Aphelion;
    return std::nullopt;
}

std::optional<OrbitElements> method_from(std::string_view s) {
    for (int i = 0; i < 3; ++i)
        if (s == kMethodTokens[i])
            return OrbitElements(i);
    return std::nullopt;
}

// Latin-1 Supplement and Latin Extended-A letters without their marks,
// from U+00C0; "" drops the character (x, the division sign).
constexpr const char* kLatin[] = {
    // U+00C0 .. U+00FF
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i", //
    "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "ss", //
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i", //
    "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "y",  //
    // U+0100 .. U+017F
    "a", "a", "a", "a", "a", "a", "c", "c", "c", "c", "c", "c", "c", "c", "d", "d",   //
    "d", "d", "e", "e", "e", "e", "e", "e", "e", "e", "e", "e", "g", "g", "g", "g",   //
    "g", "g", "g", "g", "h", "h", "h", "h", "i", "i", "i", "i", "i", "i", "i", "i",   //
    "i", "i", "ij", "ij", "j", "j", "k", "k", "k", "l", "l", "l", "l", "l", "l", "l", //
    "l", "l", "l", "n", "n", "n", "n", "n", "n", "n", "n", "n", "o", "o", "o", "o",   //
    "o", "o", "oe", "oe", "r", "r", "r", "r", "r", "r", "s", "s", "s", "s", "s", "s", //
    "s", "s", "t", "t", "t", "t", "t", "t", "u", "u", "u", "u", "u", "u", "u", "u",   //
    "u", "u", "u", "u", "w", "w", "y", "y", "y", "z", "z", "z", "z", "z", "z", "s",   //
};
static_assert(sizeof(kLatin) / sizeof(kLatin[0]) == 0x180 - 0xC0);

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return out;
}

std::string capitalised(std::string_view s) {
    std::string out(s);
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z')
        out[0] = char(out[0] - 'a' + 'A');
    return out;
}

// A query matches a name by prefix when the folded query starts the folded
// name or any word of it (the JSON lookup's rule: "node" finds "true node").
bool prefix_hit(std::string_view name, const std::string& fq) {
    size_t at = 0;
    while (true) {
        const size_t end = name.find(' ', at);
        if (fold(name.substr(at)).compare(0, fq.size(), fq) == 0)
            return true;
        if (end == std::string_view::npos)
            return false;
        at = end + 1;
    }
}

// The number SBDB gives a body: an asteroid's designation that is all
// digits, or a periodic comet's ("1P", "73P-B", "29P/Schwassmann").
long long number_of(std::string_view pdes, bool comet) {
    size_t i = 0;
    while (i < pdes.size() && pdes[i] >= '0' && pdes[i] <= '9')
        ++i;
    if (i == 0 || i > 15)
        return 0;
    const bool whole = i == pdes.size();
    const bool periodic =
        comet && i < pdes.size() && (pdes[i] == 'P' || pdes[i] == 'D' || pdes[i] == 'I');
    if (!(whole && !comet) && !periodic)
        return 0;
    return std::stoll(std::string(pdes.substr(0, i)));
}

// A provisional designation typed without its space ("2003VB12") gets it.
std::string spaced_designation(std::string_view q) {
    if (q.size() >= 6 &&
        std::all_of(q.begin(), q.begin() + 4, [](char c) { return c >= '0' && c <= '9'; }) &&
        std::isalpha(static_cast<unsigned char>(q[4])))
        return std::string(q.substr(0, 4)) + " " + std::string(q.substr(4));
    return std::string(q);
}

std::string star_designation(const stars::Object& s) {
    if (s.hr)
        return "HR " + std::to_string(s.hr);
    if (s.hip)
        return "HIP " + std::to_string(s.hip);
    if (s.messier)
        return "M " + std::to_string(s.messier);
    if (s.ngc)
        return "NGC " + std::to_string(s.ngc);
    const std::vector<std::string> d = s.designations();
    return d.empty() ? s.name() : d.front();
}

Kind star_kind(const stars::Object& s) {
    return s.kind == stars::Kind::Star || s.kind == stars::Kind::DoubleStar ? Kind::Star
                                                                            : Kind::DeepSky;
}

int quality_rank(MatchQuality q) {
    switch (q) {
    case MatchQuality::Exact:
    case MatchQuality::OldDesignation:
        return 0;
    case MatchQuality::Alias:
        return 1;
    case MatchQuality::Prefix:
        return 2;
    }
    return 3;
}

// The cursor of list(): the section in the top byte, the index below.
enum Section : uint64_t {
    kSecBodies,
    kSecLunar,
    kSecHypotheticals,
    kSecStars,
    kSecCatalog,
    kSecEnd
};
uint64_t cursor_of(uint64_t section, uint64_t index) {
    return (section << 56) | index;
}

} // namespace

std::string fold(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (c >= 'A' && c <= 'Z')
                out += char(c - 'A' + 'a');
            else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                out += char(c);
            ++i;
            continue;
        }
        // UTF-8: decode one code point; a malformed byte is dropped alone.
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (len == 1 || i + size_t(len) > s.size()) {
            ++i;
            continue;
        }
        uint32_t cp = c & (0x7F >> len);
        for (int k = 1; k < len; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3F);
        if (cp >= 0xC0 && cp < 0x180)
            out += kLatin[cp - 0xC0];
        i += size_t(len);
    }
    return out;
}

Result<Id> parse_id(std::string_view id) {
    const size_t colon = id.find(':');
    if (colon == std::string_view::npos)
        return make_error(ErrorCode::ArgumentError,
                          "an object id is naif:, sb:, orbit:, hyp: or star: and its key");
    const std::string_view space = id.substr(0, colon), key = id.substr(colon + 1);
    if (key.empty())
        return make_error(ErrorCode::ArgumentError, "an object id needs a key after its colon");
    Id out;
    if (space == "naif") {
        long n = 0;
        size_t used = 0;
        try {
            n = std::stol(std::string(key), &used);
        } catch (...) {
            used = 0;
        }
        if (used != key.size())
            return make_error(ErrorCode::ArgumentError, "a naif: id is an integer");
        out.space = Id::Space::Naif;
        out.naif = n;
        return out;
    }
    if (space == "sb") {
        out.space = Id::Space::SmallBody;
        out.designation = std::string(key);
        return out;
    }
    if (space == "hyp") {
        out.space = Id::Space::Hypothetical;
        out.token = lower_ascii(key);
        return out;
    }
    if (space == "star") {
        auto found = stars::find(key);
        if (!found)
            return found.error();
        out.space = Id::Space::Star;
        out.star = found.value();
        return out;
    }
    if (space == "orbit") {
        // orbit:<body id>:<point>:<method>, the body id itself holding a colon.
        const size_t m = key.rfind(':');
        const size_t p =
            m == std::string_view::npos || m == 0 ? std::string_view::npos : key.rfind(':', m - 1);
        if (p == std::string_view::npos)
            return make_error(ErrorCode::ArgumentError,
                              "an orbit: id is orbit:<body id>:<point>:<method>");
        auto body = parse_id(key.substr(0, p));
        if (!body)
            return body.error();
        const auto point = point_from(key.substr(p + 1, m - p - 1));
        const auto method = method_from(key.substr(m + 1));
        if (!point || !method)
            return make_error(ErrorCode::ArgumentError,
                              "an orbit point is ascending-node, descending-node, perihelion or "
                              "aphelion, by mean, osculating or interpolated");
        if (body.value().space != Id::Space::Naif && body.value().space != Id::Space::SmallBody)
            return make_error(ErrorCode::ArgumentError,
                              "an orbit point belongs to an ephemeris or catalog body");
        out = body.value();
        out.space = Id::Space::OrbitPoint;
        out.point = *point;
        out.method = *method;
        return out;
    }
    return make_error(ErrorCode::ArgumentError,
                      "an object id is naif:, sb:, orbit:, hyp: or star: and its key");
}

std::string to_string(const Id& id) {
    switch (id.space) {
    case Id::Space::Naif:
        return "naif:" + std::to_string(id.naif);
    case Id::Space::SmallBody:
        return "sb:" + id.designation;
    case Id::Space::Hypothetical:
        return "hyp:" + id.token;
    case Id::Space::Star:
        return "star:" + star_designation(stars::at(id.star));
    case Id::Space::OrbitPoint: {
        const std::string body =
            id.designation.empty() ? "naif:" + std::to_string(id.naif) : "sb:" + id.designation;
        return "orbit:" + body + ":" + kPointTokens[int(id.point)] + ":" +
               kMethodTokens[int(id.method)];
    }
    }
    return {};
}

std::string_view kind_name(Kind k) {
    static constexpr const char* names[] = {"planet",      "moon",         "lunar-point",
                                            "orbit-point", "hypothetical", "star",
                                            "deep-sky",    "asteroid",     "comet"};
    return names[int(k)];
}

std::optional<Kind> kind_from_name(std::string_view name) {
    for (int k = 0; k < kKindCount; ++k)
        if (kind_name(Kind(k)) == name)
            return Kind(k);
    return std::nullopt;
}

std::string_view standing_name(Standing s) {
    static constexpr const char* names[] = {"none", "iau", "traditional", "defined"};
    return names[int(s)];
}

std::string_view quality_name(MatchQuality q) {
    static constexpr const char* names[] = {"exact", "alias", "old-designation", "prefix"};
    return names[int(q)];
}

// ---------------------------------------------------------------------------

struct Registry::Impl {
    Engine& engine;
    explicit Impl(Engine& e) : engine(e) {}

    // What is built from the catalogs, and how many catalogs it was built
    // from: add_catalog() makes it stale.
    size_t built_for = SIZE_MAX;
    // Every body's newest record, by SPK-ID, as list() filters it.
    struct Summary {
        uint64_t spkid;
        float h;             // NaN when the record carries none
        uint8_t comet;       // BodyClass::Comet
        uint8_t orbit_class; // catalog::OrbitClass
        uint8_t named;
        uint8_t numbered;
    };
    bool summary_built = false;
    std::vector<Summary> summary;
    // The folded proper names of the named bodies, sorted: lookup's index
    // for names typed with accents or punctuation, and for prefixes.
    bool names_built = false;
    std::vector<std::pair<std::string, uint64_t>> folded_names;

    // resolve()'s answers: a scan resolves the same few ids every step,
    // and a catalog id costs a record decode (~0.2 ms) to resolve afresh.
    std::unordered_map<std::string, Engine::ObjectRef> resolved;
    static constexpr size_t kResolvedMax = 4096;

    void invalidate_if_stale() {
        const size_t n = engine.catalogs().size();
        if (n != built_for) {
            summary_built = names_built = false;
            summary.clear();
            folded_names.clear();
            resolved.clear();
            built_for = n;
        }
    }

    Result<void> ensure_names() {
        invalidate_if_stale();
        if (names_built)
            return {};
        std::vector<std::pair<std::string, uint64_t>> v;
        auto each =
            engine.for_each_catalog_name([&](size_t, uint64_t spkid, const catalog::Names& n) {
                if (!n.name.empty())
                    v.emplace_back(fold(n.name), spkid);
            });
        if (!each)
            return each.error();
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        folded_names = std::move(v);
        names_built = true;
        return {};
    }

    Result<void> ensure_summary() {
        invalidate_if_stale();
        if (summary_built)
            return {};
        std::vector<Summary> v;
        size_t records = 0;
        for (const Engine::CatalogInfo& c : engine.catalogs())
            records += size_t(c.bodies);
        v.reserve(records);
        auto each = engine.for_each_catalog_record(
            [&](size_t, const catalog::Record& r, const catalog::Names& n) {
                const bool comet = r.body_class == catalog::BodyClass::Comet;
                v.push_back(Summary{
                    r.spkid,
                    (r.flags & catalog::RecordFlags::kHg) ? r.h_mag : std::nanf(""),
                    uint8_t(comet),
                    uint8_t(r.orbit_class),
                    uint8_t(!n.name.empty()),
                    uint8_t(number_of(n.pdes, comet) != 0),
                });
            });
        if (!each)
            return each.error();
        // Catalog by catalog in the order added: the last record of a body
        // is its newest catalog's.
        std::stable_sort(v.begin(), v.end(),
                         [](const Summary& a, const Summary& b) { return a.spkid < b.spkid; });
        size_t kept = 0;
        for (size_t i = 0; i < v.size(); ++i)
            if (i + 1 == v.size() || v[i + 1].spkid != v[i].spkid)
                v[kept++] = v[i];
        v.resize(kept);
        v.shrink_to_fit();
        summary = std::move(v);
        summary_built = true;
        return {};
    }

    std::string ephemeris_source() const {
        std::string out;
        for (const Engine::EphemerisSpan& s : engine.ephemeris_spans())
            out += (out.empty() ? "" : " + ") + s.source;
        return out;
    }

    // A body's display name in an orbit point's: the catalog body's name or
    // designation, or the ephemeris body's.
    Result<std::string> body_name(const Id& id) {
        if (id.designation.empty()) {
            const EphemerisBody* b = ephemeris_body(id.naif);
            if (!b)
                return make_error(ErrorCode::NotFound,
                                  "naif:" + std::to_string(id.naif) + " is not an object here");
            return std::string(b->name);
        }
        auto spk = engine.lookup(id.designation);
        if (!spk)
            return spk.error();
        auto cb = engine.catalog_body(spk.value());
        if (!cb)
            return cb.error();
        return cb.value().names.name.empty() ? cb.value().names.designation : cb.value().names.name;
    }

    void set_rates(Info& out, double sidereal_deg_day) {
        out.sidereal_mean_motion_deg_day = sidereal_deg_day;
        out.mean_motion_deg_day = sidereal_deg_day + kPrecessionDegPerDay;
        out.period_days = 360.0 / std::fabs(sidereal_deg_day);
    }

    Result<Info> body_info(const Id& id) {
        const EphemerisBody* b = ephemeris_body(id.naif);
        if (!b)
            return make_error(ErrorCode::NotFound,
                              "naif:" + std::to_string(id.naif) + " is not an object here");
        Info out;
        out.id = to_string(id);
        out.kind = b->kind;
        out.name = b->name;
        out.standing = b->iau ? Standing::Iau : Standing::None;
        out.standing_source = b->iau ? "IAU" : "";
        out.source = ephemeris_source();
        out.ref.kind = Engine::ObjectRef::Kind::Body;
        out.ref.id = id.naif;
        if (auto n = engine.mean_motion_deg_day(int(id.naif)))
            set_rates(out, n.value());
        return out;
    }

    Result<Info> catalog_info(const Id& id) {
        auto spk = engine.lookup(id.designation);
        if (!spk)
            return spk.error();
        auto got = engine.catalog_body(spk.value());
        if (!got)
            return got.error();
        const Engine::CatalogBody& cb = got.value();
        const catalog::Record& r = cb.record;
        const bool comet = r.body_class == catalog::BodyClass::Comet;
        Info out;
        Id canonical;
        canonical.space = Id::Space::SmallBody;
        canonical.designation = cb.names.designation;
        out.id = to_string(canonical);
        out.kind = comet ? Kind::Comet : Kind::Asteroid;
        if (r.flags & catalog::RecordFlags::kOrbitClass)
            out.orbit_class = std::string(catalog::orbit_class_code(r.orbit_class));
        out.name = cb.names.name;
        out.standing = out.name.empty() ? Standing::None : Standing::Iau;
        out.standing_source = out.name.empty() ? "" : "IAU WGSBN";
        out.number = number_of(cb.names.designation, comet);
        out.designation = cb.names.designation;
        out.provisional_designation = cb.names.provisional_designation;
        if (r.flags & catalog::RecordFlags::kHg) {
            out.h_mag = detail::widened(r.h_mag);
            out.g_slope = detail::widened(r.g_slope);
        }
        if (r.flags & catalog::RecordFlags::kDiameter)
            out.diameter_km = detail::widened(r.diameter_km);
        out.elements_epoch_jd = r.epoch_jtdb;
        // Two-body mean motion about the Sun, the body massless (the
        // Gaussian constant), and retrograde negative.
        if (r.a_au > 0.0 && r.e < 1.0) {
            const double n = elements::kGaussK / (r.a_au * std::sqrt(r.a_au)) * kRad2Deg;
            set_rates(out, r.inc_rad > 1.5707963267948966 ? -n : n);
        }
        const std::vector<Engine::CatalogInfo> cats = engine.catalogs();
        if (cb.catalog < cats.size())
            out.source = std::filesystem::path(cats[cb.catalog].path).filename().string();
        out.ref.kind = Engine::ObjectRef::Kind::Body;
        out.ref.id = spk.value();
        return out;
    }

    Result<Info> orbit_point_info(const Id& id) {
        Info out;
        long body = id.naif;
        if (!id.designation.empty()) {
            auto spk = engine.lookup(id.designation);
            if (!spk)
                return spk.error();
            body = spk.value();
        } else if (!ephemeris_body(id.naif)) {
            return make_error(ErrorCode::NotFound,
                              "naif:" + std::to_string(id.naif) + " is not an object here");
        }
        out.id = to_string(id);
        const bool moon = id.designation.empty() && id.naif == body::kMoon;
        out.kind = moon ? Kind::LunarPoint : Kind::OrbitPoint;
        if (const LunarPoint* lp = moon ? lunar_point(id.point, id.method) : nullptr) {
            out.name = lp->names[0];
        } else {
            auto bn = body_name(id);
            if (!bn)
                return bn.error();
            static constexpr const char* point_words[] = {"ascending node", "descending node",
                                                          "perihelion", "aphelion"};
            const bool geocentric =
                id.designation.empty() && (id.naif == body::kMoon || id.naif == body::kSun);
            const char* word = point_words[int(id.point)];
            if (geocentric && id.point == OrbitPoint::Perihelion)
                word = "perigee";
            if (geocentric && id.point == OrbitPoint::Aphelion)
                word = "apogee";
            out.name = bn.value() + " " + kMethodTokens[int(id.method)] + " " + word;
        }
        out.standing = Standing::Defined;
        out.source = id.designation.empty() ? ephemeris_source() : std::string();
        // The mean rate of a mean point; a natural apsis follows the mean
        // one on average. An osculating point has none.
        if (id.method != OrbitElements::Osculating && id.designation.empty())
            if (auto n = engine.orbit_point_mean_motion_deg_day(int(body), id.point))
                set_rates(out, n.value());
        out.ref.kind = Engine::ObjectRef::Kind::OrbitPoint;
        out.ref.id = body;
        out.ref.point = id.point;
        out.ref.method = id.method;
        return out;
    }

    Result<Info> hypothetical_info(const Id& id) {
        const hypotheticals::Body* h = engine.hypothetical(id.token);
        if (!h)
            return make_error(ErrorCode::NotFound, "no hypothetical body '" + id.token + "'");
        Info out;
        out.id = to_string(id);
        out.kind = Kind::Hypothetical;
        out.name = h->name.empty() ? capitalised(h->token) : h->name;
        out.standing = Standing::Defined;
        out.standing_source = h->citation;
        out.designation = h->token;
        out.elements_epoch_jd = h->elements.epoch_jd_tt;
        out.source = h->set;
        // The mean longitude M + argp + node advances at the mean anomaly's
        // rate (its polynomial's, or the Kepler motion when M is quoted at
        // the epoch: docs/HYPOTHETICALS.md) plus the angles' secular rates.
        const PolynomialElements& el = h->elements;
        const auto rate = [&](const double c[5]) { return el.n_terms > 1 ? c[1] : 0.0; };
        bool m_moves = false;
        for (int k = 1; k < el.n_terms; ++k)
            m_moves = m_moves || el.mean_anomaly[k] != 0.0;
        double n = 0.0; // deg/day
        if (m_moves) {
            n = rate(el.mean_anomaly) / 36525.0;
        } else if (el.semi_major_axis[0] > 0.0 && el.origin == ElementOrigin::Sun) {
            const double a = el.semi_major_axis[0];
            n = elements::kGaussK / (a * std::sqrt(a)) * kRad2Deg;
        }
        n += (rate(el.arg_perihelion) + rate(el.ascending_node)) / 36525.0;
        if (n != 0.0) {
            // Elements of date already count from the moving equinox.
            const bool of_date = el.equinox == ElementEquinox::OfDate;
            set_rates(out, of_date ? n - kPrecessionDegPerDay : n);
        }
        out.ref.kind = Engine::ObjectRef::Kind::Hypothetical;
        out.ref.token = h->token;
        return out;
    }

    Info star_info(size_t index) {
        const stars::Object& s = stars::at(index);
        Info out;
        Id id;
        id.space = Id::Space::Star;
        id.star = index;
        out.id = to_string(id);
        out.kind = star_kind(s);
        if (!s.names.empty()) {
            out.name = std::string(s.names.front());
            const bool iau = s.iau_name;
            out.standing = iau ? Standing::Iau : Standing::Traditional;
            out.standing_source = iau ? "IAU WGSN" : "Allen (1899)";
        }
        out.designation = star_designation(s);
        out.vmag = s.vmag;
        out.source = "the compiled-in star catalog (docs/STARS.md)";
        out.ref.kind = Engine::ObjectRef::Kind::Star;
        out.ref.id = long(index);
        return out;
    }

    Result<Info> info(const Id& id) {
        switch (id.space) {
        case Id::Space::Naif:
            return body_info(id);
        case Id::Space::SmallBody:
            return catalog_info(id);
        case Id::Space::OrbitPoint:
            return orbit_point_info(id);
        case Id::Space::Hypothetical:
            return hypothetical_info(id);
        case Id::Space::Star:
            return star_info(id.star);
        }
        return make_error(ErrorCode::ArgumentError, "unknown object id");
    }
};

Registry::Registry(Engine& engine) : impl_(std::make_unique<Impl>(engine)) {}
Registry::~Registry() = default;

Result<Info> Registry::info(std::string_view id) {
    auto parsed = parse_id(id);
    if (!parsed)
        return parsed.error();
    return impl_->info(parsed.value());
}

Result<Engine::ObjectRef> Registry::resolve(std::string_view id) {
    impl_->invalidate_if_stale();
    if (auto hit = impl_->resolved.find(std::string(id)); hit != impl_->resolved.end())
        return hit->second;
    auto r = resolve_uncached(id);
    if (r) {
        if (impl_->resolved.size() >= Impl::kResolvedMax)
            impl_->resolved.clear();
        impl_->resolved.emplace(std::string(id), r.value());
    }
    return r;
}

Result<Engine::ObjectRef> Registry::resolve_uncached(std::string_view id) {
    auto parsed = parse_id(id);
    if (!parsed)
        return parsed.error();
    const Id& p = parsed.value();
    Engine::ObjectRef ref;
    switch (p.space) {
    case Id::Space::Naif:
    case Id::Space::OrbitPoint:
        if (p.designation.empty()) {
            if (!ephemeris_body(p.naif))
                return make_error(ErrorCode::NotFound,
                                  "naif:" + std::to_string(p.naif) + " is not an object here");
            ref.id = p.naif;
        } else {
            auto spk = impl_->engine.lookup(p.designation);
            if (!spk)
                return spk.error();
            ref.id = spk.value();
        }
        if (p.space == Id::Space::OrbitPoint) {
            ref.kind = Engine::ObjectRef::Kind::OrbitPoint;
            ref.point = p.point;
            ref.method = p.method;
        }
        return ref;
    case Id::Space::SmallBody: {
        auto spk = impl_->engine.lookup(p.designation);
        if (!spk)
            return spk.error();
        ref.id = spk.value();
        return ref;
    }
    case Id::Space::Hypothetical:
        if (!impl_->engine.hypothetical(p.token))
            return make_error(ErrorCode::NotFound, "no hypothetical body '" + p.token + "'");
        ref.kind = Engine::ObjectRef::Kind::Hypothetical;
        ref.token = p.token;
        return ref;
    case Id::Space::Star:
        ref.kind = Engine::ObjectRef::Kind::Star;
        ref.id = long(p.star);
        return ref;
    }
    return make_error(ErrorCode::ArgumentError, "unknown object id");
}

Result<std::vector<Match>> Registry::lookup(std::string_view query, bool prefix,
                                            size_t max_matches) {
    const std::string fq = fold(query);
    std::vector<Match> out;
    if (fq.empty() || max_matches == 0)
        return out;
    const bool want_prefix = prefix && fq.size() >= 2;
    const auto add = [&](std::string id, Kind kind, MatchQuality q, std::string name,
                         std::string matched) {
        for (const Match& m : out)
            if (m.id == id)
                return;
        out.push_back(Match{std::move(id), kind, q, std::move(name), std::move(matched)});
    };
    const auto quality = [&](std::string_view name) -> std::optional<MatchQuality> {
        if (fold(name) == fq)
            return MatchQuality::Exact;
        if (want_prefix && prefix_hit(name, fq))
            return MatchQuality::Prefix;
        return std::nullopt;
    };

    for (const EphemerisBody& b : kBodies)
        if (auto q = quality(b.name)) {
            Id id;
            id.naif = b.naif;
            add(to_string(id), b.kind, *q, b.name, b.name);
        }
    for (const LunarPoint& lp : kLunarPoints)
        for (const char* const* n = lp.names; *n; ++n)
            if (auto q = quality(*n)) {
                Id id;
                id.space = Id::Space::OrbitPoint;
                id.naif = body::kMoon;
                id.point = lp.point;
                id.method = lp.method;
                add(to_string(id), Kind::LunarPoint, *q, lp.names[0], *n);
            }
    for (const std::string& token : impl_->engine.hypothetical_tokens()) {
        const hypotheticals::Body* h = impl_->engine.hypothetical(token);
        const std::string name = h && !h->name.empty() ? h->name : capitalised(token);
        auto q = quality(token);
        if (!q)
            q = quality(name);
        if (q) {
            Id id;
            id.space = Id::Space::Hypothetical;
            id.token = token;
            add(to_string(id), Kind::Hypothetical, *q, name, token);
        }
    }
    for (const stars::Match& m : stars::lookup(query, max_matches, prefix)) {
        const stars::Object& s = stars::at(m.index);
        Id id;
        id.space = Id::Space::Star;
        id.star = m.index;
        const MatchQuality q = m.quality == stars::MatchQuality::Exact   ? MatchQuality::Exact
                               : m.quality == stars::MatchQuality::Alias ? MatchQuality::Alias
                                                                         : MatchQuality::Prefix;
        add(to_string(id), star_kind(s), q, s.name(), m.matched);
    }

    // Catalog bodies: the engine's index answers a designation, a name or
    // an old designation exactly (case aside); the folded name index
    // answers a name typed with accents or punctuation, and prefixes.
    if (!impl_->engine.catalogs().empty()) {
        const auto add_body = [&](int spkid, MatchQuality q,
                                  const std::string& matched) -> Result<void> {
            auto cb = impl_->engine.catalog_body(spkid);
            if (!cb)
                return cb.error();
            const Engine::BodyNames& n = cb.value().names;
            Id id;
            id.space = Id::Space::SmallBody;
            id.designation = n.designation;
            const bool comet = cb.value().record.body_class == catalog::BodyClass::Comet;
            add(to_string(id), comet ? Kind::Comet : Kind::Asteroid, q,
                n.name.empty() ? n.designation : n.name, matched);
            return {};
        };
        for (const std::string& key : {std::string(query), spaced_designation(query)}) {
            auto found = impl_->engine.lookup(key);
            if (!found) {
                if (found.error().code != ErrorCode::NotFound)
                    return found.error();
                continue;
            }
            auto cb = impl_->engine.catalog_body(found.value());
            if (!cb)
                return cb.error();
            const Engine::BodyNames& n = cb.value().names;
            const std::string lk = lower_ascii(key);
            const bool old = lower_ascii(n.designation) != lk && lower_ascii(n.name) != lk &&
                             lower_ascii(n.provisional_designation) == lk;
            if (auto r = add_body(found.value(),
                                  old ? MatchQuality::OldDesignation : MatchQuality::Exact, key);
                !r)
                return r.error();
            break;
        }
        if (auto r = impl_->ensure_names(); !r)
            return r.error();
        const auto& names = impl_->folded_names;
        auto it = std::lower_bound(names.begin(), names.end(), std::make_pair(fq, uint64_t(0)));
        for (; it != names.end() && out.size() < 4 * max_matches; ++it) {
            const bool exact = it->first == fq;
            if (!exact && !(want_prefix && it->first.compare(0, fq.size(), fq) == 0))
                break;
            // Confirmed against the body's newest record: an overlay may
            // have renamed it.
            auto cb = impl_->engine.catalog_body(int(it->second));
            if (!cb)
                return cb.error();
            if (fold(cb.value().names.name) != it->first)
                continue;
            if (auto r =
                    add_body(int(it->second), exact ? MatchQuality::Exact : MatchQuality::Prefix,
                             cb.value().names.name);
                !r)
                return r.error();
        }
    }

    std::stable_sort(out.begin(), out.end(), [](const Match& a, const Match& b) {
        return quality_rank(a.quality) < quality_rank(b.quality);
    });
    if (out.size() > max_matches)
        out.resize(max_matches);
    return out;
}

Result<Page> Registry::list(const Filter& f, uint64_t cursor, size_t max) {
    Page page;
    uint64_t section = cursor >> 56, index = cursor & ((uint64_t(1) << 56) - 1);
    const auto kind_ok = [&](Kind k) { return f.kinds == 0 || (f.kinds >> int(k)) & 1u; };
    // A condition on a property passes only objects that have it.
    const bool any_small_body_only =
        !f.orbit_class.empty() || f.numbered_only || !std::isnan(f.h_max);
    const auto push = [&](std::string id, Kind kind, std::string name) {
        page.objects.push_back(
            Match{std::move(id), kind, MatchQuality::Exact, std::move(name), {}});
    };
    std::optional<catalog::OrbitClass> want_class;
    if (!f.orbit_class.empty()) {
        want_class = catalog::orbit_class_from_code(f.orbit_class);
        if (!want_class)
            return make_error(ErrorCode::ArgumentError,
                              "unknown orbit class '" + f.orbit_class + "'");
    }
    while (section < kSecEnd && page.objects.size() < max) {
        switch (section) {
        case kSecBodies: {
            const size_t n = std::size(kBodies);
            for (; index < n && page.objects.size() < max; ++index) {
                const EphemerisBody& b = kBodies[index];
                if (!kind_ok(b.kind) || any_small_body_only || !std::isnan(f.vmag_max) ||
                    (f.named_only && !b.iau))
                    continue;
                Id id;
                id.naif = b.naif;
                push(to_string(id), b.kind, b.name);
            }
            if (index < n)
                break;
            ++section;
            index = 0;
            break;
        }
        case kSecLunar: {
            const size_t n = std::size(kLunarPoints);
            for (; index < n && page.objects.size() < max; ++index) {
                const LunarPoint& lp = kLunarPoints[index];
                if (!kind_ok(Kind::LunarPoint) || any_small_body_only || !std::isnan(f.vmag_max))
                    continue;
                Id id;
                id.space = Id::Space::OrbitPoint;
                id.naif = body::kMoon;
                id.point = lp.point;
                id.method = lp.method;
                push(to_string(id), Kind::LunarPoint, lp.names[0]);
            }
            if (index < n)
                break;
            ++section;
            index = 0;
            break;
        }
        case kSecHypotheticals: {
            const std::vector<std::string> tokens = impl_->engine.hypothetical_tokens();
            for (; index < tokens.size() && page.objects.size() < max; ++index) {
                if (!kind_ok(Kind::Hypothetical) || any_small_body_only || !std::isnan(f.vmag_max))
                    continue;
                const hypotheticals::Body* h = impl_->engine.hypothetical(tokens[index]);
                Id id;
                id.space = Id::Space::Hypothetical;
                id.token = tokens[index];
                push(to_string(id), Kind::Hypothetical,
                     h && !h->name.empty() ? h->name : capitalised(tokens[index]));
            }
            if (index < tokens.size())
                break;
            ++section;
            index = 0;
            break;
        }
        case kSecStars: {
            const size_t n = stars::count();
            for (; index < n && page.objects.size() < max; ++index) {
                const stars::Object& s = stars::at(index);
                const Kind k = star_kind(s);
                if (!kind_ok(k) || any_small_body_only || (f.named_only && s.names.empty()))
                    continue;
                if (!std::isnan(f.vmag_max) && !(s.vmag <= f.vmag_max))
                    continue;
                Id id;
                id.space = Id::Space::Star;
                id.star = index;
                push(to_string(id), k, s.name());
            }
            if (index < n)
                break;
            ++section;
            index = 0;
            break;
        }
        case kSecCatalog: {
            if ((!kind_ok(Kind::Asteroid) && !kind_ok(Kind::Comet)) || !std::isnan(f.vmag_max) ||
                impl_->engine.catalogs().empty()) {
                section = kSecEnd;
                break;
            }
            if (auto r = impl_->ensure_summary(); !r)
                return r.error();
            const auto& v = impl_->summary;
            for (; index < v.size() && page.objects.size() < max; ++index) {
                const Impl::Summary& s = v[index];
                if (!kind_ok(s.comet ? Kind::Comet : Kind::Asteroid) ||
                    (want_class && s.orbit_class != uint8_t(*want_class)) ||
                    (f.named_only && !s.named) || (f.numbered_only && !s.numbered) ||
                    (!std::isnan(f.h_max) && !(double(s.h) <= f.h_max)))
                    continue;
                auto cb = impl_->engine.catalog_body(int(s.spkid));
                if (!cb)
                    return cb.error();
                const Engine::BodyNames& n = cb.value().names;
                Id id;
                id.space = Id::Space::SmallBody;
                id.designation = n.designation;
                push(to_string(id), s.comet ? Kind::Comet : Kind::Asteroid,
                     n.name.empty() ? n.designation : n.name);
            }
            if (index < v.size())
                break;
            section = kSecEnd;
            break;
        }
        default:
            section = kSecEnd;
        }
    }
    page.next = section < kSecEnd ? cursor_of(section, index) : 0;
    return page;
}

namespace detail {

// A catalog float as the decimal it was written from: H -1.26 reads back
// as -1.2599999904632568 when widened bit for bit. The decimal is the
// shortest that reads back as the float. With floating-point <charconv>
// (__cpp_lib_to_chars), to_chars finds it. Apple's libc++ marks the
// floating-point to_chars and from_chars unavailable below macOS 13.3 and
// 26.0, and advertises no __cpp_lib_to_chars; there, and under
// PROMETHEIA_PORTABLE_FALLBACKS, which tests this path on Linux, it is the
// first of 1 to 9 significant digits whose correctly rounded decimal reads
// back as the float. Streams in the classic locale write and read it, so a
// process locale's decimal comma cannot enter.
double widened(float f) {
    if (!std::isfinite(f))
        return double(f);
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L &&                                \
    !defined(PROMETHEIA_PORTABLE_FALLBACKS)
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, f);
    double d = double(f);
    std::from_chars(buf, r.ptr, d);
    return d;
#else
    for (int digits = 1; digits <= std::numeric_limits<float>::max_digits10; ++digits) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(digits) << f;
        const std::string text = out.str();
        std::istringstream back(text);
        back.imbue(std::locale::classic());
        float g = 0.0f;
        if (!(back >> g) || g != f)
            continue;
        // From 2^24 up every float is a whole number, and to_chars writes it
        // in full when that is no longer than the shortest scientific form
        // (the exact value wins the tie between fixed forms of one length).
        if (std::fabs(f) >= 16777216.0f) {
            std::ostringstream fixed, sci;
            fixed.imbue(std::locale::classic());
            sci.imbue(std::locale::classic());
            fixed << std::fixed << std::setprecision(0) << double(f);
            sci << std::scientific << std::setprecision(digits - 1) << f;
            if (fixed.str().size() <= sci.str().size())
                return double(f);
        }
        std::istringstream wide(text);
        wide.imbue(std::locale::classic());
        double d = 0.0;
        if (wide >> d)
            return d;
    }
    return double(f);
#endif
}

} // namespace detail

} // namespace prometheia::registry

// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia::registry — what an engine can answer, and what each object is
// (docs/REGISTRY.md). One identity scheme covers every namespace: ephemeris
// bodies, catalog bodies, orbit points (the Moon's by the names astrologers
// give them), hypothetical bodies, stars and deep-sky objects. Each object
// has a stable string id that round-trips through parse_id() and info().
//
//   naif:<NAIF id>                        "naif:10", "naif:301", "naif:5"
//   sb:<primary designation>              "sb:136199", "sb:2017 OF201", "sb:1P"
//   orbit:<body id>:<point>:<method>      "orbit:naif:301:ascending-node:mean"
//   hyp:<token>                           "hyp:cupido"
//   star:<designation>                    "star:HR 5984", "star:HIP 71683", "star:M 44"
//
// A Registry belongs to one engine and keeps what it builds lazily (the
// index of catalog names, the listing summary) until a catalog is added.
// Like the engine, it is used by one thread at a time.
#ifndef PROMETHEIA_REGISTRY_HPP
#define PROMETHEIA_REGISTRY_HPP

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "prometheia/engine.hpp"
#include "prometheia/error.hpp"

namespace prometheia::registry {

enum class Kind {
    Planet,     // the Sun, the planets (Mars to Pluto: their barycentres), the Earth
    Moon,       // the Moon
    LunarPoint, // an orbit point of the Moon (the nodes, Lilith, the apsides)
    OrbitPoint, // an orbit point of any other body
    Hypothetical,
    Star,
    DeepSky,
    Asteroid,
    Comet,
};
inline constexpr int kKindCount = 9;

// Where an object's name comes from.
enum class Standing {
    None,        // no name: a designation or a number only
    Iau,         // issued by the IAU: the planets, WGSBN (small bodies), WGSN (stars)
    Traditional, // a star's traditional name, cited from Allen (1899)
    Defined,     // a name the object's own definition gives it: an element set's,
                 // a lunar point's conventional name
};

// The id of an object (see the header comment), parsed. Exactly one of the
// fields that matter for `kind` is set.
struct Id {
    enum class Space { Naif, SmallBody, OrbitPoint, Hypothetical, Star } space = Space::Naif;
    long naif = 0;           // Naif; OrbitPoint whose body is an ephemeris body
    std::string designation; // SmallBody; OrbitPoint whose body is a catalog body
    OrbitPoint point = OrbitPoint::AscendingNode;
    OrbitElements method = OrbitElements::Mean;
    std::string token; // Hypothetical
    size_t star = 0;   // Star: index into the star catalog
};

// Parses an id. ArgumentError for a malformed one; NotFound for a well-formed
// one naming nothing (an unknown token, a star designation that resolves to
// nothing). A catalog body's designation is not checked here: info() does.
Result<Id> parse_id(std::string_view id);

// The canonical id string of an object.
std::string to_string(const Id& id);

struct Info {
    std::string id;
    Kind kind = Kind::Planet;
    std::string orbit_class; // SBDB's code ("MBA", "CEN", "JFc"); empty when none
    std::string name;        // the proper name; empty when the object has none
    Standing standing = Standing::None;
    std::string standing_source;         // "IAU WGSBN", "Allen (1899)", an element set's citation
    long long number = 0;                // MPC number (numbered asteroids, periodic comets); 0 none
    std::string designation;             // primary designation ("136199", "2017 OF201", "HR 5984")
    std::string provisional_designation; // a numbered body's old one ("2003 VB12")
    double h_mag = std::nan(""), g_slope = std::nan(""), diameter_km = std::nan("");
    double vmag = std::nan("");              // stars and deep sky
    double elements_epoch_jd = std::nan(""); // TDB; catalog bodies and hypotheticals
    // The rate of the object's mean longitude, deg/day, signed (retrograde
    // negative): in the ecliptic of date (the tropical rate a longitude scan
    // sees), and against the fixed stars (sidereal). The period is the
    // sidereal one, days. NaN where the object has no mean motion: a star,
    // an osculating orbit point, an open orbit.
    double mean_motion_deg_day = std::nan("");
    double sidereal_mean_motion_deg_day = std::nan("");
    double period_days = std::nan("");
    // What answers for it: the ephemeris, the catalog file, the star
    // catalog or the element set.
    std::string source;
    // How to compute it: the object as rise_set_ut() and calc() take it.
    Engine::ObjectRef ref;
};

enum class MatchQuality {
    Exact,          // a name or designation equal to the query (case and accents aside)
    Alias,          // a star designation's component left out ("Beta Sco")
    OldDesignation, // a numbered body's old provisional designation
    Prefix,         // the start of a name, or of a word in it (prefix lookups only)
};

struct Match {
    std::string id;
    Kind kind = Kind::Planet;
    MatchQuality quality = MatchQuality::Exact;
    std::string name;    // the object's display name
    std::string matched; // the name or designation that matched
};

// Which objects list() returns. Every condition holds at once.
struct Filter {
    unsigned kinds = 0; // bit (1 << int(Kind)); 0 = every kind
    std::string orbit_class;
    bool named_only = false;
    bool numbered_only = false;
    double h_max = std::nan("");    // catalog bodies: H at most this (others pass)
    double vmag_max = std::nan(""); // stars and deep sky: V at most this (others pass)
};

struct Page {
    std::vector<Match> objects; // quality Exact, matched empty
    uint64_t next = 0;          // the cursor of the next page; 0 when there is none
};

// Fold for matching: ASCII lowercase, Latin accents taken off, and every
// character that is not a letter or a digit dropped ("Gǃkúnǁʼhòmdímà" and
// SBDB's "G!kun||'homdima" both fold to "gkunhomdima").
std::string fold(std::string_view s);

class Registry {
public:
    explicit Registry(Engine& engine);
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    Result<Info> info(std::string_view id);
    // Only what computing the object needs: info(id).ref, without the rest.
    Result<Engine::ObjectRef> resolve(std::string_view id);

    // Every object answering to query in every namespace, best first: exact
    // and old-designation matches, then aliases, then (with prefix) prefix
    // matches; within a quality, ephemeris bodies, lunar points,
    // hypotheticals, stars (brighter first), then catalog bodies. Up to
    // max_matches. Several matches of the best quality mean the query is
    // ambiguous, and nothing is chosen.
    Result<std::vector<Match>> lookup(std::string_view query, bool prefix = false,
                                      size_t max_matches = 20);

    // Objects passing filter, in a fixed order (ephemeris bodies, lunar
    // points, hypotheticals, stars and deep sky by catalog index, catalog
    // bodies by SPK-ID), from cursor (0: the start), at most max per page.
    // The first page that reaches the catalog bodies reads every catalog
    // record once (docs/REGISTRY.md has the measured cost).
    Result<Page> list(const Filter& filter, uint64_t cursor, size_t max);

private:
    Result<Engine::ObjectRef> resolve_uncached(std::string_view id);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string_view kind_name(Kind k); // "planet", "lunar-point", "deep-sky", ...
std::optional<Kind> kind_from_name(std::string_view name);
std::string_view standing_name(Standing s);    // "none", "iau", "traditional", "defined"
std::string_view quality_name(MatchQuality q); // "exact", "alias", "old-designation", "prefix"

} // namespace prometheia::registry

#endif // PROMETHEIA_REGISTRY_HPP

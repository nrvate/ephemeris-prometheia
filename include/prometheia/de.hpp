// SPDX-License-Identifier: GPL-2.0-or-later
//
// Reader for JPL planetary ephemeris binaries ("DExxx" files), e.g.
// linux_p1550p2650.440 (DE440) or lnxm1600p2170.200 (DE200).
//
// The binaries are self-describing: header record 1 carries the title,
// constant names, coverage, NCON, AU, EMRAT, DENUM and the Chebyshev
// pointer table (GROUP 1050); header record 2 the constant values. See
// docs/DE.md for the byte map. Either byte order is accepted.
//
// Units: positions in kilometres, velocities in km/day, angles
// (nutations, librations) in radians and rad/day, TT-TDB in seconds;
// rectangular coordinates in the ephemeris' own equatorial frame (ICRF
// for DE405+); the time argument is the TDB-scaled Julian Ephemeris Date.
// The planets and the Sun are barycentric, the Moon geocentric, and
// body 3 is the Earth-Moon barycentre.
//
// A DeFile caches its last-read record and is not safe for concurrent use;
// give each thread its own DeFile.
#ifndef PROMETHEIA_DE_HPP
#define PROMETHEIA_DE_HPP

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "prometheia/error.hpp"

namespace prometheia::de {

// Pointer-table column numbering (GROUP 1050 order).
enum class Body : int {
    Mercury = 1,
    Venus = 2,
    EarthMoonBary = 3, // barycentric
    Mars = 4,
    Jupiter = 5,
    Saturn = 6,
    Uranus = 7,
    Neptune = 8,
    Pluto = 9,
    Moon = 10,             // geocentric
    Sun = 11,              // barycentric
    Nutations = 12,        // 2 components: dpsi, deps (IAU 1980 model)
    Librations = 13,       // 3 components: lunar mantle Euler angles
    LunarMantleOmega = 14, // 3 components: lunar mantle angular velocity
    TTminusTDB = 15,       // 1 component: TT-TDB at the geocentre, seconds
};

constexpr int kColumnCount = 15;

// Components stored per body: 3, except nutations (2) and TT-TDB (1).
int component_count(Body body);

// Chebyshev block layout of one body inside a data record: the 1-based
// offset of the body's first coefficient (counting the two epoch
// doubles that lead every record), the coefficient count per component
// per subinterval, and the number of subintervals per record.
struct BodyLayout {
    int offset = 0; // 0 (or ncoeff 0) marks an absent body
    int ncoeff = 0;
    int nsubint = 0;
};

struct Header {
    bool byte_swapped = false; // file byte order differs from little-endian
    std::string title;
    double start_jed = 0.0;
    double end_jed = 0.0;
    double interval_days = 0.0;
    int denum = 0;
    int record_doubles = 0;
    uint64_t record_count = 0;
    double au_km = 0.0;
    double emrat = 0.0; // Earth/Moon mass ratio
    BodyLayout bodies[kColumnCount] = {};
    std::vector<std::string> constant_names;
    std::vector<double> constant_values;
};

// Composite targets for relative_state(), in JPL's customary numbering.
enum class Target : int {
    Mercury = 1,
    Venus = 2,
    Earth = 3,
    Mars = 4,
    Jupiter = 5,
    Saturn = 6,
    Uranus = 7,
    Neptune = 8,
    Pluto = 9,
    Moon = 10,
    Sun = 11,
    SolarSystemBary = 12,
    EarthMoonBary = 13,
};

class DeFile {
public:
    DeFile() = default;

    // Opens and validates a DE binary. The file stays open for lazy
    // record reads; the last decoded record is cached.
    static Result<DeFile> open(const std::string& path);

    const Header& header() const { return header_; }
    bool has_body(Body body) const;

    // Constant value by name (e.g. "AU", "EMRAT", "GMS"), or nullopt.
    std::optional<double> constant(std::string_view name) const;

    // Raw column evaluation at a TDB Julian Ephemeris Date:
    // out = {c0, c1, c2, c0', c1', c2'} with rates per day; unused
    // components (nutations: index 2; TT-TDB: 1 and 2) are zero.
    Result<void> state(Body body, double jed, double out[6]) const;

    // State of target relative to center (km, km/day), composing
    // Earth = EMB - Moon/(1+EMRAT) and Moon = Earth + geocentric Moon.
    Result<void> relative_state(Target target, Target center, double jed, double out[6]) const;

private:
    Result<void> load_record(uint64_t index) const;
    Result<void> barycentric(Target target, double jed, double out[6]) const;

    Header header_;
    std::string path_;
    mutable std::ifstream file_;
    mutable uint64_t cached_record_ = ~uint64_t{0};
    mutable std::vector<double> cache_;
};

} // namespace prometheia::de

#endif // PROMETHEIA_DE_HPP

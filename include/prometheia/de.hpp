// SPDX-License-Identifier: GPL-2.0-or-later
//
// Reader for JPL planetary ephemeris binaries ("DExxx" files).
//
// Supported: the old-generation binary layout (DE200 era, e.g. the
// lnxm1600p2170.200 republished by JPL). Old-format files embed the
// constant names, values and epoch range in two header records, but NOT
// the GROUP 1050 Chebyshev pointer table, so that table is supplied
// per-DENUM from the ephemeris' published ASCII header (public-domain
// JPL data; see docs/DE.md). The modern header-record layout (DE405+)
// is a planned addition.
//
// Units and frames: positions in kilometres, velocities in km/day,
// rectangular coordinates in the ephemeris' own equatorial frame, time
// argument is the TDB-scaled Julian Ephemeris Date. DE200 bodies are
// heliocentric (planets), geocentric (Moon) and barycentric (Sun).
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

// JPL DE binary body numbering (the GROUP 1050 row order).
enum class Body : int {
    Mercury = 1,
    Venus = 2,
    EarthMoonBary = 3,
    Mars = 4,
    Jupiter = 5,
    Saturn = 6,
    Uranus = 7,
    Neptune = 8,
    Pluto = 9,
    Moon = 10, // geocentric
    Sun = 11,  // barycentric
    Libration = 12,
};

// Chebyshev block layout of one body inside a data record: the 1-based
// offset of the body's first coefficient (counting the two epoch
// doubles that lead every record), the coefficient count per component
// per subinterval, and the number of subintervals per record.
struct BodyLayout {
    int offset = 0; // 0 marks an absent body (e.g. Libration in DE200)
    int ncoeff = 0;
    int nsubint = 0;
};

// Old-format layout spec for one DExxx release.
struct OldFormatSpec {
    int denum = 0;
    int record_doubles = 0;     // NCOEFF: f64 words per record (header and data)
    int constant_count = 0;     // NVALS: constants named in the file
    int name_slot_count = 0;    // fixed 6-char name slots in header record 1
    BodyLayout bodies[12] = {}; // indexed by int(Body) - 1
    const char* source = "";    // provenance of the pointer table
};

// Built-in spec for a supported old-format DENUM, or nullptr.
const OldFormatSpec* old_format_spec(int denum);

struct Header {
    std::string title;
    double start_jed = 0.0;
    double end_jed = 0.0;
    double interval_days = 0.0;
    int denum = 0;
    uint64_t record_count = 0;
    std::vector<std::string> constant_names;
    std::vector<double> constant_values;
};

class DeFile {
public:
    DeFile() = default;

    // Opens and validates an old-format DE binary. The file is kept open
    // for on-demand record reads; data records are decoded lazily and the
    // last one is cached.
    static Result<DeFile> open(const std::string& path);

    const Header& header() const { return header_; }
    const OldFormatSpec* spec() const { return spec_; }
    bool has_body(Body body) const;

    // Constant value by name (e.g. "AU", "EMRAT", "GMS"), or nullopt.
    std::optional<double> constant(std::string_view name) const;

    // Position (km) and velocity (km/day) of a body at the TDB Julian
    // Ephemeris Date. out = {x, y, z, vx, vy, vz}.
    Result<void> state(Body body, double jed, double out[6]) const;

private:
    Result<void> load_record(uint64_t index) const;

    Header header_;
    const OldFormatSpec* spec_ = nullptr;
    std::string path_;
    mutable std::ifstream file_;
    mutable uint64_t cached_record_ = ~uint64_t{0};
    mutable std::vector<double> cache_;
};

} // namespace prometheia::de

#endif // PROMETHEIA_DE_HPP
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The wire map: what the numbers in Astrolog's ephemeris protocol (version 3)
// mean to this engine. The protocol names bodies, flag bits and sidereal
// modes by the Swiss Ephemeris numbering. This repository is a cleanroom and
// holds none of those numbers: they come from the interface table in
// Astrolog's protocol specification, written into a wire-map file that
// prometheiad loads at startup. Without an entry a number means nothing, and
// the objects that use it fail with NaN rows (docs/SERVER.md).
//
// File format (docs/SERVER.md, "Wire map"): one entry per line, fields
// separated by whitespace, '#' to the end of the line is a comment.
//   body <wire-id> <naif-id> [name]    a body the engine knows by NAIF id
//   asteroids <wire-base>              wire id base + N is numbered asteroid
//                                      N, SBDB SPK-ID 20000000 + N (N >= 1)
//   flag <bit> <meaning>               bit 0-31 of the request's iflag
//   sidereal <wire-mode> <zodiac>      fagan-bradley | lahiri | user
// Flag meanings: speed, heliocentric, barycentric, topocentric, equatorial,
// j2000, icrs, no-nutation, true-position, no-aberration, no-deflection,
// astrometric, sidereal, xyz, radians, ignore.
#ifndef PROMETHEIA_SERVER_WIRE_MAP_HPP
#define PROMETHEIA_SERVER_WIRE_MAP_HPP

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "prometheia/engine.hpp"
#include "prometheia/error.hpp"

namespace prometheia::server {

enum class FlagMeaning {
    None, // the bit has no entry
    Speed,
    Heliocentric,
    Barycentric,
    Topocentric,
    Equatorial,
    J2000,
    Icrs,
    NoNutation,
    TruePosition,
    NoAberration,
    NoDeflection,
    Astrometric,
    Sidereal,
    Xyz,
    Radians,
    Ignore,
};

std::string_view flag_meaning_name(FlagMeaning meaning);

struct WireBody {
    int naif_id = 0;
    std::string name; // for the DATA metadata; may be empty
};

class WireMap {
public:
    static Result<WireMap> parse(std::string_view text);
    static Result<WireMap> load(const std::string& path);

    bool empty() const;

    // The body a wire id names, if the map has it.
    std::optional<WireBody> body(uint32_t wire_id) const;
    FlagMeaning flag(unsigned bit) const { return bit < 32 ? flags_[bit] : FlagMeaning::None; }
    std::optional<SiderealMode> sidereal(int32_t wire_mode) const;

private:
    std::map<uint32_t, WireBody> bodies_;
    std::optional<uint32_t> asteroid_base_;
    std::array<FlagMeaning, 32> flags_{};
    std::map<int32_t, SiderealMode> sidereal_;
};

} // namespace prometheia::server

#endif // PROMETHEIA_SERVER_WIRE_MAP_HPP

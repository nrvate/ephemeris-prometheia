// SPDX-License-Identifier: GPL-2.0-or-later
#include "wire_map.hpp"

#include <charconv>
#include <fstream>
#include <sstream>
#include <vector>

namespace prometheia::server {
namespace {

constexpr std::pair<FlagMeaning, std::string_view> kMeanings[] = {
    {FlagMeaning::Speed, "speed"},
    {FlagMeaning::Heliocentric, "heliocentric"},
    {FlagMeaning::Barycentric, "barycentric"},
    {FlagMeaning::Topocentric, "topocentric"},
    {FlagMeaning::Equatorial, "equatorial"},
    {FlagMeaning::J2000, "j2000"},
    {FlagMeaning::Icrs, "icrs"},
    {FlagMeaning::NoNutation, "no-nutation"},
    {FlagMeaning::TruePosition, "true-position"},
    {FlagMeaning::NoAberration, "no-aberration"},
    {FlagMeaning::NoDeflection, "no-deflection"},
    {FlagMeaning::Astrometric, "astrometric"},
    {FlagMeaning::Sidereal, "sidereal"},
    {FlagMeaning::Xyz, "xyz"},
    {FlagMeaning::Radians, "radians"},
    {FlagMeaning::Ignore, "ignore"},
};

constexpr std::pair<SiderealMode, std::string_view> kZodiacs[] = {
    {SiderealMode::FaganBradley, "fagan-bradley"},
    {SiderealMode::Lahiri, "lahiri"},
    {SiderealMode::User, "user"},
};

// A 32-bit wire id, or an int32 wire mode: std::from_chars over the whole
// field. The SPK-ID space holds NAIF ids of either sign.
template <typename T>
bool parse_int(std::string_view s, T& out) {
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && end == s.data() + s.size();
}

std::vector<std::string_view> fields_of(std::string_view line) {
    std::vector<std::string_view> fields;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
            ++i;
        }
        size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '\r') {
            ++j;
        }
        if (j > i) {
            fields.push_back(line.substr(i, j - i));
        }
        i = j;
    }
    return fields;
}

} // namespace

std::string_view flag_meaning_name(FlagMeaning meaning) {
    for (const auto& [m, name] : kMeanings) {
        if (m == meaning) {
            return name;
        }
    }
    return "none";
}

Result<WireMap> WireMap::parse(std::string_view text) {
    WireMap map;
    int line_no = 0;
    auto fail = [&](const std::string& why) {
        return make_error(ErrorCode::FormatError,
                          "wire map line " + std::to_string(line_no) + ": " + why);
    };
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++line_no;
        if (const size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        const std::vector<std::string_view> f = fields_of(line);
        if (f.empty()) {
            continue;
        }
        if (f[0] == "body") {
            uint32_t wire = 0;
            int naif = 0;
            if (f.size() < 3 || !parse_int(f[1], wire) || !parse_int(f[2], naif)) {
                return fail("expected: body <wire-id> <naif-id> [name]");
            }
            if (map.bodies_.count(wire)) {
                return fail("body " + std::to_string(wire) + " is mapped twice");
            }
            WireBody body{naif, {}};
            for (size_t i = 3; i < f.size(); ++i) {
                body.name += (i > 3 ? " " : "") + std::string(f[i]);
            }
            map.bodies_.emplace(wire, std::move(body));
        } else if (f[0] == "asteroids") {
            uint32_t base = 0;
            if (f.size() != 2 || !parse_int(f[1], base)) {
                return fail("expected: asteroids <wire-base>");
            }
            if (map.asteroid_base_) {
                return fail("asteroids is given twice");
            }
            map.asteroid_base_ = base;
        } else if (f[0] == "flag") {
            unsigned bit = 0;
            if (f.size() != 3 || !parse_int(f[1], bit) || bit > 31) {
                return fail("expected: flag <bit 0-31> <meaning>");
            }
            FlagMeaning meaning = FlagMeaning::None;
            for (const auto& [m, name] : kMeanings) {
                if (f[2] == name) {
                    meaning = m;
                }
            }
            if (meaning == FlagMeaning::None) {
                return fail("unknown flag meaning '" + std::string(f[2]) + "'");
            }
            if (map.flags_[bit] != FlagMeaning::None) {
                return fail("flag bit " + std::to_string(bit) + " is mapped twice");
            }
            map.flags_[bit] = meaning;
        } else if (f[0] == "sidereal") {
            int32_t mode = 0;
            if (f.size() != 3 || !parse_int(f[1], mode)) {
                return fail("expected: sidereal <wire-mode> <zodiac>");
            }
            std::optional<SiderealMode> zodiac;
            for (const auto& [z, name] : kZodiacs) {
                if (f[2] == name) {
                    zodiac = z;
                }
            }
            if (!zodiac) {
                return fail("unknown zodiac '" + std::string(f[2]) + "'");
            }
            if (!map.sidereal_.emplace(mode, *zodiac).second) {
                return fail("sidereal mode " + std::to_string(mode) + " is mapped twice");
            }
        } else {
            return fail("unknown entry '" + std::string(f[0]) + "'");
        }
    }
    return map;
}

Result<WireMap> WireMap::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return make_error(ErrorCode::IoError, "cannot open wire map " + path);
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parse(text.str());
}

bool WireMap::empty() const {
    for (FlagMeaning m : flags_) {
        if (m != FlagMeaning::None) {
            return false;
        }
    }
    return bodies_.empty() && !asteroid_base_ && sidereal_.empty();
}

std::optional<WireBody> WireMap::body(uint32_t wire_id) const {
    if (const auto it = bodies_.find(wire_id); it != bodies_.end()) {
        return it->second;
    }
    if (asteroid_base_ && wire_id > *asteroid_base_) {
        const uint32_t number = wire_id - *asteroid_base_;
        if (number < 10000000u) {
            return WireBody{int(20000000u + number), {}};
        }
    }
    return std::nullopt;
}

std::optional<SiderealMode> WireMap::sidereal(int32_t wire_mode) const {
    if (const auto it = sidereal_.find(wire_mode); it != sidereal_.end()) {
        return it->second;
    }
    return std::nullopt;
}

} // namespace prometheia::server

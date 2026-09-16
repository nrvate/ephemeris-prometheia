// SPDX-License-Identifier: GPL-2.0-or-later
//
// Reader for NAIF SPK ephemeris kernels (".bsp"), the Double precision
// Array File (DAF) container JPL distributes its DE ephemerides in
// (e.g. de440s.bsp), written from NAIF's public format documentation.
//
// Supported segment types: 2 (Chebyshev position; velocity by
// differentiation) and 3 (Chebyshev position and velocity), the types
// the planetary DE kernels use. Either byte order ("LTL-IEEE" or
// "BIG-IEEE") is accepted.
//
// Units match prometheia::de: kilometres and km/day, time argument the
// TDB Julian Date (SPK's native argument, TDB seconds past J2000, is
// derived internally). Bodies use NAIF integer IDs (0 = solar-system
// barycentre, 3 = Earth-Moon barycentre, 10 = Sun, 301 = Moon, 399 = Earth,
// 199/299 = Mercury/Venus, 1..9 = planetary-system barycentres).
//
// An SpkFile caches decoded records and is not safe for concurrent use;
// give each thread its own SpkFile.
#ifndef PROMETHEIA_SPK_HPP
#define PROMETHEIA_SPK_HPP

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "prometheia/error.hpp"

namespace prometheia::spk {

// NAIF IDs of the bodies in JPL's planetary kernels.
namespace naif {
constexpr int kSolarSystemBary = 0;
constexpr int kMercuryBary = 1;
constexpr int kVenusBary = 2;
constexpr int kEarthMoonBary = 3;
constexpr int kMarsBary = 4;
constexpr int kJupiterBary = 5;
constexpr int kSaturnBary = 6;
constexpr int kUranusBary = 7;
constexpr int kNeptuneBary = 8;
constexpr int kPlutoBary = 9;
constexpr int kSun = 10;
constexpr int kMercury = 199;
constexpr int kVenus = 299;
constexpr int kMoon = 301;
constexpr int kEarth = 399;
} // namespace naif

// One SPK segment: its summary plus the type-2/3 directory trailer.
struct Segment {
    std::string name;      // segment identifier (up to 40 characters)
    double start_et = 0.0; // coverage, TDB seconds past J2000
    double end_et = 0.0;
    int target = 0;          // NAIF ID
    int center = 0;          // NAIF ID
    int frame = 0;           // 1 = J2000 (ICRF for DE kernels)
    int type = 0;            // 2 or 3 (others are listed but not evaluable)
    uint64_t begin_word = 0; // 1-based DAF word addresses
    uint64_t end_word = 0;
    // Type 2/3 directory (from the last four words of the segment).
    double init_et = 0.0;    // start of the first record
    double interval_s = 0.0; // record length, seconds
    int record_words = 0;    // words per record
    uint64_t record_count = 0;
    int degree = 0; // Chebyshev degree per component
};

class SpkFile {
public:
    SpkFile() = default;

    // Opens a DAF/SPK file and reads every segment summary. Segment data
    // is decoded lazily.
    static Result<SpkFile> open(const std::string& path);

    const std::vector<Segment>& segments() const { return segments_; }
    const std::string& internal_name() const { return internal_name_; }
    bool byte_swapped() const { return swap_; }

    // TDB seconds past J2000 <-> TDB Julian Date.
    static double et_from_jd(double jd_tdb) { return (jd_tdb - 2451545.0) * 86400.0; }

    // State of a segment's target relative to its center, km and km/day.
    Result<void> segment_state(size_t segment, double jd_tdb, double out[6]) const {
        return segment_state_et(segment, et_from_jd(jd_tdb), out);
    }
    Result<void> segment_state_et(size_t segment, double et, double out[6]) const;

    // State of target relative to center (km, km/day), chaining segments
    // through their common ancestor in the segment tree. Where several
    // segments cover the same body and epoch, the one later in the file
    // wins (the SPK precedence rule). The _et form takes TDB seconds past
    // J2000 directly: a JD double near the present only resolves ~40 us,
    // which matters at exact segment boundaries.
    Result<void> state(int target, int center, double jd_tdb, double out[6]) const {
        return state_et(target, center, et_from_jd(jd_tdb), out);
    }
    Result<void> state_et(int target, int center, double et, double out[6]) const;

private:
    // Segment giving `body` relative to its parent at et, or -1.
    long find_segment(int body, double et) const;
    Result<void> state_to_root(int body, double et, std::vector<int>& chain,
                               std::vector<double>& states) const;
    Result<void> read_words(uint64_t first_word, size_t count, std::vector<double>& out) const;

    std::vector<Segment> segments_;
    std::string internal_name_;
    std::string path_;
    bool swap_ = false;
    mutable std::ifstream file_;
    // One cached record per segment: index into segments_ -> record.
    mutable std::vector<uint64_t> cached_index_;
    mutable std::vector<std::vector<double>> cached_record_;
};

} // namespace prometheia::spk

#endif // PROMETHEIA_SPK_HPP

// SPDX-License-Identifier: GPL-2.0-or-later
//
// The server's segment lattice and its cache.
//
// A client asks for segments over the span it cares about. The server does
// not fit that span: it fits the fixed lattice cells that cover it, so two
// clients asking overlapping spans share the work rather than each paying
// for a fit keyed to its own start instant. The client is given contiguous
// coverage of what it asked for, plus a little either side, which is what it
// wanted anyway.
//
// The lattice and the error ladder are the SERVER's. A client that could name
// a cell boundary, or ask for an error the server had to honour exactly,
// could defeat the sharing without meaning to. Quantisation therefore only
// ever moves toward a FINER fit: the client gets at least the accuracy it
// asked for, and the segment still publishes the residual that was measured.
//
// Two kinds of cell live here: a body's rectangular position (three Chebyshev
// axes) and a scalar series such as an ayanamsa. Both walk the same lattice
// under the same ladder; the caches are separate because the values are.
//
// Nothing here knows about any wire format. The caller supplies a key that
// identifies everything but the span — dataset, object, profile — and a
// sampler; see docs/SERVER.md.
#ifndef PROMETHEIA_SERVER_SEGCACHE_HPP
#define PROMETHEIA_SERVER_SEGCACHE_HPP

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "prometheia/segments.hpp"

namespace prometheia::server {

// Fixed cells of 32 days, aligned from J2000 TT. Chosen so a month-long
// window is one or two cells and a year is twelve or thirteen.
struct Lattice {
    static constexpr double kEpochJdTt = 2451545.0;
    static constexpr double kCellDays = 32.0;

    static long long cell_of(double jd_tt);
    static double cell_start(long long cell);
    static double cell_end(long long cell) { return cell_start(cell + 1); }
};

// The rungs, in arcseconds. A request is served at the first rung no coarser
// than it asked for. The last rung is the floor: a request finer than it is
// refused rather than served coarser, because serving it coarser is the one
// thing quantisation must never do. The protocol layer advertises this floor
// so a client never has to discover it by being refused.
inline constexpr double kErrorLadder[] = {1.0, 0.1, 0.01, 0.001};
inline constexpr double kFinestErrArcsec = 0.001;

// The rung index a request lands on, and its arcsecond value.
size_t ladder_rung(double asked_err_arcsec);
double quantise_target(double asked_err_arcsec);

// One cell's fit.
using CellSegments = std::vector<segments::Segment>;
using ScalarCell = std::vector<segments::ScalarSegment>;

size_t cell_bytes(const CellSegments& cell);
size_t cell_bytes(const ScalarCell& cell);

// Least-recently-used cells under a byte budget. One per event loop, like
// the engine and the result cache.
template <typename Cell>
class CellCache {
public:
    explicit CellCache(size_t budget_bytes) : budget_(budget_bytes) {}

    std::shared_ptr<const Cell> get(const std::string& key);
    void put(const std::string& key, std::shared_ptr<const Cell> cell);

    size_t entries() const { return map_.size(); }
    size_t used_bytes() const { return used_; }
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }

private:
    using Lru = std::list<std::pair<std::string, std::shared_ptr<const Cell>>>;
    using LruIter = typename Lru::iterator;
    size_t budget_;
    size_t used_ = 0;
    uint64_t hits_ = 0, misses_ = 0;
    Lru lru_;
    std::unordered_map<std::string, LruIter> map_;
};

using SegmentCache = CellCache<CellSegments>;
using ScalarSegmentCache = CellCache<ScalarCell>;

struct SegmentRequest {
    double jd_from_tt = 0.0;
    double jd_to_tt = 0.0;
    double target_err_arcsec = 0.1; // as asked; quantised downward by the service
    int min_degree = 6;             // the fitter raises from here
    int max_degree = 16;            // as asked; clamped to what the fitter allows
    size_t max_cells = 16;          // the span bound, in cells
};

struct SegmentAnswer {
    std::vector<segments::Segment> segments; // contiguous, in time order
    double err_arcsec_served = 0.0;          // the rung actually fitted
    double jd_from_tt = 0.0;                 // the covered span: whole cells,
    double jd_to_tt = 0.0;                   // so wider than what was asked
    size_t cells = 0;
    size_t cells_fitted = 0; // the ones this request paid for
    size_t sampler_calls = 0;
};

// The same answer for a scalar series, the residual in the value's own units
// (an ayanamsa is fitted in degrees and its rung converted with 3600).
struct ScalarAnswer {
    std::vector<segments::ScalarSegment> segments;
    double err_served = 0.0;
    double jd_from_tt = 0.0;
    double jd_to_tt = 0.0;
    size_t cells = 0;
    size_t cells_fitted = 0;
    size_t sampler_calls = 0;
};

// Serves a span from the lattice, fitting only the cells not already cached.
// `key_prefix` identifies everything except the span and the rung: dataset,
// object and profile. ArgumentError where the span is empty, inverted, or
// needs more than `max_cells` cells; the sampler's own error is returned
// unchanged and nothing is cached for the cell that failed.
Result<SegmentAnswer> segments_for(SegmentCache& cache, std::string_view key_prefix,
                                   const SegmentRequest& request, const segments::Sampler& sampler);

// The scalar twin, for a series the wire carries per sidereal profile.
// `target_value` is the rung in the value's own units.
Result<ScalarAnswer> scalar_series_for(ScalarSegmentCache& cache, std::string_view key_prefix,
                                       const SegmentRequest& request, double target_value,
                                       const segments::ScalarSampler& sampler);

} // namespace prometheia::server

#endif // PROMETHEIA_SERVER_SEGCACHE_HPP

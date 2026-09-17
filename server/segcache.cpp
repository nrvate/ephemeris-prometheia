// SPDX-License-Identifier: GPL-2.0-or-later
#include "segcache.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace prometheia::server {

long long Lattice::cell_of(double jd_tt) {
    return static_cast<long long>(std::floor((jd_tt - kEpochJdTt) / kCellDays));
}

double Lattice::cell_start(long long cell) {
    return kEpochJdTt + double(cell) * kCellDays;
}

size_t ladder_rung(double asked_err_arcsec) {
    const size_t last = std::size(kErrorLadder) - 1;
    for (size_t i = 0; i <= last; ++i) {
        // No coarser than asked: the client gets at least what it wanted.
        if (kErrorLadder[i] <= asked_err_arcsec) {
            return i;
        }
    }
    return last;
}

double quantise_target(double asked_err_arcsec) {
    return kErrorLadder[ladder_rung(asked_err_arcsec)];
}

size_t SegmentCache::bytes_of(const CellSegments& cell) {
    size_t n = sizeof(CellSegments);
    for (const segments::Segment& s : cell) {
        n += sizeof(s) + (s.x.size() + s.y.size() + s.z.size()) * sizeof(double);
    }
    return n;
}

std::shared_ptr<const CellSegments> SegmentCache::get(const std::string& key) {
    const auto it = map_.find(key);
    if (it == map_.end()) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->second;
}

void SegmentCache::put(const std::string& key, std::shared_ptr<const CellSegments> cell) {
    const size_t charge = bytes_of(*cell) + 2 * key.size();
    if (charge > budget_ || map_.count(key)) {
        return;
    }
    while (used_ + charge > budget_ && !lru_.empty()) {
        const auto& back = lru_.back();
        used_ -= bytes_of(*back.second) + 2 * back.first.size();
        map_.erase(back.first);
        lru_.pop_back();
    }
    lru_.emplace_front(key, std::move(cell));
    map_.emplace(key, lru_.begin());
    used_ += charge;
}

namespace {

// The cache key: everything the caller says identifies the object and the
// profile, then the rung and the cell. Both are appended as raw bytes rather
// than formatted, so no cell can collide with another by spelling.
std::string cell_key(std::string_view prefix, size_t rung, long long cell) {
    std::string key;
    key.reserve(prefix.size() + 1 + sizeof(uint8_t) + sizeof(cell));
    key.append(prefix);
    key.push_back('\0');
    const auto r = uint8_t(rung);
    key.append(reinterpret_cast<const char*>(&r), sizeof(r));
    key.append(reinterpret_cast<const char*>(&cell), sizeof(cell));
    return key;
}

} // namespace

Result<SegmentAnswer> segments_for(SegmentCache& cache, std::string_view key_prefix,
                                   const SegmentRequest& request,
                                   const segments::Sampler& sampler) {
    if (!(request.jd_to_tt > request.jd_from_tt)) {
        return make_error(ErrorCode::ArgumentError, "segments: the span is empty");
    }
    if (!std::isfinite(request.jd_from_tt) || !std::isfinite(request.jd_to_tt)) {
        return make_error(ErrorCode::ArgumentError, "segments: the span is not finite");
    }
    if (request.max_cells == 0) {
        return make_error(ErrorCode::ArgumentError, "segments: no cells allowed");
    }
    if (!(request.target_err_arcsec > 0.0)) {
        return make_error(ErrorCode::ArgumentError, "segments: the target error is not positive");
    }
    if (request.target_err_arcsec < kFinestErrArcsec) {
        return make_error(ErrorCode::ArgumentError,
                          "segments: the finest error this server fits is " +
                              std::to_string(kFinestErrArcsec) +
                              " arcsec; asking finer would be served coarser");
    }

    const long long first = Lattice::cell_of(request.jd_from_tt);
    // A span ending exactly on a boundary does not reach into the next cell.
    const double end = request.jd_to_tt;
    long long last = Lattice::cell_of(end);
    if (Lattice::cell_start(last) == end && last > first) {
        --last;
    }
    const auto needed = size_t(last - first + 1);
    if (needed > request.max_cells) {
        return make_error(ErrorCode::ArgumentError,
                          "segments: the span covers " + std::to_string(needed) +
                              " lattice cells of " + std::to_string(int(Lattice::kCellDays)) +
                              " days, more than the " + std::to_string(request.max_cells) +
                              " allowed");
    }

    const size_t rung = ladder_rung(request.target_err_arcsec);
    SegmentAnswer answer;
    answer.err_arcsec_served = kErrorLadder[rung];
    answer.jd_from_tt = Lattice::cell_start(first);
    answer.jd_to_tt = Lattice::cell_end(last);
    answer.cells = needed;

    segments::FitOptions options;
    options.target_err_arcsec = answer.err_arcsec_served;
    options.max_degree = std::clamp(request.max_degree, options.min_degree, 31);

    for (long long cell = first; cell <= last; ++cell) {
        const std::string key = cell_key(key_prefix, rung, cell);
        std::shared_ptr<const CellSegments> fitted = cache.get(key);
        if (!fitted) {
            auto report =
                segments::fit(sampler, Lattice::cell_start(cell), Lattice::cell_end(cell), options);
            if (!report) {
                return report.error();
            }
            answer.sampler_calls += report.value().sampler_calls;
            ++answer.cells_fitted;
            auto owned = std::make_shared<CellSegments>(std::move(report).value().segments);
            cache.put(key, owned);
            fitted = std::move(owned);
        }
        answer.segments.insert(answer.segments.end(), fitted->begin(), fitted->end());
    }
    return answer;
}

} // namespace prometheia::server

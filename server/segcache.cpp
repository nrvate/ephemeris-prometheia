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

size_t cell_bytes(const CellSegments& cell) {
    size_t n = sizeof(CellSegments);
    for (const segments::Segment& s : cell) {
        n += sizeof(s) + (s.x.size() + s.y.size() + s.z.size()) * sizeof(double);
    }
    return n;
}

size_t cell_bytes(const ScalarCell& cell) {
    size_t n = sizeof(ScalarCell);
    for (const segments::ScalarSegment& s : cell) {
        n += sizeof(s) + s.c.size() * sizeof(double);
    }
    return n;
}

template <typename Cell>
std::shared_ptr<const Cell> CellCache<Cell>::get(const std::string& key) {
    const auto it = map_.find(key);
    if (it == map_.end()) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->second;
}

template <typename Cell>
void CellCache<Cell>::put(const std::string& key, std::shared_ptr<const Cell> cell) {
    const size_t charge = cell_bytes(*cell) + 2 * key.size();
    if (charge > budget_ || map_.count(key)) {
        return;
    }
    while (used_ + charge > budget_ && !lru_.empty()) {
        const auto& back = lru_.back();
        used_ -= cell_bytes(*back.second) + 2 * back.first.size();
        map_.erase(back.first);
        lru_.pop_back();
    }
    lru_.emplace_front(key, std::move(cell));
    map_.emplace(key, lru_.begin());
    used_ += charge;
}

template class CellCache<CellSegments>;
template class CellCache<ScalarCell>;

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

// The span's cells and rung, after every refusal that costs no work. Both
// services share it; only the fitting differs.
struct CellPlan {
    long long first = 0, last = 0;
    size_t cells = 0;
    size_t rung = 0;
    double served_arcsec = 0.0;
    double jd_from_tt = 0.0, jd_to_tt = 0.0;
};

Result<CellPlan> plan_cells(const SegmentRequest& request) {
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

    CellPlan plan;
    plan.first = Lattice::cell_of(request.jd_from_tt);
    // A span ending exactly on a boundary does not reach into the next cell.
    long long last = Lattice::cell_of(request.jd_to_tt);
    if (Lattice::cell_start(last) == request.jd_to_tt && last > plan.first) {
        --last;
    }
    plan.last = last;
    plan.cells = size_t(last - plan.first + 1);
    if (plan.cells > request.max_cells) {
        return make_error(ErrorCode::ArgumentError,
                          "segments: the span covers " + std::to_string(plan.cells) +
                              " lattice cells of " + std::to_string(int(Lattice::kCellDays)) +
                              " days, more than the " + std::to_string(request.max_cells) +
                              " allowed");
    }
    plan.rung = ladder_rung(request.target_err_arcsec);
    plan.served_arcsec = kErrorLadder[plan.rung];
    plan.jd_from_tt = Lattice::cell_start(plan.first);
    plan.jd_to_tt = Lattice::cell_end(plan.last);
    return plan;
}

} // namespace

Result<SegmentAnswer> segments_for(SegmentCache& cache, std::string_view key_prefix,
                                   const SegmentRequest& request,
                                   const segments::Sampler& sampler) {
    auto planned = plan_cells(request);
    if (!planned) {
        return planned.error();
    }
    const CellPlan& plan = planned.value();

    SegmentAnswer answer;
    answer.err_arcsec_served = plan.served_arcsec;
    answer.jd_from_tt = plan.jd_from_tt;
    answer.jd_to_tt = plan.jd_to_tt;
    answer.cells = plan.cells;

    segments::FitOptions options;
    options.target_err_arcsec = answer.err_arcsec_served;
    options.max_degree = std::clamp(request.max_degree, 1, 31);
    options.min_degree = std::clamp(request.min_degree, 1, options.max_degree);

    for (long long cell = plan.first; cell <= plan.last; ++cell) {
        const std::string key = cell_key(key_prefix, plan.rung, cell);
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

Result<ScalarAnswer> scalar_series_for(ScalarSegmentCache& cache, std::string_view key_prefix,
                                       const SegmentRequest& request, double target_value,
                                       const segments::ScalarSampler& sampler) {
    auto planned = plan_cells(request);
    if (!planned) {
        return planned.error();
    }
    const CellPlan& plan = planned.value();

    ScalarAnswer answer;
    answer.err_served = target_value;
    answer.jd_from_tt = plan.jd_from_tt;
    answer.jd_to_tt = plan.jd_to_tt;
    answer.cells = plan.cells;

    segments::ScalarFitOptions options;
    options.target = target_value;
    options.max_degree = std::clamp(request.max_degree, 1, 31);
    options.min_degree = std::clamp(request.min_degree, 1, options.max_degree);

    for (long long cell = plan.first; cell <= plan.last; ++cell) {
        const std::string key = cell_key(key_prefix, plan.rung, cell);
        std::shared_ptr<const ScalarCell> fitted = cache.get(key);
        if (!fitted) {
            auto report = segments::fit_scalar(sampler, Lattice::cell_start(cell),
                                               Lattice::cell_end(cell), options);
            if (!report) {
                return report.error();
            }
            answer.sampler_calls += report.value().sampler_calls;
            ++answer.cells_fitted;
            auto owned = std::make_shared<ScalarCell>(std::move(report).value().segments);
            cache.put(key, owned);
            fitted = std::move(owned);
        }
        answer.segments.insert(answer.segments.end(), fitted->begin(), fitted->end());
    }
    return answer;
}

} // namespace prometheia::server

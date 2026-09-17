// SPDX-License-Identifier: GPL-2.0-or-later
//
// The server's segment lattice: cells are the server's, the error ladder
// only ever moves toward a finer fit, and two clients asking overlapping
// spans pay for the overlap once.
#include <cmath>
#include <cstdio>
#include <string>

#include "../server/segcache.hpp"

#include <doctest/doctest.h>

using namespace prometheia;
using namespace prometheia::server;

namespace {

// A sampler with a closed form and a call counter: circular motion at one
// AU, fast enough that a cell needs a real fit rather than a constant.
struct Circle {
    mutable size_t calls = 0;
    double rate_deg_per_day = 1.0;

    segments::Sampler sampler() const {
        return [this](double jd, double p[3], double v[3]) -> Result<void> {
            ++calls;
            const double w = rate_deg_per_day * 3.14159265358979323846 / 180.0;
            const double a = w * (jd - 2451545.0);
            p[0] = std::cos(a);
            p[1] = std::sin(a);
            p[2] = 0.0;
            v[0] = -w * std::sin(a);
            v[1] = w * std::cos(a);
            v[2] = 0.0;
            return {};
        };
    }
};

} // namespace

TEST_CASE("segcache_lattice_is_aligned_from_j2000") {
    CHECK(Lattice::cell_of(Lattice::kEpochJdTt) == 0);
    CHECK(Lattice::cell_start(0) == doctest::Approx(Lattice::kEpochJdTt));
    CHECK(Lattice::cell_of(Lattice::kEpochJdTt + 31.9) == 0);
    CHECK(Lattice::cell_of(Lattice::kEpochJdTt + 32.0) == 1);
    CHECK(Lattice::cell_of(Lattice::kEpochJdTt - 0.1) == -1);
    CHECK(Lattice::cell_end(-1) == doctest::Approx(Lattice::kEpochJdTt));
    // Cells meet exactly: no instant belongs to two, none to neither.
    for (long long c = -40; c < 40; ++c) {
        CHECK(Lattice::cell_end(c) == Lattice::cell_start(c + 1));
    }
}

TEST_CASE("segcache_ladder_never_rounds_coarser") {
    CHECK(quantise_target(1.0) == 1.0);
    CHECK(quantise_target(2.0) == 1.0);  // coarser than the ladder: served at 1"
    CHECK(quantise_target(0.12) == 0.1); // rounds toward the finer rung
    CHECK(quantise_target(0.1) == 0.1);
    CHECK(quantise_target(0.09) == 0.01);
    CHECK(quantise_target(0.001) == 0.001);
    // The property the client depends on, from the floor up.
    for (double asked = kFinestErrArcsec; asked < 4.0; asked *= 1.07) {
        CHECK(quantise_target(asked) <= asked * (1.0 + 1e-12));
    }
}

TEST_CASE("segcache_overlapping_spans_share_cells") {
    Circle body;
    SegmentCache cache(4u << 20);

    SegmentRequest a;
    a.jd_from_tt = 2461300.5;
    a.jd_to_tt = a.jd_from_tt + 90.0;
    a.target_err_arcsec = 0.1;
    auto first = segments_for(cache, "de440|jupiter|geo-true", a, body.sampler());
    REQUIRE_MESSAGE(first.ok(), first.error().message);
    const size_t calls_first = body.calls;
    CHECK(first.value().cells >= 3);
    CHECK(first.value().cells_fitted == first.value().cells);
    CHECK(calls_first > 0);

    // A second client, a different start instant, the same stretch of sky.
    SegmentRequest b = a;
    b.jd_from_tt = a.jd_from_tt + 33.0;
    b.jd_to_tt = b.jd_from_tt + 20.0;
    auto second = segments_for(cache, "de440|jupiter|geo-true", b, body.sampler());
    REQUIRE(second.ok());
    CHECK(second.value().cells_fitted == 0); // entirely inside what the first fitted
    CHECK(body.calls == calls_first);        // and it cost nothing to serve

    // A different profile shares nothing, by key.
    auto other = segments_for(cache, "de440|jupiter|helio-j2000", b, body.sampler());
    REQUIRE(other.ok());
    CHECK(other.value().cells_fitted == other.value().cells);
    CHECK(body.calls > calls_first);
}

TEST_CASE("segcache_answer_covers_the_span_and_is_contiguous") {
    Circle body;
    SegmentCache cache(4u << 20);
    SegmentRequest r;
    r.jd_from_tt = 2461310.25; // deliberately not on a boundary
    r.jd_to_tt = r.jd_from_tt + 45.0;
    r.target_err_arcsec = 0.1;
    auto got = segments_for(cache, "k", r, body.sampler());
    REQUIRE(got.ok());
    const SegmentAnswer& answer = got.value();

    // Whole cells, so the coverage is wider than what was asked for at both
    // ends, and never narrower.
    CHECK(answer.jd_from_tt <= r.jd_from_tt);
    CHECK(answer.jd_to_tt >= r.jd_to_tt);
    CHECK(answer.jd_from_tt ==
          doctest::Approx(Lattice::cell_start(Lattice::cell_of(r.jd_from_tt))));

    REQUIRE(!answer.segments.empty());
    const segments::Segment& front = answer.segments.front();
    const segments::Segment& back = answer.segments.back();
    CHECK(front.mid_jd_tt - front.half_span_days == doctest::Approx(answer.jd_from_tt));
    CHECK(back.mid_jd_tt + back.half_span_days == doctest::Approx(answer.jd_to_tt));
    for (size_t i = 1; i < answer.segments.size(); ++i) {
        const segments::Segment& p = answer.segments[i - 1];
        const segments::Segment& q = answer.segments[i];
        CHECK(std::fabs((p.mid_jd_tt + p.half_span_days) - (q.mid_jd_tt - q.half_span_days)) <
              1e-9);
    }

    // A span ending exactly on a boundary does not pull in the next cell.
    SegmentRequest exact;
    exact.jd_from_tt = Lattice::cell_start(400);
    exact.jd_to_tt = Lattice::cell_start(402);
    auto tight = segments_for(cache, "k", exact, body.sampler());
    REQUIRE(tight.ok());
    CHECK(tight.value().cells == 2);
}

TEST_CASE("segcache_refuses_a_span_it_was_not_allowed") {
    Circle body;
    SegmentCache cache(1u << 20);
    SegmentRequest r;
    r.jd_from_tt = 2461300.5;
    r.jd_to_tt = r.jd_from_tt + 365.25 * 3;
    r.max_cells = 12;
    auto too_wide = segments_for(cache, "k", r, body.sampler());
    REQUIRE(!too_wide.ok());
    CHECK(too_wide.error().code == ErrorCode::ArgumentError);
    CHECK(body.calls == 0); // refused before any work

    // Finer than the floor is refused, not quietly served coarser.
    SegmentRequest finer;
    finer.jd_from_tt = 2461300.5;
    finer.jd_to_tt = finer.jd_from_tt + 1.0;
    finer.target_err_arcsec = kFinestErrArcsec / 10.0;
    auto too_fine = segments_for(cache, "k", finer, body.sampler());
    REQUIRE(!too_fine.ok());
    CHECK(too_fine.error().code == ErrorCode::ArgumentError);
    CHECK(body.calls == 0);

    SegmentRequest empty;
    empty.jd_from_tt = empty.jd_to_tt = 2461300.5;
    CHECK(!segments_for(cache, "k", empty, body.sampler()).ok());

    // A failing sampler is reported, and leaves nothing cached behind it.
    segments::Sampler broken = [](double, double[3], double[3]) -> Result<void> {
        return make_error(ErrorCode::NotFound, "no such body");
    };
    SegmentRequest one;
    one.jd_from_tt = 2461300.5;
    one.jd_to_tt = one.jd_from_tt + 1.0;
    auto failed = segments_for(cache, "broken", one, broken);
    REQUIRE(!failed.ok());
    CHECK(failed.error().code == ErrorCode::NotFound);
    CHECK(cache.entries() == 0);
}

TEST_CASE("segcache_evicts_under_its_budget") {
    Circle body;
    // Small enough that a handful of cells will not fit.
    SegmentCache cache(8u << 10);
    SegmentRequest r;
    r.target_err_arcsec = 0.001;
    r.max_cells = 4;
    for (int i = 0; i < 40; ++i) {
        r.jd_from_tt = 2461300.5 + i * 32.0;
        r.jd_to_tt = r.jd_from_tt + 1.0;
        auto got = segments_for(cache, "k", r, body.sampler());
        REQUIRE(got.ok());
        CHECK(cache.used_bytes() <= 8u << 10);
    }
    CHECK(cache.entries() > 0);
    CHECK(cache.entries() < 40); // it did have to evict
}

TEST_CASE("segcache_segments_are_worth_evaluating") {
    // The cached cells answer instants, which is the whole point of serving
    // them instead of samples.
    Circle body;
    body.rate_deg_per_day = 13.0; // Moon-like
    SegmentCache cache(4u << 20);
    SegmentRequest r;
    r.jd_from_tt = 2461300.5;
    r.jd_to_tt = r.jd_from_tt + 60.0;
    r.target_err_arcsec = 0.01;
    auto got = segments_for(cache, "k", r, body.sampler());
    REQUIRE(got.ok());

    const auto truth = body.sampler();
    double worst = 0.0;
    for (int i = 0; i <= 600; ++i) {
        const double jd = r.jd_from_tt + i * 0.1;
        const segments::Segment* seg = nullptr;
        for (const segments::Segment& s : got.value().segments) {
            if (jd >= s.mid_jd_tt - s.half_span_days - 1e-9 &&
                jd <= s.mid_jd_tt + s.half_span_days + 1e-9) {
                seg = &s;
                break;
            }
        }
        REQUIRE(seg != nullptr);
        double p[3], want[3], v[3];
        seg->position(jd, p);
        REQUIRE(truth(jd, want, v).ok());
        const double dot = p[0] * want[0] + p[1] * want[1] + p[2] * want[2];
        const double cx[3] = {p[1] * want[2] - p[2] * want[1], p[2] * want[0] - p[0] * want[2],
                              p[0] * want[1] - p[1] * want[0]};
        const double cross = std::sqrt(cx[0] * cx[0] + cx[1] * cx[1] + cx[2] * cx[2]);
        worst = std::max(worst, std::atan2(cross, dot) * 180.0 / 3.14159265358979323846 * 3600.0);
    }
    CHECK(worst <= got.value().err_arcsec_served);
}

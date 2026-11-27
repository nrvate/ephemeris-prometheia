// SPDX-License-Identifier: GPL-2.0-or-later
//
// The roots search at its halving limits (src/segment_roots.hpp, Limits;
// docs/SEGMENTS.md, "Roots"). A cell that ends at a limit must be counted in
// unresolved, and the same window within reach must count none: a counter
// that never moves would pass every search that resolves. The limits are
// internal, so this suite calls the core directly. Needs DE440
// (PROMETHEIA_DE440, else ephe/linux_p1550p2650.440); SKIP when absent.
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

#include <prometheia/prometheia.h>

#include <doctest/doctest.h>

#include "../src/segment_roots.hpp"

namespace {

constexpr double kJ2026 = 2461041.5; // 2026-01-01 TT, near enough

std::string de440_path() {
    const char* env = std::getenv("PROMETHEIA_DE440");
    return (env && *env) ? std::string(env)
                         : std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_p1550p2650.440";
}

struct Search {
    std::vector<prometheia_segments_root> roots;
    int unresolved = 0;
};

Search search(const prometheia_segments_function& c,
              const prometheia::segment_roots::Limits& limits, double from, double to) {
    const prometheia_segments_term term{c.a, c.na, c.ca};
    prometheia::segment_roots::Function f;
    f.terms = &term;
    f.nterms = 1;
    f.component = c.component;
    f.target = c.target;
    f.limits = limits;
    Search s;
    s.roots.resize(4096);
    double resume = 0.0;
    const int n = prometheia::segment_roots::find(f, from, to, 1e-6, s.roots.data(),
                                                  int(s.roots.size()), &resume, &s.unresolved);
    REQUIRE(std::isnan(resume));
    s.roots.resize(size_t(n));
    return s;
}

} // namespace

TEST_CASE("segment_roots_unresolved_counts_cells_at_a_limit") {
    if (::access(de440_path().c_str(), R_OK) != 0) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    prometheia_engine* e = nullptr;
    REQUIRE(prometheia_engine_open(de440_path().c_str(), &e, nullptr) == PROMETHEIA_OK);
    prometheia_options o;
    prometheia_options_init(&o);
    const prometheia_object moon{PROMETHEIA_OBJECT_BODY, 301, nullptr};
    prometheia_segments* h = nullptr;
    REQUIRE(prometheia_segments_fit(e, &moon, kJ2026, kJ2026 + 60, &o, 0.001, &h, nullptr) ==
            PROMETHEIA_OK);
    const prometheia_segments* hs[1] = {h};
    // The Moon's latitude through 0: its nodes, about twice a month.
    prometheia_segments_function c{};
    c.a = hs;
    c.na = 1;
    c.ca = 1.0;
    c.component = PROMETHEIA_SEGMENTS_LAT;

    const Search full = search(c, {}, kJ2026, kJ2026 + 60);
    CHECK(full.unresolved == 0);
    CHECK(full.roots.size() == 4);

    // No halving at all: each piece is one cell, which the series' tail
    // does not trust, so every piece is unresolved.
    prometheia::segment_roots::Limits none;
    none.max_depth = 0;
    const Search cut = search(c, none, kJ2026, kJ2026 + 60);
    CHECK(cut.unresolved > 0);
    // Counted from the ends, a cell invents no root: every one it gives is
    // one the full search found.
    for (const prometheia_segments_root& r : cut.roots) {
        bool found = false;
        for (const prometheia_segments_root& q : full.roots)
            found = found || std::fabs(q.jd_tt - r.jd_tt) < 0.01;
        CHECK(found);
    }

    // The same limit by the narrowest cell: wider than a piece, nothing splits.
    prometheia::segment_roots::Limits wide;
    wide.min_cell_days = 100.0;
    CHECK(search(c, wide, kJ2026, kJ2026 + 60).unresolved > 0);

    // Only cells meeting the window count.
    const Search part = search(c, none, kJ2026 + 10, kJ2026 + 11);
    CHECK(part.unresolved >= 1);
    CHECK(part.unresolved < cut.unresolved);

    prometheia_segments_close(h);
    prometheia_engine_close(e);
}

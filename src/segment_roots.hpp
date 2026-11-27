// SPDX-License-Identifier: GPL-2.0-or-later
//
// Roots on segment handles (docs/SEGMENTS.md, "Roots"): the core of
// prometheia_segments_roots, which checks the arguments and calls find().
// It reads only the handles' layouts: no engine, no allocation, no lock.
#ifndef PROMETHEIA_SRC_SEGMENT_ROOTS_HPP
#define PROMETHEIA_SRC_SEGMENT_ROOTS_HPP

#include "prometheia/prometheia.h"

namespace prometheia::segment_roots {

// f(t) = sum of c_i X_i(t) - (target + target_rate (t - target_jd_tt)),
// taken modulo modulus when it is positive. Each term is count handles back
// to back. The caller has checked every field: 1 <= nterms <=
// PROMETHEIA_SEGMENTS_MAX_TERMS, modulus is resolved (360 for a longitude
// asked with 0) and is 0 for any other component.
// Where halving a cell stops (docs/SEGMENTS.md, "Roots", "Limits"). Only
// tests change them, to reach a limit on purpose.
struct Limits {
    int max_depth = 40;          // halvings of a piece
    int max_cells = 1 << 14;     // cells in one piece
    double min_cell_days = 1e-6; // the narrowest cell, whatever tol_days asks
};

struct Function {
    const prometheia_segments_term* terms = nullptr;
    int nterms = 0;
    int component = PROMETHEIA_SEGMENTS_LON;
    double target = 0.0, target_rate = 0.0, target_jd_tt = 0.0;
    double modulus = 0.0;
    Limits limits;
};

// Writes the roots in [from, to), in time order, at most capacity of them
// (capacity 0: none, only counted), and returns how many were written or
// counted. *resume is the instant of the first root not written, or NaN
// when every root was. *unresolved is the number of cells meeting the
// window that ended at a limit. [from, to] lies inside both terms' spans.
int find(const Function& f, double from, double to, double tol_days, prometheia_segments_root* out,
         int capacity, double* resume, int* unresolved) noexcept;

} // namespace prometheia::segment_roots

#endif

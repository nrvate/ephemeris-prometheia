// SPDX-License-Identifier: GPL-2.0-or-later
//
// Roots on segment handles (docs/SEGMENTS.md, "Roots"): the core of
// prometheia_segments_roots, which checks the arguments and calls find().
// It reads only the handles' layouts: no engine, no allocation, no lock.
#ifndef PROMETHEIA_SRC_SEGMENT_ROOTS_HPP
#define PROMETHEIA_SRC_SEGMENT_ROOTS_HPP

#include "prometheia/prometheia.h"

namespace prometheia::segment_roots {

// f(t) = ca Xa(t) + cb Xb(t) - (target + target_rate (t - target_jd_tt)),
// taken modulo modulus when it is positive. Each term is na (nb) handles
// back to back; nb is 0 for no second term. The caller has checked every
// field: modulus is resolved (360 for a longitude asked with 0) and is 0 for
// any other component.
struct Function {
    const prometheia_segments* const* a = nullptr;
    int na = 0;
    const prometheia_segments* const* b = nullptr;
    int nb = 0;
    double ca = 1.0, cb = 0.0;
    int component = PROMETHEIA_SEGMENTS_LON;
    double target = 0.0, target_rate = 0.0, target_jd_tt = 0.0;
    double modulus = 0.0;
};

// Writes the roots in [from, to), in time order, at most capacity of them
// (capacity 0: none, only counted), and returns how many were written or
// counted. *resume is the instant of the first root not written, or NaN
// when every root was. [from, to] lies inside both terms' spans.
int find(const Function& f, double from, double to, double tol_days, prometheia_segments_root* out,
         int capacity, double* resume) noexcept;

} // namespace prometheia::segment_roots

#endif

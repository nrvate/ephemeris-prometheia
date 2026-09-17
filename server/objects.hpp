// SPDX-License-Identifier: GPL-2.0-or-later
//
// What one object of a request resolves to, and the two ways the server asks
// the engine about it: one instant at a time, for the rows of a DATA answer,
// or as a sampler, for a segment fit.
//
// This is deliberately a step short of the protocol. A request's fields
// resolve to CalcOptions once (see the Plan in session.cpp); each object
// resolves to a ResolvedObject; everything after that is the engine's
// business. The segment cache needs the second half of that path without the
// first, which is why it lives here rather than inside the compute loop.
#ifndef PROMETHEIA_SERVER_OBJECTS_HPP
#define PROMETHEIA_SERVER_OBJECTS_HPP

#include <string>

#include "ephproto.h"
#include "prometheia/engine.hpp"
#include "prometheia/segments.hpp"
#include "wire_map.hpp"

namespace prometheia::server {

// An object spec with its wire numbering already resolved away.
struct ResolvedObject {
    enum class Kind { Body, Star, OrbitPoint };

    Kind kind = Kind::Body;
    int naif_id = 0;                              // Body and OrbitPoint: the SPK-ID
    size_t star_index = 0;                        // Star: its place in the catalog
    OrbitPoint point = OrbitPoint::AscendingNode; // OrbitPoint only
    OrbitElements elements = OrbitElements::Osculating;
    std::string name; // what the answer's metadata calls it
};

// Resolves one spec through the wire map and the star catalog. The error's
// message is what the object's metadata reports as its reason; no instant is
// named in it, so it is the same for every row.
Result<ResolvedObject> resolve_object(const eph::ObjSpec& spec, const WireMap& map);

// One instant. `jd_is_tt` picks the TT or UT entry point, which is the only
// thing the caller still has to remember.
Result<CalcResult> calc_at(Engine& engine, const ResolvedObject& obj, double jd, bool jd_is_tt,
                           const CalcOptions& opts);

// The same object as a sampler for segments::fit: rectangular coordinates in
// the output frame, TT throughout, rates always on. `opts` is copied, so the
// sampler outlives the caller's copy; `engine` is not, and must outlive it.
segments::Sampler sampler_for(Engine& engine, const ResolvedObject& obj, CalcOptions opts);

} // namespace prometheia::server

#endif // PROMETHEIA_SERVER_OBJECTS_HPP

// SPDX-License-Identifier: GPL-2.0-or-later
//
// What one object of a request resolves to, and the ways the server asks the
// engine about it: one instant at a time, for the rows of a DATA answer, or
// as a sampler, for a segment fit.
//
// This is deliberately a step short of the protocol. A request's profiles
// resolve to CalcOptions once (in session.cpp); each object resolves to a
// ResolvedObject; everything after that is the engine's business. The
// segment fitter needs the second half of that path without the first, which
// is why it lives here rather than inside the compute loop.
#ifndef PROMETHEIA_SERVER_OBJECTS_HPP
#define PROMETHEIA_SERVER_OBJECTS_HPP

#include <string>

#include "ephproto.h"
#include "prometheia/engine.hpp"
#include "prometheia/segments.hpp"

namespace prometheia::server {

// An object spec with its wire form resolved away. v4 names bodies by their
// NAIF/SPK-IDs, so there is no map: the ephemeris and catalogs answer the
// IDs directly.
struct ResolvedObject {
    enum class Kind { Body, Star, OrbitPoint, Hypothetical, Elements };

    Kind kind = Kind::Body;
    int naif_id = 0;                              // Body and OrbitPoint: the SPK-ID
    size_t star_index = 0;                        // Star: its place in the catalog
    OrbitPoint point = OrbitPoint::AscendingNode; // OrbitPoint only
    OrbitElements elements = OrbitElements::Osculating;
    std::string token;        // Hypothetical: the token, lowercase
    PolynomialElements poly;  // Elements: what the request sent
    std::string name;         // what the answer's metadata calls it
    bool is_sun = false;      // the object is the Sun itself (no deflection of its own light)
    bool no_parallax = false; // Star: no parallax in the catalog, so no distance
};

// Resolves one object spec against the engine's ephemeris and catalogs and
// the compiled-in star catalog. The error's message is what the object's
// metadata reports as its reason; no instant is named in it, so it is the
// same for every row.
Result<ResolvedObject> resolve_object(const eph::Object& spec, Engine& engine);

// One instant, in the request's time scale (0 UT1, 1 TT, 2 TDB; the server
// has already installed the request's delta T model on the engine).
Result<CalcResult> calc_at(Engine& engine, const ResolvedObject& obj, double jd, int time_scale,
                           const CalcOptions& opts);

// The same object as a sampler for segments::fit: rectangular coordinates in
// the output frame, TT throughout, rates always on. `opts` is copied, so the
// sampler outlives the caller's copy; `engine` is not, and must outlive it.
segments::Sampler sampler_for(Engine& engine, const ResolvedObject& obj, CalcOptions opts);

} // namespace prometheia::server

#endif // PROMETHEIA_SERVER_OBJECTS_HPP

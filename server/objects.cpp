// SPDX-License-Identifier: GPL-2.0-or-later
#include "objects.hpp"

#include <optional>

#include "prometheia/stars.hpp"

namespace prometheia::server {

Result<ResolvedObject> resolve_object(const eph::ObjSpec& spec, const WireMap& map) {
    ResolvedObject out;
    if (spec.kind == eph::kObjStar) {
        // Any name or designation the catalog knows (docs/STARS.md).
        auto found = stars::find(spec.name);
        if (!found) {
            return found.error();
        }
        out.kind = ResolvedObject::Kind::Star;
        out.star_index = found.value();
        out.name = stars::at(found.value()).name();
        return out;
    }
    const std::optional<WireBody> body = map.body(spec.id);
    if (!body) {
        return make_error(ErrorCode::NotFound,
                          "body " + std::to_string(spec.id) + " has no wire-map entry");
    }
    out.naif_id = body->naif_id;
    out.name = body->name.empty() ? "SPK-ID " + std::to_string(body->naif_id) : body->name;
    if (spec.kind == eph::kObjNodAps) {
        // parseRequest has checked point 1-4 and method 0-1.
        static constexpr OrbitPoint kPoints[] = {OrbitPoint::AscendingNode,
                                                 OrbitPoint::DescendingNode, OrbitPoint::Perihelion,
                                                 OrbitPoint::Aphelion};
        static constexpr const char* kSuffix[] = {" asc. node", " desc. node", " perihelion",
                                                  " aphelion"};
        out.kind = ResolvedObject::Kind::OrbitPoint;
        out.point = kPoints[spec.point - eph::kPntNorthNode];
        out.elements =
            spec.method == eph::kNodOscu ? OrbitElements::Osculating : OrbitElements::Mean;
        out.name += kSuffix[spec.point - eph::kPntNorthNode];
    }
    return out;
}

Result<CalcResult> calc_at(Engine& engine, const ResolvedObject& obj, double jd, bool jd_is_tt,
                           const CalcOptions& opts) {
    switch (obj.kind) {
    case ResolvedObject::Kind::Star:
        return jd_is_tt ? engine.calc_star(obj.star_index, jd, opts)
                        : engine.calc_star_ut(obj.star_index, jd, opts);
    case ResolvedObject::Kind::OrbitPoint:
        return jd_is_tt
                   ? engine.calc_orbit_point(obj.naif_id, obj.point, obj.elements, jd, opts)
                   : engine.calc_orbit_point_ut(obj.naif_id, obj.point, obj.elements, jd, opts);
    case ResolvedObject::Kind::Body:
        break;
    }
    return jd_is_tt ? engine.calc(obj.naif_id, jd, opts) : engine.calc_ut(obj.naif_id, jd, opts);
}

segments::Sampler sampler_for(Engine& engine, const ResolvedObject& obj, CalcOptions opts) {
    opts.speed = true;  // the fit is of position and rate together
    opts.sigma = false; // twelve extra integrations a sample, for nothing
    return
        [&engine, obj, opts](double jd_tt, double pos_au[3], double vel_au_day[3]) -> Result<void> {
            auto res = calc_at(engine, obj, jd_tt, true, opts);
            if (!res) {
                return res.error();
            }
            const Position& p = res.value().pos;
            for (int i = 0; i < 3; ++i) {
                pos_au[i] = p.xyz_au[i];
                vel_au_day[i] = p.vel_au_day[i];
            }
            return {};
        };
}

} // namespace prometheia::server

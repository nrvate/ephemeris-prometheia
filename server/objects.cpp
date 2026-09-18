// SPDX-License-Identifier: GPL-2.0-or-later
#include "objects.hpp"

#include "prometheia/hypotheticals.hpp"
#include "prometheia/stars.hpp"

namespace prometheia::server {
namespace {

// Display names for the ephemeris bodies, which the catalogs do not carry.
// The NAIF ids of the planets and their system barycentres are the JPL
// standard (docs/ENGINE.md); there are eleven.
std::string planet_name(int naif) {
    switch (naif) {
    case 0:
        return "solar-system barycentre";
    case 10:
        return "Sun";
    case 199:
        return "Mercury";
    case 299:
        return "Venus";
    case 399:
        return "Earth";
    case 301:
        return "Moon";
    case 4:
        return "Mars";
    case 5:
        return "Jupiter";
    case 6:
        return "Saturn";
    case 7:
        return "Uranus";
    case 8:
        return "Neptune";
    case 9:
        return "Pluto";
    default:
        return {};
    }
}

constexpr const char* kPointSuffix[] = {"asc. node", "desc. node", "perihelion", "aphelion"};

} // namespace

Result<ResolvedObject> resolve_object(const eph::Object& spec, Engine& engine) {
    ResolvedObject out;
    switch (spec.kind) {
    case eph::kObjBody:
    case eph::kObjDesignation: {
        if (spec.kind == eph::kObjDesignation) {
            // Kind 5 resolves exactly, as a LOOKUP of quality 0 or 1 (3.5);
            // Engine::lookup's index is exact, so there is no ambiguity.
            auto found = engine.lookup(spec.name);
            if (!found) {
                return found.error();
            }
            out.naif_id = found.value();
        } else {
            out.naif_id = spec.naif;
        }
        if (out.naif_id < 0) {
            return make_error(ErrorCode::ArgumentError,
                              "NAIF id " + std::to_string(out.naif_id) + " is not a body id");
        }
        out.kind = ResolvedObject::Kind::Body;
        out.is_sun = out.naif_id == 10;
        if (out.naif_id == 0) {
            return make_error(ErrorCode::ArgumentError,
                              "the barycentre is not a body with a position");
        }
        auto body_names = engine.names(out.naif_id);
        if (body_names.ok()) {
            out.name = body_names.value().name.empty() ? body_names.value().designation
                                                       : body_names.value().name;
        } else {
            out.name = planet_name(out.naif_id);
            if (out.name.empty()) {
                out.name = "SPK-ID " + std::to_string(out.naif_id);
            }
        }
        return out;
    }
    case eph::kObjStar: {
        auto found = stars::find(spec.name);
        if (!found) {
            return found.error();
        }
        out.kind = ResolvedObject::Kind::Star;
        out.star_index = found.value();
        out.name = stars::at(found.value()).name();
        out.no_parallax = stars::at(found.value()).parallax_mas <= 0.0f;
        return out;
    }
    case eph::kObjOrbitPoint: {
        // parseRequest has bounded point and method to their registries; the
        // engine serves points 0-3 by methods 0 (mean) and 1 (osculating).
        if (spec.method > 1) {
            return make_error(ErrorCode::ArgumentError, "orbit method " +
                                                            std::to_string(spec.method) +
                                                            " is not served by this engine");
        }
        static constexpr OrbitPoint kPoints[] = {OrbitPoint::AscendingNode,
                                                 OrbitPoint::DescendingNode, OrbitPoint::Perihelion,
                                                 OrbitPoint::Aphelion};
        // A body the ephemeris knows gets its display name; a catalog body
        // its own. The engine rejects what has no orbit.
        eph::Object body_spec{};
        body_spec.kind = eph::kObjBody;
        body_spec.naif = spec.naif;
        auto body = resolve_object(body_spec, engine);
        if (!body) {
            return body.error();
        }
        out.kind = ResolvedObject::Kind::OrbitPoint;
        out.naif_id = spec.naif;
        out.point = kPoints[spec.point];
        out.elements = spec.method == 0 ? OrbitElements::Mean : OrbitElements::Osculating;
        out.name =
            body.value().name + (spec.method == 0 ? " mean " : " ") + kPointSuffix[spec.point];
        return out;
    }
    case eph::kObjHypothetical: {
        // The elements are server-defined (A.15); the answer's source string
        // names the set, which the engine sets from the definition.
        const hypotheticals::Body* b = engine.hypothetical(spec.name);
        if (!b) {
            return make_error(ErrorCode::NotFound,
                              "hypothetical body \"" + spec.name + "\" is not served");
        }
        out.kind = ResolvedObject::Kind::Hypothetical;
        out.token = b->token;
        out.name = b->name.empty() ? b->token : b->name;
        return out;
    }
    case eph::kObjElements: {
        // 3.5a: the six elements' coefficients arrive element by element,
        // M's first, each nTerms long; the epoch and an explicit equinox are
        // TT (A.16). parseRequest has bounded nTerms, equinox and centre.
        static constexpr ElementEquinox kEquinoxes[] = {
            ElementEquinox::J2000, ElementEquinox::B1950, ElementEquinox::J1900,
            ElementEquinox::OfDate, ElementEquinox::Explicit};
        PolynomialElements& el = out.poly;
        el.epoch_jd_tt = spec.epoch.jd1 + spec.epoch.jd2;
        el.equinox = kEquinoxes[spec.equinox];
        el.equinox_jd_tt = spec.equinoxJd;
        el.centre = spec.centre == 1 ? ElementCentre::Earth : ElementCentre::Sun;
        el.n_terms = spec.nTerms;
        double* const rows[6] = {el.mean_anomaly,   el.semi_major_axis, el.eccentricity,
                                 el.arg_perihelion, el.ascending_node,  el.inclination};
        for (int k = 0; k < 6; ++k) {
            for (int j = 0; j < spec.nTerms; ++j) {
                rows[k][j] = spec.coef[size_t(k) * spec.nTerms + size_t(j)];
            }
        }
        out.kind = ResolvedObject::Kind::Elements;
        out.name = spec.name.empty() ? std::string("elements") : spec.name;
        return out;
    }
    default:
        return make_error(ErrorCode::ArgumentError,
                          "object kind " + std::to_string(spec.kind) + " is not served");
    }
}

Result<CalcResult> calc_at(Engine& engine, const ResolvedObject& obj, double jd, int time_scale,
                           const CalcOptions& opts) {
    // Row instants arrive in the request's time scale; the engine's entry
    // points are TT and UT1 (converted with the delta T model the server
    // has installed on it), and TDB converts to TT at a few nanoseconds'
    // loss against the Fairhead-Bretagnon series' own ~10 us (docs/TIME.md).
    if (time_scale == eph::kTimeTDB) {
        jd = prometheia::time::tt_from_tdb(jd);
        time_scale = eph::kTimeTT;
    }
    const bool ut1 = time_scale == eph::kTimeUT1;
    switch (obj.kind) {
    case ResolvedObject::Kind::Star:
        return ut1 ? engine.calc_star_ut(obj.star_index, jd, opts)
                   : engine.calc_star(obj.star_index, jd, opts);
    case ResolvedObject::Kind::OrbitPoint:
        return ut1 ? engine.calc_orbit_point_ut(obj.naif_id, obj.point, obj.elements, jd, opts)
                   : engine.calc_orbit_point(obj.naif_id, obj.point, obj.elements, jd, opts);
    case ResolvedObject::Kind::Hypothetical:
        return ut1 ? engine.calc_hypothetical_ut(obj.token, jd, opts)
                   : engine.calc_hypothetical(obj.token, jd, opts);
    case ResolvedObject::Kind::Elements:
        return ut1 ? engine.calc_elements_ut(obj.poly, jd, opts)
                   : engine.calc_elements(obj.poly, jd, opts);
    case ResolvedObject::Kind::Body:
        break;
    }
    return ut1 ? engine.calc_ut(obj.naif_id, jd, opts) : engine.calc(obj.naif_id, jd, opts);
}

segments::Sampler sampler_for(Engine& engine, const ResolvedObject& obj, CalcOptions opts) {
    opts.speed = true;  // the fit is of position and rate together
    opts.sigma = false; // twelve extra integrations a sample, for nothing
    return
        [&engine, obj, opts](double jd_tt, double pos_au[3], double vel_au_day[3]) -> Result<void> {
            auto res = calc_at(engine, obj, jd_tt, eph::kTimeTT, opts);
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

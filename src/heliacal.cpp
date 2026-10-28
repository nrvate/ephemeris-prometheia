// SPDX-License-Identifier: GPL-2.0-or-later
//
// Heliacal visibility (include/prometheia/heliacal.hpp, docs/HELIACAL.md).
// "Eq. N" is the equation of Schaefer (1993), Vistas in Astronomy 36, 311.

#include "prometheia/heliacal.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <string>

namespace prometheia::heliacal {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRad = kPi / 180.0;
constexpr double kEarthRadiusKm = 6378.0; // R_o of eq. 3b
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

double dex(double x) {
    return std::pow(10.0, x);
}

Error arg(const std::string& m) {
    return make_error(ErrorCode::ArgumentError, m);
}

} // namespace

// ---- air masses, extinction ------------------------------------------------

double air_mass(double z_deg) {
    const double c = std::cos(std::min(std::max(z_deg, 0.0), 90.0) * kRad);
    return 1.0 / (c + 0.025 * std::exp(-11.0 * c)); // eq. 2b (Rozenberg)
}

double air_mass_layer(double z_deg, double h_km) {
    const double c = std::cos(std::min(std::max(z_deg, 0.0), 90.0) * kRad);
    const double s = std::sqrt(h_km);
    return 1.0 / (c + 0.01 * s * std::exp(-30.0 * c / s)); // eq. 3a
}

double air_mass_shell(double z_deg, double h_km) {
    const double s =
        std::sin(std::min(std::max(z_deg, 0.0), 90.0) * kRad) / (1.0 + h_km / kEarthRadiusKm);
    return 1.0 / std::sqrt(1.0 - s * s); // eq. 3b
}

double illuminance_from_magnitude(double v) {
    return dex(-0.4 * (v + 16.57)); // eq. 10 inverted
}

double magnitude_from_illuminance(double fc) {
    return -16.57 - 2.5 * std::log10(fc); // eq. 10
}

Extinction extinction(double height_m, double lat_deg, double sun_ra_deg, const Atmosphere& atm,
                      bool night) {
    const double h_km = height_m / 1000.0;
    const double phi = lat_deg * kRad, alpha = sun_ra_deg * kRad;
    Extinction k;
    const double kr_day = 0.1066 * std::exp(-h_km / 8.2);                          // eq. 4b
    const double d_oz = 3.0 + 0.4 * (phi * std::cos(alpha) - std::cos(3.0 * phi)); // eq. 5a
    const double ko_day = 0.031 * d_oz / 3.0;                                      // eq. 5b
    // The night-vision (0.51 um) factors are the paper's own: Rayleigh 1.35
    // (eq. 4a), ozone 0.3 (after eq. 5b), aerosol 1.10 (eq. 6a's wavelength term).
    k.rayleigh = kr_day * (night ? 1.35 : 1.0);
    k.ozone = ko_day * (night ? 0.3 : 1.0);
    double ka_day;
    if (std::isfinite(atm.extinction_coefficient)) {
        ka_day = std::max(0.0, atm.extinction_coefficient - kr_day - ko_day);
    } else if (std::isfinite(atm.visual_range_km)) {
        // Koschmieder: beta = 3.912 / VR per km; the Rayleigh part (the
        // sea-level k_R over 1.086 H_R) is taken off and the rest, spread
        // over the 1.5 km aerosol layer, is the zenith aerosol extinction.
        // Not in Schaefer's paper.
        const double beta = 3.912 / atm.visual_range_km;
        const double beta_r = kr_day / (1.086 * 8.2);
        ka_day = std::max(0.0, 1.086 * 1.5 * (beta - beta_r));
    } else {
        const double s = std::min(std::max(atm.relative_humidity_pct, 5.0), 100.0);
        // eq. 6a at 0.55 um; the humidity is held within 5..100 % (the
        // bracket leaves its range below 1.4 %).
        ka_day = 0.12 * std::exp(-h_km / 1.5) * std::pow(1.0 - 0.32 / std::log(s), 4.0 / 3.0) *
                 (1.0 + 0.33 * std::sin(alpha));
    }
    k.aerosol = ka_day * (night ? 1.10 : 1.0);
    k.total = k.rayleigh + k.ozone + k.aerosol;
    return k;
}

// ---- limiting magnitude ----------------------------------------------------

double limiting_magnitude(double b_nl, const Observer& obs) {
    const double b6 = std::pow(b_nl * 1e-6, 0.4);
    const double y = obs.age_years;
    // The observer's pupil (eq. 32, no glare) and the standard observer's (eq. 33), cm.
    const double p = std::max(0.2, 0.7 * std::exp(-0.5 * (y / 100.0) * (y / 100.0) - 0.16 * b6));
    const double pst = std::max(0.2, 0.68 * std::exp(-0.16 * b6));
    // Naked eye: the aperture D is the pupil, no magnification (M = 1), no
    // telescope (F_t = F_p = F_m = 1, F_c = 1: no colour index is used).
    const double fa = (pst / p) * (pst / p); // eq. 31e
    double fsc;                              // eq. 31i, 31l
    if (std::log10(b_nl) > 3.17) {
        fsc = std::min(1.0, (p / pst) * (p / pst) * (1.0 - std::exp(-(pst / 0.62) * (pst / 0.62))) /
                                (1.0 - std::exp(-(p / 0.62) * (p / 0.62))));
    } else {
        fsc = std::min(1.0, (1.0 - std::pow(pst / 12.44, 4)) / (1.0 - std::pow(p / 12.44, 4)));
    }
    const double fb = obs.binocular ? 1.0 : std::sqrt(2.0);          // eq. 31a,b
    const double fr = 1.0 / (obs.snellen_ratio * obs.snellen_ratio); // eq. 31f, point source
    const double b_eff = b_nl * fb * fa * fsc;                       // eqs. 29b, 30b
    double c, k;                                                     // eq. 34
    if (std::log10(b_eff) > 3.17) {
        c = dex(-8.35);
        k = dex(-5.90);
    } else {
        c = dex(-9.80);
        k = dex(-1.90);
    }
    const double i_th = c * (1.0 + std::sqrt(k * b_eff)) * (1.0 + std::sqrt(k * b_eff));
    return magnitude_from_illuminance(i_th * fb * fa * fsc * fr); // eqs. 29a, 30a, 10
}

namespace {

// The scattering function of eq. 14, rho in degrees.
double scatter(double rho) {
    rho = std::max(rho, 0.1);
    const double c = std::cos(rho * kRad);
    return dex(5.36) * (1.06 + c * c) + dex(6.15 - rho / 40.0) + 6.2e7 / (rho * rho);
}

// B_sky of eq. 15e for one extinction coefficient, nanoLamberts.
double sky_brightness(const Scene& s, double k, double b0) {
    const double z = 90.0 - s.object_alt_deg;
    const double x = air_mass(z);
    const double haze = 1.0 - dex(-0.4 * k * x);
    // 15a. Near the horizon the sky never goes black: the paper takes the
    // brightness of 10 deg higher (its section 3.17), z = 80 deg at most.
    const double zn = std::min(z, 80.0);
    const double sn = std::sin(zn * kRad);
    const double b_night =
        b0 * (0.4 + 0.6 / std::sqrt(1.0 - 0.96 * sn * sn)) * dex(-0.4 * k * air_mass(zn));
    // 15b: only with the Moon above the horizon.
    double b_moon = 0.0;
    if (std::isfinite(s.moon_v) && s.moon_alt_deg > 0.0) {
        const double zm = 90.0 - s.moon_alt_deg;
        b_moon = scatter(s.moon_sep_deg) * dex(-0.4 * (s.moon_v + 16.57 + k * air_mass(zm))) * haze;
    }
    // 15c and 15d. Eqs. 2b and 3a are not defined for a zenith distance past
    // 90 deg, and B_day with the horizon's air mass (40) is a hundred times
    // fainter than Table 6's sunset sky, so with the Sun on or below the
    // horizon the twilight term stands alone (docs/HELIACAL.md); above it the
    // paper's min applies.
    double b_sun =
        std::max(1.0, dex(s.sun_sep_deg / 90.0 - 1.1)) * dex(8.45 + 0.4 * s.sun_alt_deg) * haze;
    if (s.sun_alt_deg > 0.0) {
        const double zs = 90.0 - s.sun_alt_deg;
        const double b_day = 11700.0 * scatter(s.sun_sep_deg) * dex(-0.4 * k * air_mass(zs)) * haze;
        b_sun = std::min(b_sun, b_day);
    }
    return b_night + b_moon + b_sun; // 15e, no city light
}

} // namespace

Visibility visibility(const Scene& s, const Atmosphere& atm, const Observer& obs, double b0) {
    Visibility v;
    // Day-vision coefficients first; a sky fainter than 1500 nL (the paper's
    // day/night vision boundary, Table 6) is seen with the night-vision ones.
    Extinction k = extinction(s.site_height_m, s.site_lat_deg, s.sun_ra_deg, atm, false);
    double b = sky_brightness(s, k.total, b0);
    if (b < 1500.0) {
        k = extinction(s.site_height_m, s.site_lat_deg, s.sun_ra_deg, atm, true);
        b = sky_brightness(s, k.total, b0);
        v.night_vision = true;
    }
    v.k = k;
    v.sky_brightness_nl = b;
    const double z = 90.0 - s.object_alt_deg;
    v.air_mass_object = air_mass(z);
    v.extinction_mag = k.rayleigh * air_mass_layer(z, 8.2) + k.ozone * air_mass_shell(z, 20.0) +
                       k.aerosol * air_mass_layer(z, 1.5); // eq. 8b
    v.limit_at_eye_mag = limiting_magnitude(b, obs);
    v.limit_outside_mag = v.limit_at_eye_mag - v.extinction_mag;
    v.object_at_eye_mag = s.object_v + v.extinction_mag;
    v.margin_mag = s.object_alt_deg < 0.0 ? -kInf : v.limit_at_eye_mag - v.object_at_eye_mag;
    return v;
}

// ---- the engine's view of the sky -------------------------------------------

namespace {

Engine::ObjectRef body_ref(long naif) {
    Engine::ObjectRef r;
    r.id = naif;
    return r;
}

bool is_moon(const Engine::ObjectRef& o) {
    return o.kind == Engine::ObjectRef::Kind::Body && o.id == 301;
}

struct Vec {
    double x, y, z;
};
Vec unit(double ra_deg, double dec_deg) {
    const double a = ra_deg * kRad, d = dec_deg * kRad;
    return {std::cos(d) * std::cos(a), std::cos(d) * std::sin(a), std::sin(d)};
}
double separation_deg(const Vec& a, const Vec& b) {
    const double cx = a.y * b.z - a.z * b.y, cy = a.z * b.x - a.x * b.z, cz = a.x * b.y - a.y * b.x;
    return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), a.x * b.x + a.y * b.y + a.z * b.z) /
           kRad;
}

// What a day's search holds constant: the magnitudes.
struct Magnitudes {
    double v = kNaN, moon_v = kNaN;
};

struct Ctx {
    Engine& engine;
    const Engine::ObjectRef& object;
    const frames::GeoSite& site;
    const Atmosphere& atm;
    const Observer& observer;
    const Options& options;
    CalcOptions calc;
    frames::Atmosphere frame_atm;
    Error error; // the first engine failure
    Ctx(Engine& e, const Engine::ObjectRef& o, const frames::GeoSite& s, const Atmosphere& a,
        const Observer& ob, const Options& op)
        : engine(e), object(o), site(s), atm(a), observer(ob), options(op) {
        calc.center = Center::Topocentric;
        calc.site = s;
        calc.coords = Coords::Equatorial;
        calc.frame = Frame::TrueOfDate;
        calc.speed = false;
        calc.sigma = false;
        frame_atm.pressure_hpa = a.pressure_hpa;
        frame_atm.temperature_c = a.temperature_c;
    }
    double dt_days(double jd_tt) const { return engine.delta_t_seconds(jd_tt) / 86400.0; }
};

Result<Magnitudes> magnitudes(Ctx& c, double jd_tt) {
    Magnitudes m;
    auto p = c.engine.phenomena(c.object, jd_tt, c.calc);
    if (!p.ok())
        return p.error();
    m.v = p.value().magnitude;
    if (!is_moon(c.object) && c.options.model == Model::Schaefer && c.options.moonlight) {
        auto q = c.engine.phenomena(body_ref(301), jd_tt, c.calc);
        if (!q.ok())
            return q.error();
        m.moon_v = q.value().magnitude;
    }
    return m;
}

Result<Sight> sight(Ctx& c, double jd_tt, const Magnitudes& mags) {
    Sight out;
    out.jd_tt = jd_tt;
    const double dt = c.dt_days(jd_tt);
    out.jd_ut1 = jd_tt - dt;
    auto sun = c.engine.calc_object(body_ref(10), jd_tt, c.calc);
    if (!sun.ok())
        return sun.error();
    auto obj = c.engine.calc_object(c.object, jd_tt, c.calc);
    if (!obj.ok())
        return obj.error();
    auto gast = c.engine.sidereal_time_ut(out.jd_ut1, true);
    if (!gast.ok())
        return gast.error();
    const Position& ps = sun.value().pos;
    const Position& po = obj.value().pos;
    auto alt_az = [&](const Position& p, double& az, double& alt) {
        const double ha = (gast.value() + c.site.lon_rad / kRad - p.lon_deg) * kRad;
        frames::equatorial_to_horizontal(ha, p.lat_deg * kRad, c.site.lat_rad, az, alt);
        az /= kRad;
        alt /= kRad;
    };
    double az = 0, alt = 0, saz = 0, salt = 0;
    alt_az(po, az, alt);
    alt_az(ps, saz, salt);
    Scene& s = out.scene;
    s.object_alt_deg = alt + frames::refraction_from_true_deg(alt, c.frame_atm, 0.0); // apparent
    s.object_v = mags.v;
    s.sun_alt_deg = salt;
    s.sun_ra_deg = ps.lon_deg;
    s.sun_sep_deg = separation_deg(unit(po.lon_deg, po.lat_deg), unit(ps.lon_deg, ps.lat_deg));
    s.site_height_m = c.site.height_m;
    s.site_lat_deg = c.site.lat_rad / kRad;
    if (std::isfinite(mags.moon_v)) {
        auto moon = c.engine.calc_object(body_ref(301), jd_tt, c.calc);
        if (!moon.ok())
            return moon.error();
        const Position& pm = moon.value().pos;
        double maz = 0, malt = 0;
        alt_az(pm, maz, malt);
        s.moon_alt_deg = malt;
        s.moon_v = mags.moon_v;
        s.moon_sep_deg = separation_deg(unit(po.lon_deg, po.lat_deg), unit(pm.lon_deg, pm.lat_deg));
    }
    out.object_az_deg = az;
    out.sun_depression_deg = -salt;
    if (c.options.model == Model::Schaefer) {
        out.seen = visibility(s, c.atm, c.observer, c.options.dark_sky_nl);
        // Below the horizon the margin is a finite slope toward the horizon,
        // so that the optimum can be searched for.
        out.margin =
            std::isfinite(mags.v)
                ? (s.object_alt_deg < 0.0 ? -5.0 + s.object_alt_deg / 10.0
                                          : out.seen.margin_mag - c.options.detection_margin_mag)
                : -10.0;
    } else {
        out.margin = std::min(out.sun_depression_deg - c.options.arcus_visionis_deg,
                              s.object_alt_deg - c.options.object_altitude_deg);
    }
    return out;
}

// The margin as a plain number; an engine failure is kept in c.error.
double margin_at(Ctx& c, double jd_tt, const Magnitudes& m) {
    auto r = sight(c, jd_tt, m);
    if (!r.ok()) {
        if (c.error.code == ErrorCode::Ok)
            c.error = r.error();
        return -1e9;
    }
    return r.value().margin;
}

struct Day {
    bool vis = false;
    double opt_tt = 0.0, margin = 0.0;
    double w0 = 0.0, w1 = 0.0; // the visibility window, TT
    Magnitudes mags;
};

constexpr double kWindowDays = 3.5 / 24.0; // the twilight searched, from sunrise or sunset

bool morning_of(Event e) {
    return e == Event::Rising || e == Event::MorningLast;
}

Day eval_day(Ctx& c, Event event, double jd_start_tt, int j) {
    Day d;
    const bool morning = morning_of(event);
    const double anchor_tt = jd_start_tt + j;
    Engine::RiseSetOptions rs;
    rs.limb = 0;
    rs.atmosphere = c.frame_atm;
    rs.window_days = 2.0;
    auto ev = c.engine.rise_set_ut(body_ref(10), morning ? Engine::Event::Rise : Engine::Event::Set,
                                   anchor_tt - c.dt_days(anchor_tt), c.site, rs);
    if (!ev.ok()) {
        c.error = ev.error();
        return d;
    }
    if (ev.value().outcome != Engine::Outcome::Found)
        return d;
    const double ut = ev.value().jd_ut1;
    const double tt = ut + c.dt_days(ut + c.dt_days(anchor_tt));
    const double t0 = morning ? tt - kWindowDays : tt;
    const double t1 = morning ? tt : tt + kWindowDays;
    auto m = magnitudes(c, (t0 + t1) / 2);
    if (!m.ok()) {
        c.error = m.error();
        return d;
    }
    d.mags = m.value();
    if (c.options.model == Model::Schaefer && !std::isfinite(d.mags.v))
        return d;
    constexpr int kSamples = 16;
    double best = -kInf;
    int kb = 0;
    for (int k = 0; k < kSamples; ++k) {
        const double g = margin_at(c, t0 + (t1 - t0) * k / (kSamples - 1), d.mags);
        if (g > best) {
            best = g;
            kb = k;
        }
    }
    const double floor_ = c.options.model == Model::Schaefer ? -1.5 : -5.0;
    d.opt_tt = t0 + (t1 - t0) * kb / (kSamples - 1);
    d.margin = best;
    if (best > floor_) {
        // Golden-section search for the maximum between the neighbours.
        double a = t0 + (t1 - t0) * std::max(kb - 1, 0) / (kSamples - 1);
        double b = t0 + (t1 - t0) * std::min(kb + 1, kSamples - 1) / (kSamples - 1);
        const double gr = 0.6180339887498949;
        double x1 = b - gr * (b - a), x2 = a + gr * (b - a);
        double f1 = margin_at(c, x1, d.mags), f2 = margin_at(c, x2, d.mags);
        for (int i = 0; i < 18; ++i) {
            if (f1 < f2) {
                a = x1;
                x1 = x2;
                f1 = f2;
                x2 = a + gr * (b - a);
                f2 = margin_at(c, x2, d.mags);
            } else {
                b = x2;
                x2 = x1;
                f2 = f1;
                x1 = b - gr * (b - a);
                f1 = margin_at(c, x1, d.mags);
            }
        }
        const double xm = f1 > f2 ? x1 : x2;
        const double fm = std::max(f1, f2);
        if (fm > d.margin) {
            d.opt_tt = xm;
            d.margin = fm;
        }
    }
    d.vis = d.margin > 0.0;
    if (d.vis) {
        // The window: out from the optimum a half minute at a time while the
        // margin holds (and the twilight searched does), then bisected to a second.
        const double step = 30.0 / 86400.0;
        auto edge = [&](double dir) {
            double good = d.opt_tt, t = d.opt_tt;
            for (;;) {
                t += dir * step;
                if (t < t0 || t > t1) {
                    t = std::min(std::max(t, t0), t1);
                    if (margin_at(c, t, d.mags) > 0.0)
                        return t;
                    break;
                }
                if (margin_at(c, t, d.mags) <= 0.0)
                    break;
                good = t;
            }
            double bad = t;
            for (int i = 0; i < 6; ++i) {
                const double mid = (good + bad) / 2;
                (margin_at(c, mid, d.mags) > 0.0 ? good : bad) = mid;
            }
            return good;
        };
        d.w0 = edge(-1.0);
        d.w1 = edge(+1.0);
    }
    return d;
}

} // namespace

// ---- public engine calls ----------------------------------------------------

namespace {

Error check_inputs(const Engine::ObjectRef& obj, const Atmosphere& atm, const Observer& ob,
                   const Options& op) {
    if (std::isfinite(atm.extinction_coefficient) && std::isfinite(atm.visual_range_km))
        return arg("give the extinction coefficient or the visual range, not both");
    if (!(atm.pressure_hpa > 0.0) || !std::isfinite(atm.temperature_c) ||
        !(atm.relative_humidity_pct >= 0.0 && atm.relative_humidity_pct <= 100.0))
        return arg("atmosphere out of range");
    if (std::isfinite(atm.extinction_coefficient) && !(atm.extinction_coefficient > 0.0))
        return arg("extinction coefficient must be positive");
    if (std::isfinite(atm.visual_range_km) && !(atm.visual_range_km > 0.0))
        return arg("visual range must be positive");
    if (!(ob.age_years > 0.0 && ob.age_years < 120.0) || !(ob.snellen_ratio > 0.0))
        return arg("observer out of range");
    if (obj.kind == Engine::ObjectRef::Kind::Body && (obj.id == 10 || obj.id == 399))
        return arg("the Sun and the Earth have no heliacal events");
    if (op.model == Model::ArcusVisionis) {
        if (!std::isfinite(op.arcus_visionis_deg))
            return arg("arcus visionis needs the arc (arcus_visionis_deg)");
        if (!std::isfinite(op.object_altitude_deg))
            return arg("object altitude is not finite");
    } else {
        if (obj.kind == Engine::ObjectRef::Kind::Hypothetical ||
            obj.kind == Engine::ObjectRef::Kind::OrbitPoint)
            return arg("the object has no magnitude");
        if (!(op.dark_sky_nl > 0.0))
            return arg("dark sky brightness must be positive");
    }
    return Error{};
}

} // namespace

Result<Scene> scene_at(Engine& engine, const Engine::ObjectRef& object, double jd_tt,
                       const frames::GeoSite& site, const Atmosphere& atm) {
    Options o;
    Observer ob;
    Ctx c(engine, object, site, atm, ob, o);
    auto m = magnitudes(c, jd_tt);
    if (!m.ok())
        return m.error();
    auto s = sight(c, jd_tt, m.value());
    if (!s.ok())
        return s.error();
    return s.value().scene;
}

Result<Sight> sight_at(Engine& engine, const Engine::ObjectRef& object, double jd_tt,
                       const frames::GeoSite& site, const Atmosphere& atm, const Observer& observer,
                       const Options& options) {
    if (Error e = check_inputs(object, atm, observer, options); e.code != ErrorCode::Ok)
        return e;
    if (!std::isfinite(jd_tt))
        return arg("jd is not finite");
    Ctx c(engine, object, site, atm, observer, options);
    auto m = magnitudes(c, jd_tt);
    if (!m.ok())
        return m.error();
    return sight(c, jd_tt, m.value());
}

Result<Found> next(Engine& engine, const Engine::ObjectRef& object, double jd_start_tt,
                   const frames::GeoSite& site, const Atmosphere& atm, const Observer& observer,
                   Event event, const Options& options) {
    if (Error e = check_inputs(object, atm, observer, options); e.code != ErrorCode::Ok)
        return e;
    if (!std::isfinite(jd_start_tt))
        return arg("jd is not finite");
    if (event < Event::Rising || event > Event::MorningLast)
        return arg("unknown event");
    if (object.kind == Engine::ObjectRef::Kind::Star &&
        (event == Event::EveningFirst || event == Event::MorningLast))
        return arg("a star has only the heliacal rising and setting");
    Ctx c(engine, object, site, atm, observer, options);
    const bool first = event == Event::Rising || event == Event::EveningFirst;
    const int stride = is_moon(object) ? 1 : 4;
    const int max_day = int(std::ceil(options.search_days));
    std::map<int, Day> days;
    auto S = [&](int j) -> const Day& {
        auto it = days.find(j);
        if (it == days.end())
            it = days.emplace(j, eval_day(c, event, jd_start_tt, j)).first;
        return it->second;
    };
    auto finish = [&](int j) -> Result<Found> {
        const Day& d = S(j);
        Found f;
        f.event = event;
        f.model = options.model;
        f.jd_tt = d.w0;
        f.jd_ut1 = d.w0 - c.dt_days(d.w0);
        f.optimum_tt = d.opt_tt;
        f.optimum_ut1 = d.opt_tt - c.dt_days(d.opt_tt);
        f.end_tt = d.w1;
        f.end_ut1 = d.w1 - c.dt_days(d.w1);
        auto s = sight(c, d.opt_tt, d.mags);
        if (!s.ok())
            return s.error();
        f.at_optimum = s.value();
        f.atmosphere = atm;
        f.observer = observer;
        f.options = options;
        return f;
    };
    bool prev = S(-1).vis;
    int pos = -1;
    while (pos < max_day) {
        if (c.error.code != ErrorCode::Ok)
            return c.error;
        const int nxt = pos + stride;
        const bool cur = S(nxt).vis;
        if (first && cur && !prev) {
            for (int d = pos + 1; d <= nxt; ++d) {
                if (S(d).vis && !S(d - 1).vis && d >= 0 && S(d).w0 > jd_start_tt)
                    return finish(d);
            }
        } else if (!first && prev && !cur) {
            for (int d = pos; d < nxt; ++d) {
                if (d >= 0 && S(d).vis && !S(d + 1).vis && S(d).w0 > jd_start_tt)
                    return finish(d);
            }
        }
        prev = cur;
        pos = nxt;
    }
    if (c.error.code != ErrorCode::Ok)
        return c.error;
    return make_error(ErrorCode::NotFound, "no heliacal event within the search window");
}

} // namespace prometheia::heliacal

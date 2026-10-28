// SPDX-License-Identifier: GPL-2.0-or-later
//
// Lunar eclipses; the method is in include/prometheia/lunar_eclipse.hpp and
// docs/ECLIPSES.md.

#include "prometheia/lunar_eclipse.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace prometheia::lunar_eclipse {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRad2Deg = 180.0 / kPi;
constexpr double kAuKm = 149597870.7;
// Mean new moon of lunation 0 (2000 Jan 6) and the mean synodic month; the
// lunation of a full moon is counted as L = floor((t - K0)/P), so that its
// mean time is K0 + (L + 1/2) P.
constexpr double kK0 = 2451550.09766;
constexpr double kSynodic = 29.530588861;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Series {
    int number;
    long first_lunation;
};
constexpr Series kSeries[] = {
#include "lunar_saros.inc"
};

long lunation_of(double jd_tt) {
    return long(std::floor((jd_tt - kK0) / kSynodic));
}

// What the shadow geometry needs of one instant.
struct Geo {
    double sep = 0.0;     // Moon's angular distance from the shadow axis, rad
    double north = 1.0;   // the sign of gamma
    double moon_km = 1.0; // geocentric distance
    double pen = 0.0;     // Rp, rad
    double umb = 0.0;     // Ru, rad
    double semi = 0.0;    // the Moon's semi-diameter, rad
};

class Solver {
public:
    explicit Solver(Engine& e) : engine_(e) {
        opts_.center = Center::Geocentric;
        opts_.frame = Frame::TrueOfDate;
        opts_.coords = Coords::Ecliptic;
        opts_.speed = false;
        opts_.sigma = false;
    }

    std::optional<Error> failure;

    // Moon minus Sun longitude less 180, in (-180, 180] degrees.
    double elongation(double jd) {
        CalcResult s, m;
        if (!bodies(jd, s, m))
            return 0.0;
        return std::remainder(m.pos.lon_deg - s.pos.lon_deg - 180.0, 360.0);
    }

    Geo geo(double jd) {
        Geo g;
        CalcResult s, m;
        if (!bodies(jd, s, m))
            return g;
        const double* a = m.pos.xyz_au;
        const double* b = s.pos.xyz_au;
        const double na = m.pos.dist_au, nb = s.pos.dist_au;
        // The axis points away from the Sun: -b. The angle by atan2 of the
        // cross and dot products, never an acos.
        double u[3], v[3];
        for (int i = 0; i < 3; ++i) {
            u[i] = a[i] / na;
            v[i] = -b[i] / nb;
        }
        const double cx = u[1] * v[2] - u[2] * v[1], cy = u[2] * v[0] - u[0] * v[2],
                     cz = u[0] * v[1] - u[1] * v[0];
        g.sep = std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz),
                           u[0] * v[0] + u[1] * v[1] + u[2] * v[2]);
        g.north = (u[2] + v[2] >= 0.0) ? 1.0 : -1.0;
        g.moon_km = na * kAuKm;
        const double sun_km = nb * kAuKm;
        const double pi_moon = std::asin(kEarthEquatorialRadiusKm / g.moon_km);
        const double s_sun = std::asin(kSunRadiusKm / sun_km);
        const double p_sun = std::asin(kEarthEquatorialRadiusKm / sun_km);
        g.pen = kDanjon * pi_moon + s_sun + p_sun;
        g.umb = kDanjon * pi_moon - s_sun + p_sun;
        g.semi = std::asin(kMoonRadiusKm / g.moon_km);
        return g;
    }

    // The time of least separation in [lo, hi]: a golden-section search to
    // 2e-3 day, then the vertex of a parabola through three squared
    // separations, three times over.
    double minimise(double lo, double hi) {
        auto f = [&](double t) {
            const double d = geo(t).sep;
            return d * d;
        };
        const double phi = 0.6180339887498949;
        double a = lo, b = hi;
        double c = b - phi * (b - a), d = a + phi * (b - a);
        double fc = f(c), fd = f(d);
        while (b - a > 2e-3) {
            if (fc < fd) {
                b = d, d = c, fd = fc;
                c = b - phi * (b - a);
                fc = f(c);
            } else {
                a = c, c = d, fc = fd;
                d = a + phi * (b - a);
                fd = f(d);
            }
        }
        double t = 0.5 * (a + b);
        const double h = 1e-3;
        for (int i = 0; i < 3; ++i) {
            const double fm = f(t - h), f0 = f(t), fp = f(t + h);
            const double den = fm - 2.0 * f0 + fp;
            if (!(den > 0.0))
                break;
            t += 0.5 * h * (fm - fp) / den;
        }
        return t;
    }

private:
    bool bodies(double jd, CalcResult& s, CalcResult& m) {
        if (failure)
            return false;
        auto rs = engine_.calc(body::kSun, jd, opts_);
        if (!rs) {
            failure = rs.error();
            return false;
        }
        auto rm = engine_.calc(body::kMoon, jd, opts_);
        if (!rm) {
            failure = rm.error();
            return false;
        }
        s = rs.value();
        m = rm.value();
        return true;
    }

    Engine& engine_;
    CalcOptions opts_;
};

// The root of f in [a, b] (f(a), f(b) of opposite signs) by the Illinois
// method, to 1e-8 day (~1 ms).
template <typename F>
double root(F&& f, double a, double b) {
    double fa = f(a), fb = f(b);
    int side = 0;
    for (int i = 0; i < 100 && std::fabs(b - a) > 1e-8; ++i) {
        const double c = (a * fb - b * fa) / (fb - fa);
        const double fc = f(c);
        if (fc == 0.0)
            return c;
        if (fc * fb > 0.0) {
            b = c, fb = fc;
            if (side == -1)
                fa *= 0.5;
            side = -1;
        } else {
            a = c, fa = fc;
            if (side == 1)
                fb *= 0.5;
            side = 1;
        }
    }
    return 0.5 * (a + b);
}

// The eclipse of lunation `ell`, if there is one.
Result<std::optional<Eclipse>> eclipse_of(Engine& engine, long ell) {
    Solver sv(engine);
    // The true opposition, by Newton from the mean full moon.
    double t = kK0 + (double(ell) + 0.5) * kSynodic;
    for (int i = 0; i < 4; ++i)
        t -= sv.elongation(t) / 12.19;
    const Geo at_opposition = sv.geo(t);
    if (sv.failure)
        return *sv.failure;
    // Nothing can touch the penumbra with the Moon this far from the axis
    // (the largest Rp + semi-diameter is under 2 deg, and the least
    // separation is within a quarter degree of this one).
    if (at_opposition.sep * kRad2Deg > 2.4)
        return std::optional<Eclipse>{};
    const double tm = sv.minimise(t - 0.25, t + 0.25);
    const Geo gm = sv.geo(tm);
    if (sv.failure)
        return *sv.failure;
    Eclipse e;
    const double two_semi = 2.0 * gm.semi;
    e.penumbral_magnitude = (gm.pen + gm.semi - gm.sep) / two_semi;
    e.umbral_magnitude = (gm.umb + gm.semi - gm.sep) / two_semi;
    if (!(e.penumbral_magnitude > 0.0))
        return std::optional<Eclipse>{};
    e.kind = e.umbral_magnitude >= 1.0
                 ? Kind::Total
                 : (e.umbral_magnitude > 0.0 ? Kind::Partial : Kind::Penumbral);
    e.jd_max_tt = tm;
    e.jd_max_ut1 = tm - engine.delta_t_seconds(tm) / 86400.0;
    e.gamma = gm.north * std::sin(gm.sep) * gm.moon_km / kEarthEquatorialRadiusKm;
    e.umbra_radius_deg = gm.umb * kRad2Deg;
    e.penumbra_radius_deg = gm.pen * kRad2Deg;
    e.moon_semidiameter_deg = gm.semi * kRad2Deg;
    const Saros sa = saros_of(tm);
    e.saros_series = sa.series;
    e.saros_member = sa.member;

    // The circle each contact is on, as a separation from the axis.
    auto circle = [](const Geo& g, int c) {
        switch (c) {
        case P1:
        case P4:
            return g.pen + g.semi;
        case U1:
        case U4:
            return g.umb + g.semi;
        default:
            return g.umb - g.semi;
        }
    };
    for (int c = 0; c < kContacts; ++c) {
        e.contact_tt[c] = e.contact_ut1[c] = kNaN;
        if ((c == U1 || c == U4) && !(e.umbral_magnitude > 0.0))
            continue;
        if ((c == U2 || c == U3) && !(e.umbral_magnitude >= 1.0))
            continue;
        const bool before = c == P1 || c == U1 || c == U2;
        auto f = [&](double x) {
            const Geo g = sv.geo(x);
            return g.sep - circle(g, c);
        };
        // Out from the greatest eclipse until the sign changes.
        double span = 0.1;
        double far = before ? tm - span : tm + span;
        while (!(f(far) > 0.0) && span < 0.6 && !sv.failure) {
            span *= 1.5;
            far = before ? tm - span : tm + span;
        }
        if (sv.failure)
            return *sv.failure;
        const double r = before ? root(f, far, tm) : root(f, tm, far);
        if (sv.failure)
            return *sv.failure;
        e.contact_tt[c] = r;
        e.contact_ut1[c] = r - engine.delta_t_seconds(r) / 86400.0;
    }
    return std::optional<Eclipse>{e};
}

} // namespace

Saros saros_of(double jd_tt) {
    const long ell = lunation_of(jd_tt);
    // L = 135 s + 208 (mod 223) for every eclipse in NASA's canon, whose
    // series numbers lie in [-20, 183]; s is taken in [-20, 202].
    constexpr long kInverse135 = 38; // 135 * 38 = 1 (mod 223)
    long s = (((ell - 208) % 223 + 223) % 223) * kInverse135 % 223;
    if (s > 202)
        s -= 223;
    Saros out;
    out.series = int(s);
    for (const Series& x : kSeries) {
        if (x.number == s) {
            const long since = ell - x.first_lunation;
            if (since >= 0 && since % 223 == 0)
                out.member = int(since / 223 + 1);
            break;
        }
    }
    return out;
}

Result<Eclipse> next(Engine& engine, double jd_start_tt, unsigned kinds, bool backward) {
    if (!std::isfinite(jd_start_tt))
        return make_error(ErrorCode::ArgumentError, "the start is not finite");
    if (kinds == 0 || (kinds & ~kAllKinds) != 0)
        return make_error(ErrorCode::ArgumentError,
                          "kinds is a nonzero mask of total, partial and penumbral");
    long ell = lunation_of(jd_start_tt) + (backward ? 1 : -1);
    for (int n = 0; n < 2000; ++n, ell += backward ? -1 : 1) {
        auto r = eclipse_of(engine, ell);
        if (!r.ok())
            return r.error();
        if (!r.value())
            continue;
        const Eclipse& e = *r.value();
        if (backward ? !(e.jd_max_tt < jd_start_tt) : !(e.jd_max_tt > jd_start_tt))
            continue;
        const unsigned bit = e.kind == Kind::Total     ? kTotal
                             : e.kind == Kind::Partial ? kPartial
                                                       : kPenumbral;
        if (kinds & bit)
            return e;
    }
    return make_error(ErrorCode::NotFound, "no lunar eclipse of those kinds within 160 years");
}

Result<Eclipse> at(Engine& engine, double jd_tt) {
    if (!std::isfinite(jd_tt))
        return make_error(ErrorCode::ArgumentError, "the time is not finite");
    const long ell = lunation_of(jd_tt);
    std::optional<Eclipse> best;
    for (long k = ell - 1; k <= ell + 1; ++k) {
        auto r = eclipse_of(engine, k);
        if (!r.ok())
            return r.error();
        if (r.value() && std::fabs(r.value()->jd_max_tt - jd_tt) <= 2.0 &&
            (!best || std::fabs(r.value()->jd_max_tt - jd_tt) < std::fabs(best->jd_max_tt - jd_tt)))
            best = r.value();
    }
    if (!best)
        return make_error(ErrorCode::NotFound, "no lunar eclipse within two days of that time");
    return *best;
}

Result<Local> local(Engine& engine, double jd_tt, const frames::GeoSite& site) {
    auto er = at(engine, jd_tt);
    if (!er.ok())
        return er.error();
    Local out;
    out.eclipse = er.value();
    CalcOptions o;
    o.center = Center::Topocentric;
    o.frame = Frame::TrueOfDate;
    o.coords = Coords::Equatorial;
    o.speed = false;
    o.sigma = false;
    o.site = site;
    const frames::Atmosphere standard;
    for (int c = 0; c <= kMaximum; ++c) {
        const double tt = c == kMaximum ? out.eclipse.jd_max_tt : out.eclipse.contact_tt[c];
        out.altitude_deg[c] = out.apparent_altitude_deg[c] = kNaN;
        if (!std::isfinite(tt))
            continue;
        auto p = engine.calc(body::kMoon, tt, o);
        if (!p.ok())
            return p.error();
        const double ut1 = tt - engine.delta_t_seconds(tt) / 86400.0;
        auto h = engine.horizontal_ut(ut1, site, p.value().pos.lon_deg, p.value().pos.lat_deg,
                                      &standard);
        if (!h.ok())
            return h.error();
        out.altitude_deg[c] = h.value().altitude_deg;
        out.apparent_altitude_deg[c] = h.value().apparent_altitude_deg;
    }
    return out;
}

} // namespace prometheia::lunar_eclipse

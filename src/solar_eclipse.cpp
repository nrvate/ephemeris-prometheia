// SPDX-License-Identifier: GPL-2.0-or-later
//
// Solar eclipses from Besselian elements (include/prometheia/solar_eclipse.hpp,
// docs/SOLAR_ECLIPSES.md).

#include "prometheia/solar_eclipse.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>

#include "prometheia/vec3.hpp"

namespace prometheia::solar_eclipse {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kDeg = kPi / 180.0;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kE2 = kFlattening * (2.0 - kFlattening);
constexpr double kPolar = 1.0 - kFlattening; // c/a
constexpr double kSynodic = 29.530588861;    // mean lunation, days
constexpr double kNewMoon0 = 2451550.09766;  // mean new moon of 2000-01-06 (lunation 0)

double norm_pm(double a, double period) { // into [-period/2, period/2)
    a = std::fmod(a + period / 2, period);
    if (a < 0)
        a += period;
    return a - period / 2;
}

// ---- The state of the Sun and Moon at one instant -----------------------

struct State {
    Besselian b;
    Vec3 sun_km, moon_km; // geocentric apparent, true equator and equinox of date
    Vec3 xh, yh, zh;      // the fundamental plane's axes in that frame
    double gast = 0.0;    // radians
};

double delta_t_s(const Options& o, double jd_tt) {
    return o.delta_t ? o.delta_t->delta_t_seconds(jd_tt) : time::delta_t(jd_tt);
}

Result<State> state_at(Engine& engine, double jd_tt, const Options& o) {
    CalcOptions co = CalcOptions::apparent();
    co.frame = Frame::TrueOfDate;
    co.coords = Coords::Equatorial;
    co.speed = false;
    co.sigma = false;
    auto sun = engine.calc(body::kSun, jd_tt, co);
    if (!sun.ok())
        return sun.error();
    auto moon = engine.calc(body::kMoon, jd_tt, co);
    if (!moon.ok())
        return moon.error();
    State s;
    const auto& sp = sun.value().pos;
    const auto& mp = moon.value().pos;
    s.sun_km = Vec3{sp.xyz_au[0], sp.xyz_au[1], sp.xyz_au[2]} * kAuKm;
    s.moon_km = Vec3{mp.xyz_au[0], mp.xyz_au[1], mp.xyz_au[2]} * kAuKm;
    const double jd_ut1 = jd_tt - delta_t_s(o, jd_tt) / 86400.0;
    auto st = engine.sidereal_time_ut(jd_ut1, true);
    if (!st.ok())
        return st.error();
    s.gast = st.value() * kDeg;

    const Vec3 axis = s.sun_km - s.moon_km;
    const double dist = norm(axis);
    s.zh = axis * (1.0 / dist);
    const double a = std::atan2(s.zh.y, s.zh.x);
    const double d = std::atan2(s.zh.z, std::hypot(s.zh.x, s.zh.y));
    s.xh = Vec3{-std::sin(a), std::cos(a), 0.0};
    s.yh = cross(s.zh, s.xh);

    Besselian& b = s.b;
    b.jd_tt = jd_tt;
    b.jd_ut1 = jd_ut1;
    b.x = dot(s.moon_km, s.xh) / kEarthRadiusKm;
    b.y = dot(s.moon_km, s.yh) / kEarthRadiusKm;
    b.z_moon = dot(s.moon_km, s.zh) / kEarthRadiusKm;
    b.d_rad = d;
    b.ra_rad = a;
    b.mu_rad = norm_pm(s.gast - a - kPi, kTwoPi) + kPi; // [0, 2pi)
    const double sin_f1 = (o.sun_radius_km + o.k_penumbra * kEarthRadiusKm) / dist;
    const double sin_f2 = (o.sun_radius_km - o.k_umbra * kEarthRadiusKm) / dist;
    const double cos_f1 = std::sqrt(1.0 - sin_f1 * sin_f1);
    const double cos_f2 = std::sqrt(1.0 - sin_f2 * sin_f2);
    b.tan_f1 = sin_f1 / cos_f1;
    b.tan_f2 = sin_f2 / cos_f2;
    b.l1 = b.z_moon * b.tan_f1 + o.k_penumbra / cos_f1;
    b.l2 = b.z_moon * b.tan_f2 - o.k_umbra / cos_f2;
    return s;
}

// ---- Earth geometry in the fundamental plane -----------------------------

// Semi-axis, along y, of the Earth's outline seen along the shadow axis (the
// other, along x, is 1): the spheroid projected at declination d.
double outline_b(double d_rad) {
    const double sd = std::sin(d_rad), cd = std::cos(d_rad);
    return std::sqrt(sd * sd + kPolar * kPolar * cd * cd);
}

// Distance from (x, y) to the outline ellipse (1, b), negative inside;
// theta receives the nearest outline point's parameter.
double outline_distance(double x, double y, double b, double& theta) {
    constexpr int kN = 72;
    double best = 1e30, bt = 0.0;
    for (int i = 0; i < kN; ++i) {
        const double t = kTwoPi * i / kN;
        const double d = std::hypot(x - std::cos(t), y - b * std::sin(t));
        if (d < best) {
            best = d;
            bt = t;
        }
    }
    double lo = bt - kTwoPi / kN, hi = bt + kTwoPi / kN;
    const double g = 0.6180339887498949;
    auto dist = [&](double t) { return std::hypot(x - std::cos(t), y - b * std::sin(t)); };
    double c = hi - g * (hi - lo), d = lo + g * (hi - lo);
    double fc = dist(c), fd = dist(d);
    for (int i = 0; i < 40; ++i) {
        if (fc < fd) {
            hi = d;
            d = c;
            fd = fc;
            c = hi - g * (hi - lo);
            fc = dist(c);
        } else {
            lo = c;
            c = d;
            fc = fd;
            d = lo + g * (hi - lo);
            fd = dist(d);
        }
    }
    theta = 0.5 * (lo + hi);
    const double dd = dist(theta);
    return (x * x + y * y / (b * b) < 1.0) ? -dd : dd;
}

struct Ground {
    Vec3 p;      // Earth radii, true equator of date
    double zeta; // the point's height above the fundamental plane
};

// Where the line parallel to the axis through plane point (xi, eta) meets the
// ellipsoid on the Sun's side. A line that misses by a hair is taken as
// grazing.
std::optional<Ground> ground_point(const State& s, double xi, double eta) {
    const Vec3 q = s.xh * xi + s.yh * eta;
    const Vec3& z = s.zh;
    const double a = z.x * z.x + z.y * z.y + z.z * z.z / (kPolar * kPolar);
    const double b2 = q.x * z.x + q.y * z.y + q.z * z.z / (kPolar * kPolar); // half of B
    const double c = q.x * q.x + q.y * q.y + q.z * q.z / (kPolar * kPolar) - 1.0;
    double disc = b2 * b2 - a * c;
    if (disc < 0.0) {
        if (disc < -1e-9)
            return std::nullopt;
        disc = 0.0;
    }
    const double zeta = (-b2 + std::sqrt(disc)) / a;
    return Ground{q + z * zeta, zeta};
}

Place place_of(const Vec3& p, double gast) {
    const double lon = norm_pm(std::atan2(p.y, p.x) - gast, kTwoPi);
    const double r = std::hypot(p.x, p.y);
    double lat = std::atan2(p.z, r * (1.0 - kE2));
    for (int i = 0; i < 6; ++i) {
        const double sl = std::sin(lat);
        const double n = 1.0 / std::sqrt(1.0 - kE2 * sl * sl);
        lat = std::atan2(p.z + kE2 * n * sl, r);
    }
    return Place{lat / kDeg, lon / kDeg};
}

// A site's geocentric vector in Earth radii, true equator of date.
Vec3 site_vector(const frames::GeoSite& site, double gast) {
    const double sl = std::sin(site.lat_rad), cl = std::cos(site.lat_rad);
    const double n = 1.0 / std::sqrt(1.0 - kE2 * sl * sl);
    const double h = site.height_m / (kEarthRadiusKm * 1000.0);
    const double th = gast + site.lon_rad;
    const double r = (n + h) * cl;
    return Vec3{r * std::cos(th), r * std::sin(th), (n * (1.0 - kE2) + h) * sl};
}

// The shadow at a site at one instant.
struct Local1 {
    double u = 0, v = 0, m = 0; // the axis minus the site, in the plane, and its length
    double L1 = 0, L2 = 0;
    double alt_deg = 0, az_deg = 0; // geometric, the Sun's centre
};

Local1 local_at_state(const State& s, const frames::GeoSite& site) {
    const Vec3 o = site_vector(site, s.gast);
    Local1 r;
    const double xi = dot(o, s.xh), eta = dot(o, s.yh), zeta = dot(o, s.zh);
    r.u = s.b.x - xi;
    r.v = s.b.y - eta;
    r.m = std::hypot(r.u, r.v);
    r.L1 = s.b.l1 - zeta * s.b.tan_f1;
    r.L2 = s.b.l2 - zeta * s.b.tan_f2;
    const Vec3 dir = normalized(s.sun_km - o * kEarthRadiusKm);
    const double sl = std::sin(site.lat_rad), cl = std::cos(site.lat_rad);
    const double th = s.gast + site.lon_rad;
    const Vec3 up{cl * std::cos(th), cl * std::sin(th), sl};
    const Vec3 east{-std::sin(th), std::cos(th), 0.0};
    const Vec3 north{-sl * std::cos(th), -sl * std::sin(th), cl};
    r.alt_deg = std::asin(std::clamp(dot(dir, up), -1.0, 1.0)) / kDeg;
    double az = std::atan2(dot(dir, east), dot(dir, north));
    if (az < 0)
        az += kTwoPi;
    r.az_deg = az / kDeg;
    return r;
}

// Fraction of the Sun's disc (radius R) covered by the Moon's (radius r) with
// centres d apart.
double obscuration_of(double R, double r, double d) {
    if (d >= R + r)
        return 0.0;
    if (d <= std::fabs(R - r))
        return r >= R ? 1.0 : (r * r) / (R * R);
    const double a1 = std::acos(std::clamp((d * d + R * R - r * r) / (2 * d * R), -1.0, 1.0));
    const double a2 = std::acos(std::clamp((d * d + r * r - R * R) / (2 * d * r), -1.0, 1.0));
    const double area =
        R * R * a1 + r * r * a2 -
        0.5 * std::sqrt(std::max(0.0, (-d + r + R) * (d + r - R) * (d - r + R) * (d + r + R)));
    return std::clamp(area / (kPi * R * R), 0.0, 1.0);
}

// ---- Numerical helpers ----------------------------------------------------

// First error raised inside a numerical search: the callbacks return NaN and
// park the error here.
struct Fail {
    std::optional<Error> err;
    double take(Result<double> r) {
        if (r.ok())
            return r.value();
        if (!err)
            err = r.error();
        return kNaN;
    }
};

double golden_min(const std::function<double(double)>& f, double lo, double hi, double tol) {
    const double g = 0.6180339887498949;
    double c = hi - g * (hi - lo), d = lo + g * (hi - lo);
    double fc = f(c), fd = f(d);
    while (hi - lo > tol) {
        if (fc < fd) {
            hi = d;
            d = c;
            fd = fc;
            c = hi - g * (hi - lo);
            fc = f(c);
        } else {
            lo = c;
            c = d;
            fc = fd;
            d = lo + g * (hi - lo);
            fd = f(d);
        }
    }
    return 0.5 * (lo + hi);
}

// Root of f in [a, b] (Illinois); false when the ends do not bracket one.
bool find_root(const std::function<double(double)>& f, double a, double b, double tol,
               double& root) {
    double fa = f(a), fb = f(b);
    if (!(fa * fb <= 0.0))
        return false;
    if (fa == 0.0) {
        root = a;
        return true;
    }
    if (fb == 0.0) {
        root = b;
        return true;
    }
    int side = 0;
    double c = a;
    for (int i = 0; i < 100; ++i) {
        const double prev = c;
        c = (a * fb - b * fa) / (fb - fa);
        if (!(c > std::min(a, b) && c < std::max(a, b)))
            c = 0.5 * (a + b);
        const double fc = f(c);
        if (fc == 0.0 || std::fabs(c - prev) < tol * 0.1 || std::fabs(b - a) < tol)
            break;
        if (fc * fb > 0) {
            b = c;
            fb = fc;
            if (side == -1)
                fa *= 0.5;
            side = -1;
        } else {
            a = c;
            fa = fc;
            if (side == 1)
                fb *= 0.5;
            side = 1;
        }
    }
    root = c;
    return true;
}

constexpr double kTimeTol = 2e-8; // days, 2 ms

// ---- The shadow axis on the ground ----------------------------------------

struct Track {
    bool hits = false;
    unsigned type = 0;
    Place centre, north, south;
    double alt_deg = 0, width_km = kNaN, duration_s = kNaN, magnitude = 0, obscuration = 0;
};

double magnitude_of(const Local1& l) {
    return (l.L1 - l.m) / (l.L1 + l.L2);
}
double obscuration_at(const Local1& l) {
    return obscuration_of(0.5 * (l.L1 + l.L2), 0.5 * (l.L1 - l.L2), l.m);
}

frames::GeoSite site_of(const Place& p) {
    return frames::GeoSite{p.lon_deg * kDeg, p.lat_deg * kDeg, 0.0};
}

Result<Track> track_at(Engine& engine, const State& s, const Options& o) {
    Track t;
    const double b = outline_b(s.b.d_rad);
    if (s.b.x * s.b.x + s.b.y * s.b.y / (b * b) >= 1.0)
        return t;
    auto g = ground_point(s, s.b.x, s.b.y);
    if (!g)
        return t;
    t.hits = true;
    t.centre = place_of(g->p, s.gast);
    const frames::GeoSite site = site_of(t.centre);
    const Local1 l = local_at_state(s, site);
    t.type = l.L2 < 0 ? kTotal : kAnnular;
    t.alt_deg = l.alt_deg;
    t.magnitude = magnitude_of(l);
    t.obscuration = obscuration_at(l);

    // Relative velocity of the axis over the ground point, by central differences.
    const double h = 30.0 / 86400.0;
    auto sa = state_at(engine, s.b.jd_tt - h, o);
    if (!sa.ok())
        return sa.error();
    auto sb = state_at(engine, s.b.jd_tt + h, o);
    if (!sb.ok())
        return sb.error();
    const Local1 la = local_at_state(sa.value(), site), lb = local_at_state(sb.value(), site);
    const double vu = (lb.u - la.u) / (2 * h), vv = (lb.v - la.v) / (2 * h); // Re/day
    const double speed = std::hypot(vu, vv);
    if (speed > 0)
        t.duration_s = 2.0 * std::fabs(l.L2) / speed * 86400.0;

    // The umbra's limits along the perpendicular to the motion.
    const double xi0 = dot(site_vector(site, s.gast), s.xh);
    const double eta0 = dot(site_vector(site, s.gast), s.yh);
    const double px = -vv / speed, py = vu / speed;
    Place lim[2];
    bool ok[2] = {false, false};
    for (int side = 0; side < 2; ++side) {
        const double sg = side == 0 ? 1.0 : -1.0;
        auto edge = [&](double sdist, double& hval) {
            const double xi = xi0 + sg * sdist * px, eta = eta0 + sg * sdist * py;
            auto gp = ground_point(s, xi, eta);
            if (!gp)
                return false;
            const double m = std::hypot(s.b.x - xi, s.b.y - eta);
            hval = m - std::fabs(s.b.l2 - gp->zeta * s.b.tan_f2);
            return true;
        };
        const double step = 0.004;
        double prev = 0.0, hv = 0.0;
        for (double sd = step; sd < 0.4; sd += step) {
            if (!edge(sd, hv))
                break;
            if (hv > 0.0) {
                double root = sd;
                find_root(
                    [&](double q) {
                        double hq = 0.0;
                        return edge(q, hq) ? hq : 1.0;
                    },
                    prev, sd, 1e-9, root);
                auto gp = ground_point(s, xi0 + sg * root * px, eta0 + sg * root * py);
                if (gp) {
                    lim[side] = place_of(gp->p, s.gast);
                    ok[side] = true;
                }
                break;
            }
            prev = sd;
        }
    }
    t.north = t.south = Place{kNaN, kNaN};
    if (ok[0] && ok[1]) {
        const bool first_north = lim[0].lat_deg > lim[1].lat_deg;
        t.north = first_north ? lim[0] : lim[1];
        t.south = first_north ? lim[1] : lim[0];
        auto vec = [&](const Place& p) { return site_vector(site_of(p), 0.0); };
        t.width_km = norm(vec(lim[0]) - vec(lim[1])) * kEarthRadiusKm;
    }
    return t;
}

} // namespace

// ---- Public: elements, saros ------------------------------------------------

Result<Besselian> besselian_elements(Engine& engine, double jd_tt, const Options& opts) {
    auto s = state_at(engine, jd_tt, opts);
    if (!s.ok())
        return s.error();
    return s.value().b;
}

int saros_number(long lunation) {
    long r = (38 * lunation + 112) % 223;
    if (r < 0)
        r += 223;
    if (r > 202)
        r -= 223;
    return int(r);
}

namespace {

// ---- Global analysis of one lunation ---------------------------------------

struct Searcher {
    Engine& engine;
    const Options& opts;
    Fail fail;

    double m2(double t) {
        auto s = state_at(engine, t, opts);
        if (!s.ok()) {
            if (!fail.err)
                fail.err = s.error();
            return kNaN;
        }
        return s.value().b.x * s.value().b.x + s.value().b.y * s.value().b.y;
    }
    // The signed distance of the axis from the Earth's outline minus a
    // function of the elements.
    double outline_minus(double t, bool umbra) {
        auto s = state_at(engine, t, opts);
        if (!s.ok()) {
            if (!fail.err)
                fail.err = s.error();
            return kNaN;
        }
        double th;
        const auto& b = s.value().b;
        const double d = outline_distance(b.x, b.y, outline_b(b.d_rad), th);
        return d - (umbra ? std::fabs(b.l2) : b.l1);
    }
    double inside_ellipse(double t) {
        auto s = state_at(engine, t, opts);
        if (!s.ok()) {
            if (!fail.err)
                fail.err = s.error();
            return kNaN;
        }
        const auto& b = s.value().b;
        const double bb = outline_b(b.d_rad);
        return b.x * b.x + b.y * b.y / (bb * bb) - 1.0;
    }
};

// The instant near t_mean when the Moon and Sun share ecliptic longitude, and
// the Moon's latitude then.
Result<bool> conjunction(Engine& engine, double t_mean, double& t_c, double& beta_deg) {
    CalcOptions co = CalcOptions::apparent();
    co.frame = Frame::TrueOfDate;
    co.coords = Coords::Ecliptic;
    co.sigma = false;
    double t = t_mean;
    for (int i = 0; i < 8; ++i) {
        auto sun = engine.calc(body::kSun, t, co);
        if (!sun.ok())
            return sun.error();
        auto moon = engine.calc(body::kMoon, t, co);
        if (!moon.ok())
            return moon.error();
        const double dl = norm_pm(moon.value().pos.lon_deg - sun.value().pos.lon_deg, 360.0);
        const double rate = moon.value().pos.lon_speed - sun.value().pos.lon_speed;
        beta_deg = moon.value().pos.lat_deg - sun.value().pos.lat_deg;
        t -= dl / rate;
        if (std::fabs(dl) < 1e-7)
            break;
    }
    t_c = t;
    return true;
}

struct Analysis {
    bool eclipse = false;
    Global g;
};

Result<Analysis> analyze(Engine& engine, double t_c, const Options& opts) {
    Searcher sr{engine, opts, {}};
    Analysis out;
    const double tg =
        golden_min([&](double t) { return sr.m2(t); }, t_c - 0.25, t_c + 0.25, kTimeTol);
    if (sr.fail.err)
        return *sr.fail.err;
    auto st = state_at(engine, tg, opts);
    if (!st.ok())
        return st.error();
    const State& s = st.value();
    const Besselian& e = s.b;
    const double b = outline_b(e.d_rad);
    double theta = 0.0;
    const double dist = outline_distance(e.x, e.y, b, theta);
    if (dist >= e.l1)
        return out;
    out.eclipse = true;
    Global& g = out.g;
    g.elements = e;
    g.max_tt = tg;
    g.max_ut1 = e.jd_ut1;
    g.gamma = std::copysign(std::hypot(e.x, e.y), e.y);
    g.lunation = std::lround((t_c - kNewMoon0) / kSynodic);
    g.saros = saros_number(g.lunation);
    const double nan = kNaN;
    g.p1_tt = g.u1_tt = g.c1_tt = g.c2_tt = g.u4_tt = g.p4_tt = nan;
    g.path_width_km = g.central_duration_s = nan;

    const bool central = dist < 0.0;
    const bool umbral = central || dist < std::fabs(e.l2);
    const double span = 0.2; // days either side of greatest eclipse, to bracket the contacts
    auto root = [&](const std::function<double(double)>& f, double lo, double hi, double& r) {
        return find_root(f, lo, hi, kTimeTol, r);
    };
    root([&](double t) { return sr.outline_minus(t, false); }, tg - span, tg, g.p1_tt);
    root([&](double t) { return sr.outline_minus(t, false); }, tg, tg + span, g.p4_tt);
    if (umbral) {
        root([&](double t) { return sr.outline_minus(t, true); }, tg - span, tg, g.u1_tt);
        root([&](double t) { return sr.outline_minus(t, true); }, tg, tg + span, g.u4_tt);
    }
    if (central) {
        root([&](double t) { return sr.inside_ellipse(t); }, tg - span, tg, g.c1_tt);
        root([&](double t) { return sr.inside_ellipse(t); }, tg, tg + span, g.c2_tt);
    }
    if (sr.fail.err)
        return *sr.fail.err;

    // Kind.
    if (central) {
        // Total where l2 < 0 at the axis' ground point, annular where > 0;
        // hybrid when both occur along the track.
        bool neg = false, pos = false;
        const double c1 = std::isnan(g.c1_tt) ? tg : g.c1_tt;
        const double c2 = std::isnan(g.c2_tt) ? tg : g.c2_tt;
        constexpr int kSamples = 24;
        for (int i = 0; i <= kSamples; ++i) {
            const double t = c1 + (c2 - c1) * i / kSamples;
            auto si = state_at(engine, t, opts);
            if (!si.ok())
                return si.error();
            auto gp = ground_point(si.value(), si.value().b.x, si.value().b.y);
            const double zeta = gp ? gp->zeta : 0.0;
            const double L2 = si.value().b.l2 - zeta * si.value().b.tan_f2;
            (L2 < 0 ? neg : pos) = true;
        }
        g.type = (neg && pos ? kHybrid : (neg ? kTotal : kAnnular)) | kCentral;
    } else if (umbral) {
        g.type = (e.l2 < 0 ? kTotal : kAnnular) | kNonCentral;
    } else {
        g.type = kPartial;
    }

    // The point of greatest eclipse, and the circumstances there.
    Local1 l;
    if (central) {
        auto gp = ground_point(s, e.x, e.y);
        g.greatest = place_of(gp->p, s.gast);
        l = local_at_state(s, site_of(g.greatest));
        auto tr = track_at(engine, s, opts);
        if (!tr.ok())
            return tr.error();
        g.path_width_km = tr.value().width_km;
        g.central_duration_s = tr.value().duration_s;
    } else {
        // The nearest outline point, nudged inside so the line meets the Earth.
        auto gp = ground_point(s, 0.9999999 * std::cos(theta), 0.9999999 * b * std::sin(theta));
        if (!gp)
            return Error{ErrorCode::NotFound, "no ground point at the outline"};
        g.greatest = place_of(gp->p, s.gast);
        l = local_at_state(s, site_of(g.greatest));
    }
    g.magnitude_covered = magnitude_of(l);
    g.magnitude = !(g.type & kCentral) ? g.magnitude_covered : (l.L1 - l.L2) / (l.L1 + l.L2);
    g.obscuration = obscuration_at(l);
    g.sun_alt_deg = l.alt_deg;
    g.sun_az_deg = l.az_deg;
    return out;
}

} // namespace

Result<Global> next_global(Engine& engine, double jd_start_tt, unsigned kinds, bool backward,
                           const Options& opts) {
    if (!std::isfinite(jd_start_tt))
        return Error{ErrorCode::ArgumentError, "start is not finite"};
    kinds &= kAllKinds;
    if (kinds == 0)
        kinds = kAllKinds;
    const double kf = (jd_start_tt - kNewMoon0) / kSynodic;
    long k = backward ? long(std::ceil(kf)) + 1 : long(std::floor(kf)) - 1;
    for (int n = 0; n < 3000; ++n, k += backward ? -1 : 1) {
        const double t_mean = kNewMoon0 + kSynodic * double(k);
        double t_c = 0.0, beta = 0.0;
        auto cj = conjunction(engine, t_mean, t_c, beta);
        if (!cj.ok())
            return cj.error();
        if (std::fabs(beta) > 1.9)
            continue;
        auto an = analyze(engine, t_c, opts);
        if (!an.ok())
            return an.error();
        if (!an.value().eclipse)
            continue;
        const Global& g = an.value().g;
        if (backward ? g.max_tt >= jd_start_tt : g.max_tt <= jd_start_tt)
            continue;
        if (!(g.type & kinds))
            continue;
        return g;
    }
    return Error{ErrorCode::NotFound, "no such solar eclipse found"};
}

Result<Where> where(Engine& engine, double jd_tt, const Options& opts) {
    auto s = state_at(engine, jd_tt, opts);
    if (!s.ok())
        return s.error();
    auto tr = track_at(engine, s.value(), opts);
    if (!tr.ok())
        return tr.error();
    Where w;
    w.jd_tt = jd_tt;
    w.jd_ut1 = s.value().b.jd_ut1;
    w.elements = s.value().b;
    const Track& t = tr.value();
    w.axis_hits = t.hits;
    w.type = t.type;
    w.central = t.hits ? t.centre : Place{kNaN, kNaN};
    w.north_limit = t.north;
    w.south_limit = t.south;
    if (!t.hits) {
        w.north_limit = w.south_limit = Place{kNaN, kNaN};
        w.sun_alt_deg = w.path_width_km = w.central_duration_s = w.magnitude = w.obscuration = kNaN;
        return w;
    }
    w.sun_alt_deg = t.alt_deg;
    w.path_width_km = t.width_km;
    w.central_duration_s = t.duration_s;
    w.magnitude = t.magnitude;
    w.obscuration = t.obscuration;
    return w;
}

Result<Local> local_at(Engine& engine, const Global& eclipse, const frames::GeoSite& site,
                       const Options& opts) {
    Fail fail;
    auto local1 = [&](double t, Local1& out) {
        auto s = state_at(engine, t, opts);
        if (!s.ok()) {
            if (!fail.err)
                fail.err = s.error();
            return false;
        }
        out = local_at_state(s.value(), site);
        return true;
    };
    auto fm = [&](double t) {
        Local1 l;
        return local1(t, l) ? l.m * l.m : kNaN;
    };
    const double t0 = eclipse.max_tt;
    const double tmax = golden_min(fm, t0 - 0.125, t0 + 0.125, kTimeTol);
    Local1 lm;
    if (!local1(tmax, lm))
        return *fail.err;
    if (lm.m >= lm.L1)
        return Error{ErrorCode::NotFound, "no eclipse at this site"};

    Local r;
    r.lunation = eclipse.lunation;
    r.saros = eclipse.saros;
    r.gamma = eclipse.gamma;
    r.global_max_tt = eclipse.max_tt;
    r.max_tt = tmax;
    r.max_ut1 = tmax - delta_t_s(opts, tmax) / 86400.0;
    r.max_alt_deg = lm.alt_deg;
    r.max_az_deg = lm.az_deg;
    r.visible_max = lm.alt_deg >= 0.0;
    r.magnitude = magnitude_of(lm);
    r.obscuration = obscuration_at(lm);
    r.diameter_ratio = (lm.L1 - lm.L2) / (lm.L1 + lm.L2);
    const bool umbral = lm.m < std::fabs(lm.L2);
    r.type = umbral ? (lm.L2 < 0 ? kTotal : kAnnular) : kPartial;

    for (int i = 0; i < 4; ++i)
        r.contact_tt[i] = r.contact_ut1[i] = r.contact_alt_deg[i] = r.contact_az_deg[i] = kNaN;
    const double span = 0.2;
    auto contact = [&](int idx, bool inner, double lo, double hi) {
        double t = 0.0;
        auto f = [&](double tt) {
            Local1 l;
            if (!local1(tt, l))
                return kNaN;
            return l.m - (inner ? std::fabs(l.L2) : l.L1);
        };
        if (!find_root(f, lo, hi, kTimeTol, t))
            return;
        Local1 l;
        if (!local1(t, l))
            return;
        r.contact_alt_deg[idx] = l.alt_deg;
        r.contact_az_deg[idx] = l.az_deg;
        if (l.alt_deg >= 0.0) { // below the horizon the contact is missing
            r.contact_tt[idx] = t;
            r.contact_ut1[idx] = t - delta_t_s(opts, t) / 86400.0;
        }
    };
    contact(0, false, tmax - span, tmax);
    contact(3, false, tmax, tmax + span);
    if (umbral) {
        contact(1, true, tmax - span, tmax);
        contact(2, true, tmax, tmax + span);
    }
    if (fail.err)
        return *fail.err;
    r.duration_s = (std::isnan(r.contact_tt[1]) || std::isnan(r.contact_tt[2]))
                       ? kNaN
                       : (r.contact_tt[2] - r.contact_tt[1]) * 86400.0;
    return r;
}

Result<Local> local_next(Engine& engine, double jd_start_tt, const frames::GeoSite& site,
                         bool backward, const Options& opts) {
    double start = jd_start_tt;
    for (int n = 0; n < 400; ++n) {
        auto g = next_global(engine, start, kAllKinds, backward, opts);
        if (!g.ok())
            return g.error();
        start = g.value().max_tt + (backward ? -1.0 : 1.0);
        auto l = local_at(engine, g.value(), site, opts);
        if (!l.ok()) {
            if (l.error().code == ErrorCode::NotFound)
                continue;
            return l.error();
        }
        const Local& v = l.value();
        bool seen = v.visible_max;
        for (int i = 0; i < 4; ++i)
            seen = seen || !std::isnan(v.contact_tt[i]);
        if (seen)
            return v;
    }
    return Error{ErrorCode::NotFound, "no eclipse visible from the site found"};
}

} // namespace prometheia::solar_eclipse

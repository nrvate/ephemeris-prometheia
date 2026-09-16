// SPDX-License-Identifier: GPL-2.0-or-later
#include <prometheia/kepler.hpp>

#include <algorithm>
#include <cmath>

namespace prometheia {

namespace {

constexpr double kNewtonTol = 1e-14; // relative; a handful of iterations
constexpr int kNewtonMaxIter = 64;

double wrap_pi(double x) {
    const double two_pi = 6.283185307179586476925286766559;
    x = std::fmod(x, two_pi);
    if (x > 3.1415926535897932384626433832795)
        x -= two_pi;
    if (x < -3.1415926535897932384626433832795)
        x += two_pi;
    return x;
}

// Solve M = E - e sin E for E (0 <= e < 1). Newton from a good starter.
double solve_elliptic_e(double e, double M) {
    M = wrap_pi(M);
    // Starter: Danby's recommendation — linear for small e, better for high e.
    double E = e < 0.8 ? M : 3.1415926535897932384626433832795;
    for (int i = 0; i < kNewtonMaxIter; ++i) {
        const double f = E - e * std::sin(E) - M;
        const double fp = 1.0 - e * std::cos(E);
        const double dE = f / fp;
        E -= dE;
        if (std::fabs(dE) <= kNewtonTol * (1.0 + std::fabs(E)))
            break;
    }
    return E;
}

// Solve M = e sinh H - H for H (e > 1). M scaled by the same sign as a.
double solve_hyperbolic_h(double e, double M) {
    const double s = M >= 0.0 ? 1.0 : -1.0;
    const double absM = std::fabs(M);
    // Starter: linear near periapsis, asymptotic log for large M.
    double H = std::log(2.0 * absM / e + 1.8);
    if (!(H > 0.0))
        H = absM / e; // guard: log of tiny
    for (int i = 0; i < kNewtonMaxIter; ++i) {
        const double f = e * std::sinh(H) - H - absM;
        const double fp = e * std::cosh(H) - 1.0;
        const double dH = f / fp;
        H -= dH;
        if (std::fabs(dH) <= kNewtonTol * (1.0 + std::fabs(H)))
            break;
    }
    return s * H;
}

} // namespace

double mean_motion(double mu, double a) {
    return std::sqrt(mu / (std::fabs(a) * std::fabs(a) * std::fabs(a)));
}

Result<State> elements_to_state(double mu, const Elements& el) {
    if (!(mu > 0.0) || !std::isfinite(mu)) {
        return make_error(ErrorCode::ArgumentError, "mu must be positive finite");
    }
    if (!std::isfinite(el.a) || el.a == 0.0 || !std::isfinite(el.e) || el.e < 0.0 || el.e == 1.0) {
        return make_error(ErrorCode::ArgumentError, "invalid a/e");
    }
    if ((el.e < 1.0 && el.a <= 0.0) || (el.e > 1.0 && el.a >= 0.0)) {
        return make_error(ErrorCode::ArgumentError, "a/e sign inconsistent");
    }

    // Perifocal coordinates, then rotate by argp, inc, node.
    double px, py, vx, vy;
    if (el.e < 1.0) {
        const double E = solve_elliptic_e(el.e, el.mean_anom);
        const double sq = std::sqrt(1.0 - el.e * el.e);
        const double r_over_a = 1.0 - el.e * std::cos(E);
        px = el.a * (std::cos(E) - el.e);
        py = el.a * sq * std::sin(E);
        const double k = el.a * mean_motion(mu, el.a) / r_over_a; // > 0
        vx = -k * std::sin(E);
        vy = k * sq * std::cos(E);
    } else {
        // Hyperbolic, prograde convention matching the elliptic branch
        // (h = r x v along +z of the perifocal frame):
        //   r = |a|(e cosh H - 1),  x = |a|(e - cosh H),  y = |a| sqrt(e^2-1) sinh H,
        //   dH/dt = n / (e cosh H - 1).
        const double abs_a = std::fabs(el.a);
        const double H = solve_hyperbolic_h(el.e, el.mean_anom);
        const double sq = std::sqrt(el.e * el.e - 1.0);
        const double coshH = std::cosh(H), sinhH = std::sinh(H);
        const double denom = el.e * coshH - 1.0; // = r / |a| > 0
        px = abs_a * (el.e - coshH);
        py = abs_a * sq * sinhH;
        const double k = abs_a * mean_motion(mu, el.a) / denom;
        vx = -k * sinhH;
        vy = k * sq * coshH;
    }

    // Rotation: R3(node) * R1(inc) * R3(argp).
    const double cw = std::cos(el.argp), sw = std::sin(el.argp);
    const double ci = std::cos(el.inc), si = std::sin(el.inc);
    const double cn = std::cos(el.node), sn = std::sin(el.node);

    const double r11 = cw * cn - sw * sn * ci;
    const double r12 = -sw * cn - cw * sn * ci;
    const double r21 = cw * sn + sw * cn * ci;
    const double r22 = -sw * sn + cw * cn * ci;
    const double r31 = sw * si;
    const double r32 = cw * si;

    State s;
    s.pos = Vec3(r11 * px + r12 * py, r21 * px + r22 * py, r31 * px + r32 * py);
    s.vel = Vec3(r11 * vx + r12 * vy, r21 * vx + r22 * vy, r31 * vx + r32 * vy);
    return s;
}

Result<Elements> state_to_elements(double mu, const State& s) {
    if (!(mu > 0.0))
        return make_error(ErrorCode::ArgumentError, "mu must be positive");
    const double r = norm(s.pos);
    const double v2 = norm2(s.vel);
    if (!(r > 0.0))
        return make_error(ErrorCode::ArgumentError, "degenerate state");

    const Vec3 hvec = cross(s.pos, s.vel);
    const double h = norm(hvec);
    if (!(h > 0.0))
        return make_error(ErrorCode::ArgumentError, "radial orbit: h = 0");

    const Vec3 evec = (v2 - mu / r) * s.pos - dot(s.pos, s.vel) * s.vel;
    const double ev = norm(evec) / mu;

    Elements el;
    el.e = ev;
    el.a = 1.0 / (2.0 / r - v2 / mu); // negative automatically when hyperbolic
    el.inc = std::acos(std::clamp(hvec.z / h, -1.0, 1.0));
    el.node = std::atan2(hvec.x, -hvec.y);

    // Argument of perihelion: angle from node vector to evector in orbital plane.
    Vec3 nvec = normalized(Vec3(-hvec.y, hvec.x, 0.0)); // node line, h × n = z-ish
    if (el.inc < 1e-12)
        nvec = Vec3(1.0, 0.0, 0.0); // equatorial: node undefined
    const double cos_argp = dot(nvec, evec) / (norm(nvec) * norm(evec) + 1e-300);
    double argp = std::acos(std::clamp(cos_argp, -1.0, 1.0));
    if (dot(cross(nvec, evec), hvec) < 0.0)
        argp = 6.283185307179586476925286766559 - argp;
    el.argp = el.e < 1e-12 ? 0.0 : argp; // circular: argp undefined

    // True anomaly from evector, then mean anomaly via the appropriate anomaly.
    const double cos_nu = dot(evec, s.pos) / (norm(evec) * r + 1e-300);
    double nu = std::acos(std::clamp(cos_nu, -1.0, 1.0));
    if (dot(s.pos, s.vel) < 0.0)
        nu = 6.283185307179586476925286766559 - nu;
    if (el.e < 1.0) {
        const double E = 2.0 * std::atan2(std::sqrt(1.0 - el.e) * std::sin(nu / 2.0),
                                          std::sqrt(1.0 + el.e) * std::cos(nu / 2.0));
        el.mean_anom = wrap_pi(E - el.e * std::sin(E));
    } else {
        // tan(nu/2) = sqrt((e+1)/(e-1)) tanh(H/2); invert, with the log form
        // when |tanh| approaches 1 (far from periapsis).
        const double tn = std::tan(nu / 2.0) / std::sqrt((el.e + 1.0) / (el.e - 1.0));
        double H;
        if (std::fabs(tn) < 1.0) {
            H = 2.0 * std::atanh(tn);
        } else {
            const double t = std::fabs(tn);
            H = 2.0 * std::log(t + std::sqrt(t * t - 1.0)) * (tn > 0.0 ? 1.0 : -1.0);
        }
        el.mean_anom = el.e * std::sinh(H) - H;
    }
    return el;
}

Result<State> kepler_propagate(double mu, const State& s0, double t0, double t1) {
    auto el = state_to_elements(mu, s0);
    if (!el.ok())
        return Result<State>(el.error());
    Elements prop = el.value();
    const double dt = t1 - t0;
    if (prop.e < 1.0) {
        prop.mean_anom += mean_motion(mu, prop.a) * dt;
    } else {
        prop.mean_anom += mean_motion(mu, prop.a) * dt;
    }
    return elements_to_state(mu, prop);
}

} // namespace prometheia

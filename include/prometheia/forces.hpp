// SPDX-License-Identifier: GPL-2.0-or-later
//
// Force models for small-body integration. HeliocentricForce is the v1
// model: the central mass at the origin, perturbers on Trajectory tables.
// BarycentricForce is the engine's model: every mass, the Sun included,
// is a perturber whose barycentric state comes from the loaded planetary
// ephemeris.
#ifndef PROMETHEIA_FORCES_HPP
#define PROMETHEIA_FORCES_HPP

#include <cstddef>

#include <vector>

#include <prometheia/trajectory.hpp>

namespace prometheia {

struct Perturber {
    double mu = 0.0;                  // GM of the perturber (AU^3/day^2)
    const Trajectory* traj = nullptr; // not owned
};

// Heliocentric N-body point-mass force: central mu at origin plus moving
// perturbers. dydt[0..2] = velocity, dydt[3..5] = acceleration.
struct HeliocentricForce {
    double mu_central = 0.0;
    const std::vector<Perturber>* perturbers = nullptr;

    void operator()(const double y[6], double t, double dydt[6]) const {
        const double r2 = y[0] * y[0] + y[1] * y[1] + y[2] * y[2];
        const double inv_r3 = 1.0 / (r2 * std::sqrt(r2));
        dydt[0] = y[3];
        dydt[1] = y[4];
        dydt[2] = y[5];
        const double f = -mu_central * inv_r3;
        double ax = f * y[0], ay = f * y[1], az = f * y[2];
        for (const Perturber& p : *perturbers) {
            const State ps = p.traj->eval(t);
            const double dx = ps.pos.x - y[0], dy = ps.pos.y - y[1], dz = ps.pos.z - y[2];
            const double d2 = dx * dx + dy * dy + dz * dz;
            const double inv_d3 = p.mu / (d2 * std::sqrt(d2));
            ax += inv_d3 * dx;
            ay += inv_d3 * dy;
            az += inv_d3 * dz;
        }
        dydt[3] = ax;
        dydt[4] = ay;
        dydt[5] = az;
    }
};

// ---------------------------------------------------------------------------
// Barycentric force model.

// GM values from the DE440 header constants (public domain; JPL), in
// AU^3/day^2 — the units the force models take. The engine prefers the
// constants published by the opened ephemeris and falls back to these;
// DE440 values differ from DE441 by ~1e-9 relative, far below any
// accuracy this library promises for small bodies.
namespace gm {

// Sun GMS; the mass SBDB-style heliocentric elements are referred to.
inline constexpr double kSun = 0.295912208284119561e-3;
inline constexpr double kMercury = 0.491250019488931818e-10;      // GM1
inline constexpr double kVenus = 0.724345233264411869e-9;         // GM2
inline constexpr double kEarthMoonBary = 0.899701139294734660e-9; // GMB
inline constexpr double kMars = 0.954954882972581189e-10;         // GM4
inline constexpr double kJupiter = 0.282534582522579175e-6;       // GM5
inline constexpr double kSaturn = 0.845970599337629027e-7;        // GM6
inline constexpr double kUranus = 0.129202656496823994e-7;        // GM7
inline constexpr double kNeptune = 0.152435734788519386e-7;       // GM8
inline constexpr double kPluto = 0.217509646489335811e-11;        // GM9
// EMRAT, the Earth/Moon mass ratio that splits GMB.
inline constexpr double kEmrat = 81.3005682214972154;

inline constexpr double kEarth = kEarthMoonBary * kEmrat / (1.0 + kEmrat);
inline constexpr double kMoon = kEarthMoonBary / (1.0 + kEmrat);

// The asteroid perturbers of JPL's SB441-N16 kernel (IOM 392R-21-005): GMs
// as published in the DE440 header (MA0001, ...), AU^3/day^2, by number;
// 0 for any other body.
inline double asteroid(int number) {
    switch (number) {
    case 1:
        return 1.396451812308107e-13; // Ceres
    case 2:
        return 3.04711463300432e-14; // Pallas
    case 3:
        return 4.282343967799501e-15; // Juno
    case 4:
        return 3.8548000225257904e-14; // Vesta
    case 7:
        return 2.5416014973471498e-15; // Iris
    case 10:
        return 1.254253076164081e-14; // Hygiea
    case 15:
        return 4.5107799051436795e-15; // Eunomia
    case 16:
        return 3.5445002842488978e-15; // Psyche
    case 31:
        return 2.4067012218937576e-15; // Euphrosyne
    case 52:
        return 5.982431526486984e-15; // Europa
    case 65:
        return 2.0917175955133682e-15; // Cybele
    case 87:
        return 4.834560654610552e-15; // Sylvia
    case 88:
        return 2.6529436610356353e-15; // Thisbe
    case 107:
        return 3.219139207587859e-15; // Camilla
    case 511:
        return 8.683625349228654e-15; // Davida
    case 704:
        return 6.311034342087889e-15; // Interamnia
    }
    return 0.0;
}

} // namespace gm

// Barycentric states of the perturbing masses, indexed 0..count()-1.
// Implemented by the engine over the loaded ephemeris (sampling it onto
// spline tables lazily, in blocks, as integration windows march). The
// calls are non-const: this is where coverage grows.
struct PerturberStates {
    virtual ~PerturberStates() = default;

    // Makes sure the tables cover TDB JD t. Builds them on first use.
    // A failure (no masses at all, or the ephemeris does not cover the
    // epoch) is sticky: ok() turns false and states read zero — the
    // caller refuses results after checking ok().
    virtual void ensure(double t) = 0;

    // out = {x, y, z, vx, vy, vz}, barycentric, AU and AU/day. Assumes
    // ensure(t) has been called for this t.
    virtual void state(size_t index, double t, double out[6]) = 0;

    // All count() states at once into out[6 * index .. 6 * index + 5]; the
    // force model's per-evaluation call. The default loops over state().
    virtual void states(double t, double* out) {
        for (size_t i = 0; i < count(); ++i)
            state(i, t, out + 6 * i);
    }

    virtual size_t count() const = 0;
    // GM per perturber (AU^3/day^2), count() entries.
    virtual const double* mus() const = 0;
    virtual bool ok() const = 0;
    // Index of the Sun among the perturbers (the source of the relativistic
    // term), or -1 when it is absent or not yet known.
    virtual long sun_index() const { return -1; }
    // Index of the perturber with this NAIF/SPK id, or -1: lets a body that
    // is itself one of the perturbing masses leave itself out.
    virtual long index_of(long) const { return -1; }
};

// Speed of light in AU/day (IAU 2012 AU, exact c).
inline constexpr double kLightAuPerDay = 299792.458 * 86400.0 / 149597870.7;

// Barycentric N-body point-mass force: acceleration is the sum of
// mu_p (r_p - r)/|r_p - r|^3 over the perturbers — the Sun is one of
// them, so there is no central-body term — plus, when the perturbers
// name the Sun, its post-Newtonian (Schwarzschild, PPN beta = gamma = 1)
// correction in the test-particle form used for asteroid integrations:
//   a_GR = mu / (c^2 r^3) [ (4 mu / r - v^2) r_vec + 4 (r . v) v_vec ]
// with r, v relative to the Sun. It is the source of the relativistic
// perihelion advance 6 pi mu / (c^2 a (1 - e^2)) per orbit (43"/century
// for Mercury, ~0.3"/century at 2.8 AU). dydt[0..2] = velocity,
// dydt[3..5] = acceleration. Not const: lazy perturber coverage.
struct BarycentricForce {
    PerturberStates* perturbers = nullptr; // not owned
    bool relativity = true;                // the Sun's post-Newtonian term
    long exclude_id = -1;                  // the integrated body's own id: never perturbs itself

    void operator()(const double y[6], double t, double dydt[6]) {
        dydt[0] = y[3];
        dydt[1] = y[4];
        dydt[2] = y[5];
        perturbers->ensure(t);
        if (!perturbers->ok() || perturbers->count() == 0) {
            // The engine rejects results whenever ok() is false; zero
            // acceleration keeps the integrator finite in the meantime.
            dydt[3] = dydt[4] = dydt[5] = 0.0;
            return;
        }
        const size_t n = perturbers->count();
        const double* mu = perturbers->mus();
        const long sun = relativity ? perturbers->sun_index() : -1;
        const long self = exclude_id >= 0 ? perturbers->index_of(exclude_id) : -1;
        double buffer[6 * 32];
        std::vector<double> large;
        double* all = buffer;
        if (n > 32) {
            large.resize(6 * n);
            all = large.data();
        }
        perturbers->states(t, all);
        double ax = 0.0, ay = 0.0, az = 0.0;
        for (size_t i = 0; i < n; ++i) {
            if (long(i) == self)
                continue;
            const double* ps = all + 6 * i;
            const double dx = ps[0] - y[0], dy = ps[1] - y[1], dz = ps[2] - y[2];
            const double d2 = dx * dx + dy * dy + dz * dz;
            const double k = mu[i] / (d2 * std::sqrt(d2));
            ax += k * dx;
            ay += k * dy;
            az += k * dz;
            if (long(i) == sun) {
                const double r[3] = {-dx, -dy, -dz};
                const double v[3] = {y[3] - ps[3], y[4] - ps[4], y[5] - ps[5]};
                const double rr = std::sqrt(d2);
                const double v2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
                const double rv = r[0] * v[0] + r[1] * v[1] + r[2] * v[2];
                const double g = mu[i] / (kLightAuPerDay * kLightAuPerDay * d2 * rr);
                const double cr = g * (4.0 * mu[i] / rr - v2), cv = g * 4.0 * rv;
                ax += cr * r[0] + cv * v[0];
                ay += cr * r[1] + cv * v[1];
                az += cr * r[2] + cv * v[2];
            }
        }
        dydt[3] = ax;
        dydt[4] = ay;
        dydt[5] = az;
    }
};

} // namespace prometheia

#endif // PROMETHEIA_FORCES_HPP
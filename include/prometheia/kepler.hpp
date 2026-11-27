// SPDX-License-Identifier: GPL-2.0-or-later
//
// Two-body mechanics from published theory (Danby, "Fundamentals of
// Celestial Mechanics"; Vallado, "Fundamentals of Astrodynamics").
//
// Three roles in this library:
//  1. the engine derives initial Cartesian states from catalog elements;
//  2. KeplerPropagate is the closed-form two-body oracle the M2/M5 test
//     gates validate the numeric integrator against (the two-body problem
//     has an exact solution — the classic external reference);
//  3. osculating-elements conversion supports ephemeris diagnostics.
#ifndef PROMETHEIA_KEPLER_HPP
#define PROMETHEIA_KEPLER_HPP

#include <prometheia/error.hpp>
#include <prometheia/vec3.hpp>

namespace prometheia {

struct Elements {
    double a = 0.0;         // semimajor axis (AU in catalogs; any unit works here
                            // as long as mu matches) — negative for hyperbolic
    double e = 0.0;         // eccentricity (>= 0, != 1)
    double inc = 0.0;       // inclination, rad
    double node = 0.0;      // longitude of ascending node, rad
    double argp = 0.0;      // argument of perihelion, rad
    double mean_anom = 0.0; // mean anomaly at epoch, rad
};

struct State {
    Vec3 pos;
    Vec3 vel;
};

// mean motion n = sqrt(mu / |a|^3), valid elliptic and hyperbolic alike.
double mean_motion(double mu, double a);

// Elements -> Cartesian state at epoch.
Result<State> elements_to_state(double mu, const Elements& el);

// Cartesian state -> osculating elements.
Result<Elements> state_to_elements(double mu, const State& s);

// Exact (closed-form) two-body propagation from epoch to t. Elliptic via
// Newton iteration on the eccentric anomaly, hyperbolic via the hyperbolic
// anomaly; parabolic (e = 1) is rejected — singular elements.
Result<State> kepler_propagate(double mu, const State& s0, double t0, double t1);

} // namespace prometheia

#endif // PROMETHEIA_KEPLER_HPP

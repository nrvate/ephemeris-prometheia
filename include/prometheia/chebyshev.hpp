// SPDX-License-Identifier: GPL-2.0-or-later
//
// Chebyshev series evaluation shared by the external-ephemeris readers
// (JPL DE binaries, NAIF SPK). Our own formats do not store Chebyshev
// series; this exists only to read theirs.
#ifndef PROMETHEIA_CHEBYSHEV_HPP
#define PROMETHEIA_CHEBYSHEV_HPP

namespace prometheia {

// Value and d/dtau derivative of sum(cs[k] * T_k(tau)) for k < n.
// T_0 = 1, T_1 = tau, T_k = 2*tau*T_{k-1} - T_{k-2};
// T'_k = k * U_{k-1} with the same recurrence for U (U_0 = 1, U_1 = 2*tau).
inline void chebyshev_eval(const double* cs, int n, double tau, double& value, double& deriv) {
    if (n <= 0) {
        value = 0.0;
        deriv = 0.0;
        return;
    }
    if (n == 1) {
        value = cs[0];
        deriv = 0.0;
        return;
    }
    double t_km2 = 1.0, t_km1 = tau;
    double u_km2 = 1.0, u_km1 = 2.0 * tau;
    value = cs[0] + cs[1] * tau;
    deriv = cs[1];
    for (int k = 2; k < n; ++k) {
        const double t_k = 2.0 * tau * t_km1 - t_km2;
        const double u_k = 2.0 * tau * u_km1 - u_km2;
        value += cs[k] * t_k;
        deriv += cs[k] * double(k) * u_km1;
        t_km2 = t_km1;
        t_km1 = t_k;
        u_km2 = u_km1;
        u_km1 = u_k;
    }
}

} // namespace prometheia

#endif // PROMETHEIA_CHEBYSHEV_HPP

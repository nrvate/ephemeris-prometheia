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

// The same for three series sharing tau and n, stored one after another
// (cs[c * n + k]), as a position's x, y, z are. The recurrences are computed
// once for all three; each series accumulates exactly as chebyshev_eval
// does, so the results are the same to the bit.
inline void chebyshev_eval3(const double* cs, int n, double tau, double value[3], double deriv[3]) {
    if (n <= 1) {
        for (int c = 0; c < 3; ++c)
            chebyshev_eval(cs + c * n, n, tau, value[c], deriv[c]);
        return;
    }
    const double* c0 = cs;
    const double* c1 = cs + n;
    const double* c2 = cs + 2 * n;
    double v0 = c0[0] + c0[1] * tau, v1 = c1[0] + c1[1] * tau, v2 = c2[0] + c2[1] * tau;
    double d0 = c0[1], d1 = c1[1], d2 = c2[1];
    double t_km2 = 1.0, t_km1 = tau;
    double u_km2 = 1.0, u_km1 = 2.0 * tau;
    for (int k = 2; k < n; ++k) {
        const double t_k = 2.0 * tau * t_km1 - t_km2;
        const double u_k = 2.0 * tau * u_km1 - u_km2;
        const double dk = double(k);
        v0 += c0[k] * t_k;
        v1 += c1[k] * t_k;
        v2 += c2[k] * t_k;
        d0 += c0[k] * dk * u_km1; // (c k) u, the order chebyshev_eval rounds in
        d1 += c1[k] * dk * u_km1;
        d2 += c2[k] * dk * u_km1;
        t_km2 = t_km1;
        t_km1 = t_k;
        u_km2 = u_km1;
        u_km1 = u_k;
    }
    value[0] = v0;
    value[1] = v1;
    value[2] = v2;
    deriv[0] = d0;
    deriv[1] = d1;
    deriv[2] = d2;
}

} // namespace prometheia

#endif // PROMETHEIA_CHEBYSHEV_HPP

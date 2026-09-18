// SPDX-License-Identifier: GPL-2.0-or-later
#include <prometheia/elements.hpp>

#include <cmath>

namespace prometheia::elements {

namespace {

constexpr double kDegPerRad = 57.295779513082320876798154814105;

} // namespace

double evaluate(const double c[5], int n_terms, double T) {
    double v = 0.0;
    for (int i = n_terms - 1; i >= 0; --i)
        v = v * T + c[i];
    return v;
}

double mean_anomaly_deg(const PolynomialElements& el, double jd_tt) {
    const double dt = jd_tt - el.epoch_jd_tt;
    const double T = dt / 36525.0;
    // Any nonzero M coefficient past the constant makes M the fitted mean
    // anomaly of date in full; adding a mean motion on top would count the
    // motion twice. Nonzero, not n_terms: the count is shared by all six
    // elements, and zero padding must not change the body.
    for (int i = 1; i < el.n_terms; ++i)
        if (el.mean_anomaly[i] != 0.0)
            return evaluate(el.mean_anomaly, el.n_terms, T);

    // Otherwise the constant is M at the epoch, carried along at the Gaussian
    // mean motion so the answer does not depend on the loaded ephemeris. a is
    // taken at the instant, from its own polynomial.
    const double a = evaluate(el.semi_major_axis, el.n_terms, T);
    double n = kGaussK / (a * std::sqrt(a)); // rad/day
    if (el.centre == ElementCentre::Earth)
        n /= std::sqrt(kSunEarthMassRatio); // the Earth alone, not Earth+Moon
    return el.mean_anomaly[0] + n * kDegPerRad * dt;
}

} // namespace prometheia::elements

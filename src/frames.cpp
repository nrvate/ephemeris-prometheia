// SPDX-License-Identifier: GPL-2.0-or-later
//
// IAU 2006/2000A precession-nutation and sidereal time. All constants
// from USNO Circular 179 (Kaplan 2005, public domain); derivation and
// validation in docs/FRAMES.md.
#include "prometheia/frames.hpp"

#include <cmath>

namespace prometheia::frames {
namespace {

constexpr double kAs2Rad = 3.14159265358979323846 / (180.0 * 3600.0);
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kJ2000 = 2451545.0;
constexpr double kDaysPerCentury = 36525.0;

double centuries(double jd_tt) {
    return (jd_tt - kJ2000) / kDaysPerCentury;
}

// Row-major rotation matrices. With these, R1(eps) converts a vector's
// equatorial components to ecliptic components, and the precession
// matrix P = R3(-z) R2(theta) R3(-zeta) maps ICRF to mean-of-date.
void rot1(double a, double m[9]) {
    const double c = std::cos(a), s = std::sin(a);
    m[0] = 1.0;
    m[1] = 0.0;
    m[2] = 0.0;
    m[3] = 0.0;
    m[4] = c;
    m[5] = s;
    m[6] = 0.0;
    m[7] = -s;
    m[8] = c;
}
void rot2(double a, double m[9]) {
    const double c = std::cos(a), s = std::sin(a);
    m[0] = c;
    m[1] = 0.0;
    m[2] = -s;
    m[3] = 0.0;
    m[4] = 1.0;
    m[5] = 0.0;
    m[6] = s;
    m[7] = 0.0;
    m[8] = c;
}
void rot3(double a, double m[9]) {
    const double c = std::cos(a), s = std::sin(a);
    m[0] = c;
    m[1] = s;
    m[2] = 0.0;
    m[3] = -s;
    m[4] = c;
    m[5] = 0.0;
    m[6] = 0.0;
    m[7] = 0.0;
    m[8] = 1.0;
}
void matmul(const double a[9], const double b[9], double out[9]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out[3 * i + j] = a[3 * i] * b[j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j];
        }
    }
}

// One nutation series term: 14 integer argument multipliers and the
// coefficients S, Sdot, C0, C, Cdot, S0 (arcseconds), per Circular 179
// eqs. 5.15-5.16.
struct NutationTerm {
    int m[14];
    double c[6];
};
#include "nutation_table.inc"

// WGS84 ellipsoid (published values).
constexpr double kWgs84A = 6378137.0; // m
constexpr double kWgs84F = 1.0 / 298.257223563;

} // namespace

void fundamental_arguments(double jd_tt, double phi[14]) {
    const double T = centuries(jd_tt);
    // Simon et al. (1994) expressions as printed in Circular 179 eqs.
    // 5.17-5.19; coefficients in arcseconds.
    static const double kPlanet[8][2] = {
        {908103.259872, 538101628.688982}, {655127.283060, 210664136.433548},
        {361679.244588, 129597742.283429}, {1279558.798488, 68905077.493988},
        {123665.467464, 10925660.377991},  {180278.799480, 4399609.855732},
        {1130598.018396, 1542481.193933},  {1095655.195728, 786550.320744}};
    for (int j = 0; j < 8; ++j) {
        phi[j] = std::fmod((kPlanet[j][0] + kPlanet[j][1] * T) * kAs2Rad, kTwoPi);
        if (phi[j] < 0.0)
            phi[j] += kTwoPi;
    }
    phi[8] = std::fmod((5028.8200 * T + 1.112022 * T * T) * kAs2Rad, kTwoPi);
    if (phi[8] < 0.0)
        phi[8] += kTwoPi;
    phi[9] = std::fmod((485868.249036 + 1717915923.2178 * T + 31.8792 * T * T +
                        0.051635 * T * T * T - 0.00024470 * T * T * T * T) *
                           kAs2Rad,
                       kTwoPi);
    phi[10] = std::fmod((1287104.79305 + 129596581.0481 * T - 0.5532 * T * T +
                         0.000136 * T * T * T - 0.00001149 * T * T * T * T) *
                            kAs2Rad,
                        kTwoPi);
    phi[11] = std::fmod((335779.526232 + 1739527262.8478 * T - 12.7512 * T * T -
                         0.001037 * T * T * T + 0.00000417 * T * T * T * T) *
                            kAs2Rad,
                        kTwoPi);
    phi[12] = std::fmod((1072260.70369 + 1602961601.2090 * T - 6.3706 * T * T +
                         0.006593 * T * T * T - 0.00003169 * T * T * T * T) *
                            kAs2Rad,
                        kTwoPi);
    phi[13] = std::fmod((450160.398036 - 6962890.5431 * T + 7.4722 * T * T + 0.007702 * T * T * T -
                         0.00005939 * T * T * T * T) *
                            kAs2Rad,
                        kTwoPi);
    if (phi[9] < 0.0)
        phi[9] += kTwoPi;
    if (phi[10] < 0.0)
        phi[10] += kTwoPi;
    if (phi[11] < 0.0)
        phi[11] += kTwoPi;
    if (phi[12] < 0.0)
        phi[12] += kTwoPi;
    if (phi[13] < 0.0)
        phi[13] += kTwoPi;
}

void nutation(double jd_tt, double& dpsi, double& deps) {
    const double T = centuries(jd_tt);
    double phi[14];
    fundamental_arguments(jd_tt, phi);
    double sum_psi = 0.0, sum_eps = 0.0;
    // The circular recommends accumulating smallest terms first; the table
    // is printed largest first, so walk it backwards.
    for (int i = 1365 - 1; i >= 0; --i) {
        const NutationTerm& t = kNutationTerms[i];
        double ang = 0.0;
        for (int j = 0; j < 14; ++j) {
            if (t.m[j] != 0)
                ang += double(t.m[j]) * phi[j];
        }
        const double sa = std::sin(ang), ca = std::cos(ang);
        sum_psi += (t.c[0] + t.c[1] * T) * sa + t.c[2] * ca;
        sum_eps += (t.c[3] + t.c[4] * T) * ca + t.c[5] * sa;
    }
    dpsi = sum_psi * kAs2Rad;
    deps = sum_eps * kAs2Rad;
}

void precession_angles(double jd_tt, double& zeta, double& z, double& theta) {
    const double T = centuries(jd_tt);
    // Circular 179 eq. 5.11 (IAU 2006), arcseconds.
    zeta = (2.650545 + 2306.083227 * T + 0.2988499 * T * T + 0.01801828 * T * T * T -
            0.000005971 * T * T * T * T - 0.0000003173 * T * T * T * T * T) *
           kAs2Rad;
    z = (-2.650545 + 2306.077181 * T + 1.0927348 * T * T + 0.01826837 * T * T * T -
         0.000028596 * T * T * T * T - 0.0000002904 * T * T * T * T * T) *
        kAs2Rad;
    theta = (2004.191903 * T - 0.4294934 * T * T - 0.04182264 * T * T * T -
             0.000007089 * T * T * T * T - 0.0000001274 * T * T * T * T * T) *
            kAs2Rad;
}

double mean_obliquity(double jd_tt) {
    const double T = centuries(jd_tt);
    // Circular 179 eq. 5.12, arcseconds, epsilon0 = 84381.406.
    const double as = 84381.406 - 46.836769 * T - 0.0001831 * T * T + 0.00200340 * T * T * T -
                      0.000000576 * T * T * T * T - 0.0000000434 * T * T * T * T * T;
    return as * kAs2Rad;
}

void mean_equator_of_date_matrix(double jd_tt, double m[9]) {
    double zeta, z, theta;
    precession_angles(jd_tt, zeta, z, theta);
    double r3a[9], r2[9], r3b[9], ab[9];
    rot3(-z, r3a);
    rot2(theta, r2);
    rot3(-zeta, r3b);
    matmul(r3a, r2, ab);
    matmul(ab, r3b, m);
}

void true_equator_of_date_matrix(double jd_tt, double m[9]) {
    double p[9];
    mean_equator_of_date_matrix(jd_tt, p);
    double dpsi, deps;
    nutation(jd_tt, dpsi, deps);
    const double eps = mean_obliquity(jd_tt);
    // N = R1(-(eps + deps)) R3(-dpsi) R1(eps): mean -> true equator of
    // date, derived from the classical construction (see docs/FRAMES.md)
    // and verified differentially against Swiss Ephemeris.
    double ra[9], r3[9], rb[9], t1[9], t2[9];
    rot1(-(eps + deps), ra);
    rot3(-dpsi, r3);
    rot1(eps, rb);
    matmul(ra, r3, t1);
    matmul(t1, rb, t2);
    matmul(t2, p, m);
}

void mean_ecliptic_of_date_matrix(double jd_tt, double m[9]) {
    double p[9];
    mean_equator_of_date_matrix(jd_tt, p);
    double re[9];
    rot1(mean_obliquity(jd_tt), re);
    matmul(re, p, m);
}

void true_ecliptic_of_date_matrix(double jd_tt, double m[9]) {
    // The ecliptic plane does not nutate; the true equinox slides along it
    // by dpsi, so apparent longitudes are mean longitudes + dpsi and
    // latitudes are unchanged. deps does not enter this frame.
    double base[9];
    mean_ecliptic_of_date_matrix(jd_tt, base);
    double dpsi, deps;
    nutation(jd_tt, dpsi, deps);
    double r3[9];
    rot3(-dpsi, r3);
    matmul(r3, base, m);
}

double earth_rotation_angle(double jd_ut1) {
    // Circular 179 eq. 2.11: rotations; frac(JD) keeps full precision.
    const double frac = jd_ut1 - std::floor(jd_ut1);
    const double du = jd_ut1 - kJ2000;
    double theta = 0.7790572732640 + 0.00273781191135448 * du + frac;
    theta -= std::floor(theta);
    return theta * kTwoPi;
}

double gmst_rad(double jd_ut1, double jd_tt) {
    const double theta = earth_rotation_angle(jd_ut1) / kTwoPi;
    const double T = centuries(jd_tt);
    // Circular 179 eq. 2.12: the polynomial is the accumulated precession
    // of the equinox in right ascension (arcseconds); GMST in seconds.
    const double precession_ra = 0.014506 + 4612.156534 * T + 1.3915817 * T * T -
                                 0.00000044 * T * T * T - 0.000029956 * T * T * T * T -
                                 0.0000000368 * T * T * T * T * T;
    const double gmst_seconds = 86400.0 * theta + precession_ra / 15.0;
    return gmst_seconds * (kTwoPi / 86400.0);
}

double equation_of_equinoxes_rad(double jd_tt) {
    const double T = centuries(jd_tt);
    double phi[14];
    fundamental_arguments(jd_tt, phi);
    const double& om = phi[13];
    const double& f = phi[11];
    const double& d = phi[12];
    double dpsi, deps;
    nutation(jd_tt, dpsi, deps);
    // Circular 179 eq. 2.14, arcseconds.
    const double as =
        dpsi / kAs2Rad * std::cos(mean_obliquity(jd_tt)) + 0.00264096 * std::sin(om) +
        0.00006352 * std::sin(2.0 * om) + 0.00001175 * std::sin(2.0 * f - 2.0 * d + 3.0 * om) +
        0.00001121 * std::sin(2.0 * f - 2.0 * d + om) -
        0.00000455 * std::sin(2.0 * f - 2.0 * d + 2.0 * om) +
        0.00000202 * std::sin(2.0 * f + 3.0 * om) + 0.00000198 * std::sin(2.0 * f + om) -
        0.00000172 * std::sin(3.0 * om) - 0.00000087 * T * std::sin(om);
    return as * kAs2Rad;
}

double gast_rad(double jd_ut1, double jd_tt) {
    return gmst_rad(jd_ut1, jd_tt) + equation_of_equinoxes_rad(jd_tt);
}

void observer_geocentric(const GeoSite& site, double gast, double out[3]) {
    // Geodetic -> geocentric on the WGS84 ellipsoid, then rotate the ECEF
    // vector into the true equator-and-equinox-of-date frame by the
    // apparent sidereal time. Polar motion is neglected.
    const double e2 = kWgs84F * (2.0 - kWgs84F);
    const double sin_lat = std::sin(site.lat_rad);
    const double cos_lat = std::cos(site.lat_rad);
    const double n = kWgs84A / std::sqrt(1.0 - e2 * sin_lat * sin_lat);
    const double x = (n + site.height_m) * cos_lat; // m, in equatorial plane
    const double z = (n * (1.0 - e2) + site.height_m) * sin_lat;
    const double c = std::cos(gast), s = std::sin(gast);
    const double x_ecef = x * std::cos(site.lon_rad);
    const double y_ecef = x * std::sin(site.lon_rad);
    out[0] = (c * x_ecef - s * y_ecef) / 1000.0; // km
    out[1] = (s * x_ecef + c * y_ecef) / 1000.0;
    out[2] = z / 1000.0;
}

} // namespace prometheia::frames
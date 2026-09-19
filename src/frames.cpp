// SPDX-License-Identifier: GPL-2.0-or-later
//
// IAU 2006/2000A precession-nutation and sidereal time. All constants
// from USNO Circular 179 (Kaplan 2005, public domain); derivation and
// validation in docs/FRAMES.md.
#include "prometheia/frames.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <vector>

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

namespace {

// The series as printed: one sine/cosine pair per term, 1365 terms, two
// libm calls each. It is kept because it is the definition the fast form
// below is checked against (nutation_fast_form_matches_printed), and
// nothing else calls it.
void nutation_series_reference(double jd_tt, double out[6]);

// The same series, summed without calling sin and cos 2730 times.
//
// Every term's argument is an integer combination of the same fourteen
// fundamental arguments, so the sine and cosine of each multiple that the
// table actually uses are built once, by angle addition from the argument
// itself, and each term's pair is then composed from two or three complex
// multiplications. The table is also compacted to its nonzero multipliers,
// which removes an inner loop of fourteen branches per term.
//
// This is arithmetic rearrangement, not approximation: it agrees with the
// printed form to 3e-22 rad (6e-17 arcsec) on dpsi and deps, far below the
// 0.004 uas the half-day interpolator already costs. It takes a node from
// 64 us to 8 us, which matters because the first body asked for over a time
// window pays for every nutation node in it and the rest ride free.
// The printed table uses at most six nonzero multipliers in a term and a
// largest multiple of 21; both are checked when the compact table is built,
// so a regenerated table that broke either would abort rather than read past
// the end of a multiple table.
constexpr int kMaxFactors = 6;
constexpr int kMaxMultiple = 21;

struct CompactTerm {
    uint8_t n = 0;
    uint8_t arg[kMaxFactors] = {};
    int8_t mul[kMaxFactors] = {};
    double fmul[kMaxFactors] = {}; // mul, as the double the sums use
    double c[6] = {};
};

struct CompactTable {
    std::vector<CompactTerm> terms;
    int max_multiple[14] = {};
};

const CompactTable& compact_table() {
    static const CompactTable table = [] {
        CompactTable t;
        t.terms.reserve(1365);
        for (int i = 1365 - 1; i >= 0; --i) { // smallest first, as the circular asks
            const NutationTerm& src = kNutationTerms[i];
            CompactTerm c;
            for (int j = 0; j < 14; ++j) {
                if (src.m[j] == 0)
                    continue;
                if (c.n >= kMaxFactors || std::abs(src.m[j]) > kMaxMultiple)
                    std::abort(); // the table outgrew the compact form
                c.arg[c.n] = uint8_t(j);
                c.mul[c.n] = int8_t(src.m[j]);
                c.fmul[c.n] = double(src.m[j]);
                ++c.n;
                t.max_multiple[j] = std::max(t.max_multiple[j], std::abs(src.m[j]));
            }
            std::copy(std::begin(src.c), std::end(src.c), std::begin(c.c));
            t.terms.push_back(c);
        }
        return t;
    }();
    return table;
}

// sin and cos of m * phi[j] for every multiple the table uses, by angle
// addition from m = 1. Twelve additions carry at most 1e-15 of relative
// error, which the agreement figure above accounts for.
struct MultipleTables {
    // Index kMaxMultiple + m for the multiple m, negative ones included
    // (sin(-x) = -sin x, cos(-x) = cos x, exactly), so no term branches on
    // its multiplier's sign.
    double sn[14][2 * kMaxMultiple + 1];
    double cs[14][2 * kMaxMultiple + 1];
};

void build_multiples(const double phi[14], const CompactTable& t, MultipleTables& m) {
    constexpr int z = kMaxMultiple;
    for (int j = 0; j < 14; ++j) {
        double* sn = m.sn[j] + z;
        double* cs = m.cs[j] + z;
        sn[0] = 0.0;
        cs[0] = 1.0;
        const int top = t.max_multiple[j];
        if (top == 0)
            continue;
        const double s1 = std::sin(phi[j]), c1 = std::cos(phi[j]);
        sn[1] = s1;
        cs[1] = c1;
        for (int k = 2; k <= top; ++k) {
            sn[k] = sn[k - 1] * c1 + cs[k - 1] * s1;
            cs[k] = cs[k - 1] * c1 - sn[k - 1] * s1;
        }
        for (int k = 1; k <= top; ++k) {
            sn[-k] = -sn[k];
            cs[-k] = cs[k];
        }
    }
}

// sin and cos of the term's whole argument, composed from its factors.
inline void term_sin_cos(const CompactTerm& t, const MultipleTables& m, double& sa, double& ca) {
    sa = 0.0;
    ca = 1.0;
    for (int k = 0; k < t.n; ++k) {
        const int j = t.arg[k], at = kMaxMultiple + t.mul[k];
        const double s2 = m.sn[j][at];
        const double c2 = m.cs[j][at];
        const double s = sa * c2 + ca * s2;
        ca = ca * c2 - sa * s2;
        sa = s;
    }
}

} // namespace

void nutation(double jd_tt, double& dpsi, double& deps) {
    const double T = centuries(jd_tt);
    double phi[14];
    fundamental_arguments(jd_tt, phi);
    const CompactTable& table = compact_table();
    MultipleTables mult;
    build_multiples(phi, table, mult);
    double sum_psi = 0.0, sum_eps = 0.0;
    // The circular recommends accumulating smallest terms first; the
    // compact table is already in that order.
    for (const CompactTerm& t : table.terms) {
        double sa, ca;
        term_sin_cos(t, mult, sa, ca);
        sum_psi += (t.c[0] + t.c[1] * T) * sa + t.c[2] * ca;
        sum_eps += (t.c[3] + t.c[4] * T) * ca + t.c[5] * sa;
    }
    dpsi = sum_psi * kAs2Rad;
    deps = sum_eps * kAs2Rad;
}

void nutation_with_rates(double jd_tt, double out[6]) {
    const double T = centuries(jd_tt);
    double phi[14];
    fundamental_arguments(jd_tt, phi);
    // First and second derivatives of the fundamental arguments in
    // arcsec/century and arcsec/century^2 (the polynomials of
    // fundamental_arguments differentiated).
    static const double kPlanetRate[8] = {538101628.688982, 210664136.433548, 129597742.283429,
                                          68905077.493988,  10925660.377991,  4399609.855732,
                                          1542481.193933,   786550.320744};
    double d1[14], d2[14];
    for (int j = 0; j < 8; ++j) {
        d1[j] = kPlanetRate[j];
        d2[j] = 0.0;
    }
    d1[8] = 5028.8200 + 2.0 * 1.112022 * T;
    d2[8] = 2.0 * 1.112022;
    auto quartic = [&](int j, double b1, double b2, double b3, double b4) {
        d1[j] = b1 + T * (2.0 * b2 + T * (3.0 * b3 + T * 4.0 * b4));
        d2[j] = 2.0 * b2 + T * (6.0 * b3 + T * 12.0 * b4);
    };
    quartic(9, 1717915923.2178, 31.8792, 0.051635, -0.00024470);
    quartic(10, 129596581.0481, -0.5532, 0.000136, -0.00001149);
    quartic(11, 1739527262.8478, -12.7512, -0.001037, 0.00000417);
    quartic(12, 1602961601.2090, -6.3706, 0.006593, -0.00003169);
    quartic(13, -6962890.5431, 7.4722, 0.007702, -0.00005939);
    for (int j = 0; j < 14; ++j) {
        d1[j] *= kAs2Rad;
        d2[j] *= kAs2Rad;
    }

    const CompactTable& table = compact_table();
    MultipleTables mult;
    build_multiples(phi, table, mult);
    double psi = 0.0, eps = 0.0, psi1 = 0.0, eps1 = 0.0, psi2 = 0.0, eps2 = 0.0;
    for (const CompactTerm& t : table.terms) {
        double w = 0.0, w2 = 0.0; // the argument's rates (rad/cy, rad/cy^2)
        for (int k = 0; k < t.n; ++k) {
            w += t.fmul[k] * d1[t.arg[k]];
            w2 += t.fmul[k] * d2[t.arg[k]];
        }
        double sa, ca;
        term_sin_cos(t, mult, sa, ca);
        // The term p (psi) and its quadrature q = dp/d(argument); the same
        // for eps. Then p' = c1 sin + q w and p'' = 2 c1 cos w - p w^2 + q w2,
        // which reuses p and q instead of expanding them per derivative.
        const double a_psi = t.c[0] + t.c[1] * T, a_eps = t.c[3] + t.c[4] * T;
        const double p = a_psi * sa + t.c[2] * ca, q = a_psi * ca - t.c[2] * sa;
        const double e = a_eps * ca + t.c[5] * sa, r = t.c[5] * ca - a_eps * sa;
        const double ww = w * w;
        psi += p;
        eps += e;
        psi1 += t.c[1] * sa + q * w;
        eps1 += t.c[4] * ca + r * w;
        psi2 += 2.0 * t.c[1] * ca * w - p * ww + q * w2;
        eps2 += -2.0 * t.c[4] * sa * w - e * ww + r * w2;
    }
    constexpr double kCy = 36525.0;
    out[0] = psi * kAs2Rad;
    out[1] = eps * kAs2Rad;
    out[2] = psi1 * kAs2Rad / kCy;
    out[3] = eps1 * kAs2Rad / kCy;
    out[4] = psi2 * kAs2Rad / (kCy * kCy);
    out[5] = eps2 * kAs2Rad / (kCy * kCy);
}

namespace {

// The printed form, for the test that keeps the fast one honest.
void nutation_series_reference(double jd_tt, double out[6]) {
    const double T = centuries(jd_tt);
    double phi[14];
    fundamental_arguments(jd_tt, phi);
    double sum_psi = 0.0, sum_eps = 0.0;
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
    out[0] = sum_psi * kAs2Rad;
    out[1] = sum_eps * kAs2Rad;
}

} // namespace

void nutation_printed_form(double jd_tt, double& dpsi, double& deps) {
    double out[2];
    nutation_series_reference(jd_tt, out);
    dpsi = out[0];
    deps = out[1];
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

// --- Long-term precession (Vondrak, Capitaine & Wallace 2011) ---------

namespace {

// One periodic term: period (centuries), then cosine and sine amplitudes
// (arcsec) of the first and second parameter of the table.
struct LtpTerm {
    double period_cy, c1, s1, c2, s2;
};

// Table 1: P_A, Q_A. C7 of Q_A as corrected in A&A 541, C1 (2012):
// 198.296701 (the original printed 198.296071); with it the series
// vanish exactly at J2000.
constexpr LtpTerm kLtpPQ[] = {
    {708.15, -5486.751211, 667.666730, -684.661560, -5523.863691},
    {2309.00, -17.127623, -2354.886252, 2446.283880, -549.747450},
    {1620.00, -617.517403, -428.152441, 399.671049, -310.998056},
    {492.20, 413.442940, 376.202861, -356.652376, 421.535876},
    {1183.00, 78.614193, 184.778874, -186.387003, -36.776172},
    {622.00, -180.732815, 335.321713, -316.800070, -145.278396},
    {882.00, -87.676083, -185.138669, 198.296701, -34.744450},
    {547.00, 46.140315, -120.972830, 101.135679, 22.885731},
};

// Table 2: X_A, Y_A.
constexpr LtpTerm kLtpXY[] = {
    {256.75, -819.940624, 81491.287984, 75004.344875, 1558.515853},
    {708.15, -8444.676815, 787.163481, 624.033993, 7774.939698},
    {274.20, 2600.009459, 1251.296102, 1251.136893, -2219.534038},
    {241.45, 2755.175630, -1257.950837, -1102.212834, -2523.969396},
    {2309.00, -167.659835, -2966.799730, -2660.664980, 247.850422},
    {492.20, 871.855056, 639.744522, 699.291817, -846.485643},
    {396.10, 44.769698, 131.600209, 153.167220, -1393.124055},
    {288.90, -512.313065, -445.040117, -950.865637, 368.526116},
    {231.10, -819.415595, 584.522874, 499.754645, 749.045012},
    {1610.00, -538.071099, -89.756563, -145.188210, 444.704518},
    {620.00, -189.793622, 524.429630, 558.116553, 235.934465},
    {157.87, -402.922932, -13.549067, -23.923029, 374.049623},
    {220.30, 179.516345, -210.157124, -165.405086, -171.330180},
    {1200.00, -9.814756, -44.919798, 9.344131, -22.899655},
};

// Table 3: p_A, epsilon_A.
constexpr LtpTerm kLtpPE[] = {
    {409.90, -6908.287473, -2845.175469, 753.872780, -1704.720302},
    {396.15, -3198.706291, 449.844989, -247.805823, -862.308358},
    {537.22, 1453.674527, -1255.915323, 379.471484, 447.832178},
    {402.90, -857.748557, 886.736783, -53.880558, -889.571909},
    {417.15, 1173.231614, 418.887514, -90.109153, 190.402846},
    {288.92, -156.981465, 997.912441, -353.600190, -56.564991},
    {4043.00, 371.836550, -240.979710, -63.115353, -296.222622},
    {306.00, -216.619040, 76.541307, -28.248187, -75.859952},
    {277.00, 193.691479, -36.788069, 17.703387, 67.473503},
    {203.00, 11.891524, -170.964086, 38.911307, 3.014055},
};

// Cubic polynomial coefficients (arcsec, per century^k): {a0, a1, a2, a3}.
constexpr double kLtpP[4] = {5851.607687, -0.1189000, -0.00028913, 101e-9};
constexpr double kLtpQ[4] = {-1600.886300, 1.1689818, -0.00000020, -437e-9};
constexpr double kLtpX[4] = {5453.282155, 0.4252841, -0.00037173, -152e-9};
constexpr double kLtpY[4] = {-73750.930350, -0.7675452, -0.00018725, 231e-9};
constexpr double kLtpPA[4] = {8134.017132, 5043.0520035, -0.00710733, 271e-9};
constexpr double kLtpEps[4] = {84028.206305, 0.3624445, -0.00004039, -110e-9};

// Polynomial plus the periodic terms of one column, arcseconds.
template <size_t N>
void ltp_series(const LtpTerm terms[N], const double a[4], const double b[4], double T,
                double& first, double& second) {
    first = a[0] + T * (a[1] + T * (a[2] + T * a[3]));
    second = b[0] + T * (b[1] + T * (b[2] + T * b[3]));
    for (size_t i = 0; i < N; ++i) {
        const double arg = 2.0 * 3.14159265358979323846 * T / terms[i].period_cy;
        const double c = std::cos(arg), s = std::sin(arg);
        first += terms[i].c1 * c + terms[i].s1 * s;
        second += terms[i].c2 * c + terms[i].s2 * s;
    }
}

} // namespace

void ltp_ecliptic_pole(double jd_tt, double k[3]) {
    double p, q;
    ltp_series<std::size(kLtpPQ)>(kLtpPQ, kLtpP, kLtpQ, centuries(jd_tt), p, q);
    p *= kAs2Rad;
    q *= kAs2Rad;
    const double w = std::sqrt(std::max(1.0 - p * p - q * q, 0.0));
    // (P, -Q, W) in the J2000 ecliptic, rotated to the J2000 equator by
    // the IAU 2006 obliquity at J2000.
    const double eps0 = 84381.406 * kAs2Rad;
    const double s = std::sin(eps0), c = std::cos(eps0);
    k[0] = p;
    k[1] = -q * c - w * s;
    k[2] = -q * s + w * c;
}

void ltp_equator_pole(double jd_tt, double n[3]) {
    double x, y;
    ltp_series<std::size(kLtpXY)>(kLtpXY, kLtpX, kLtpY, centuries(jd_tt), x, y);
    x *= kAs2Rad;
    y *= kAs2Rad;
    n[0] = x;
    n[1] = y;
    n[2] = std::sqrt(std::max(1.0 - x * x - y * y, 0.0));
}

void ltp_mean_equator_of_date_matrix(double jd_tt, double m[9]) {
    double n[3], k[3];
    ltp_equator_pole(jd_tt, n);
    ltp_ecliptic_pole(jd_tt, k);
    double e[3] = {n[1] * k[2] - n[2] * k[1], n[2] * k[0] - n[0] * k[2], n[0] * k[1] - n[1] * k[0]};
    const double len = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    for (double& v : e)
        v /= len;
    const double mid[3] = {n[1] * e[2] - n[2] * e[1], n[2] * e[0] - n[0] * e[2],
                           n[0] * e[1] - n[1] * e[0]};
    for (int j = 0; j < 3; ++j) {
        m[j] = e[j];
        m[3 + j] = mid[j];
        m[6 + j] = n[j];
    }
}

double ltp_mean_obliquity(double jd_tt) {
    double n[3], k[3];
    ltp_equator_pole(jd_tt, n);
    ltp_ecliptic_pole(jd_tt, k);
    const double cx = n[1] * k[2] - n[2] * k[1], cy = n[2] * k[0] - n[0] * k[2],
                 cz = n[0] * k[1] - n[1] * k[0];
    return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz),
                      n[0] * k[0] + n[1] * k[1] + n[2] * k[2]);
}

double ltp_precession_in_longitude_deg(double jd_tt) {
    double pa, eps;
    ltp_series<std::size(kLtpPE)>(kLtpPE, kLtpPA, kLtpEps, centuries(jd_tt), pa, eps);
    return pa / 3600.0;
}

double ltp_obliquity_series(double jd_tt) {
    double pa, eps;
    ltp_series<std::size(kLtpPE)>(kLtpPE, kLtpPA, kLtpEps, centuries(jd_tt), pa, eps);
    return eps * kAs2Rad;
}

// --- Sidereal zodiacs ------------------------------------------------

namespace {

constexpr double kRad2Deg = 180.0 / 3.14159265358979323846;

// IAU 2006 general precession in longitude p_A (Capitaine et al. 2003),
// arcseconds from J2000: the motion of the mean equinox along the
// ecliptic of date.
double precession_in_longitude_arcsec(double T) {
    return 5028.796195 * T + 1.1054348 * T * T + 0.00007964 * T * T * T -
           0.000023856 * T * T * T * T;
}

// Published anchors: the TT epoch of the definition and the MEAN
// ayanamsha there (the true value at the epoch less the nutation in
// longitude of the series above). The instants follow the published
// definitions; the values are the Swiss Ephemeris true ayanamsha at
// those instants (24°02'27.6547" at 1 Jan 1950, 23°15'00.7963" at
// 21 Mar 1956, both 0:00 TT) — the constants traditionally quoted for
// the same instants (24°02'31.36" and 23°15'00".658) differ from the
// Swiss Ephemeris output; see docs/ENGINE.md.
struct AyanAnchor {
    double t0_jtdb;
    double mean0_deg;
};
constexpr AyanAnchor kAyanAnchors[] = {
    {2433282.5, 24.0419327432}, // Fagan/Bradley: 1 Jan 1950
    {2435553.5, 23.2455606650}, // Lahiri: 21 Mar 1956
};

} // namespace

double precession_in_longitude_deg(double jd_tt) {
    return precession_in_longitude_arcsec(centuries(jd_tt)) / 3600.0;
}

Ayanamsa ayanamsa_anchored(double t0_jtdb, double ayan0_mean_deg, double jd_tt) {
    return ayanamsa_anchored(t0_jtdb, ayan0_mean_deg, jd_tt, PrecessionModel::IAU2006);
}

Ayanamsa ayanamsa_anchored(double t0_jtdb, double ayan0_mean_deg, double jd_tt,
                           PrecessionModel model) {
    const double mean = ayan0_mean_deg + precession_in_longitude_deg(jd_tt, model) -
                        precession_in_longitude_deg(t0_jtdb, model);
    double dpsi, deps;
    nutation(jd_tt, dpsi, deps);
    return Ayanamsa{mean, mean + dpsi * kRad2Deg};
}

std::optional<Ayanamsa> ayanamsa(int mode, double jd_tt) {
    return ayanamsa(mode, jd_tt, PrecessionModel::IAU2006);
}

std::optional<AyanamsaAnchor> ayanamsa_anchor(int mode) {
    if (mode < 0 || size_t(mode) >= sizeof kAyanAnchors / sizeof kAyanAnchors[0])
        return std::nullopt;
    return AyanamsaAnchor{kAyanAnchors[mode].t0_jtdb, kAyanAnchors[mode].mean0_deg};
}

std::optional<Ayanamsa> ayanamsa(int mode, double jd_tt, PrecessionModel model) {
    if (mode < 0 || size_t(mode) >= sizeof kAyanAnchors / sizeof kAyanAnchors[0])
        return std::nullopt;
    const AyanAnchor& a = kAyanAnchors[mode];
    return ayanamsa_anchored(a.t0_jtdb, a.mean0_deg, jd_tt, model);
}

double precession_in_longitude_deg(double jd_tt, PrecessionModel model) {
    return model == PrecessionModel::Vondrak2011 ? ltp_precession_in_longitude_deg(jd_tt)
                                                 : precession_in_longitude_deg(jd_tt);
}

double mean_obliquity(double jd_tt, PrecessionModel model) {
    return model == PrecessionModel::Vondrak2011 ? ltp_mean_obliquity(jd_tt)
                                                 : mean_obliquity(jd_tt);
}

void mean_equator_of_date_matrix(double jd_tt, PrecessionModel model, double m[9]) {
    if (model == PrecessionModel::Vondrak2011)
        ltp_mean_equator_of_date_matrix(jd_tt, m);
    else
        mean_equator_of_date_matrix(jd_tt, m);
}

void frame_bias_matrix(double m[9]) {
    // B = R1(-eta0) R2(xi0) R3(d_alpha0).
    const double da0 = -0.01460 * kAs2Rad;
    const double xi0 = -0.0166170 * kAs2Rad;
    const double eta0 = -0.0068192 * kAs2Rad;
    double r1[9], r2[9], r3[9], t[9];
    rot1(-eta0, r1);
    rot2(xi0, r2);
    rot3(da0, r3);
    matmul(r1, r2, t);
    matmul(t, r3, m);
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
    double dpsi, deps;
    nutation(jd_tt, dpsi, deps);
    return equation_of_equinoxes_rad(jd_tt, dpsi, mean_obliquity(jd_tt));
}

double equation_of_equinoxes_rad(double jd_tt, double dpsi, double eps_mean) {
    const double T = centuries(jd_tt);
    double phi[14];
    fundamental_arguments(jd_tt, phi);
    const double& om = phi[13];
    const double& f = phi[11];
    const double& d = phi[12];
    // Circular 179 eq. 2.14, arcseconds.
    const double as =
        dpsi / kAs2Rad * std::cos(eps_mean) + 0.00264096 * std::sin(om) +
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

double gast_rad(double jd_ut1, double jd_tt, double dpsi, double eps_mean) {
    return gmst_rad(jd_ut1, jd_tt) + equation_of_equinoxes_rad(jd_tt, dpsi, eps_mean);
}

double ut1_from_sidereal_time(double jd_ut1_guess, double jd_tt, double last_rad,
                              double site_lon_rad) {
    constexpr double kTwoPi = 6.283185307179586476925287;
    constexpr double kSiderealPerSolar = 1.00273781191135448;
    double jd_ut1 = jd_ut1_guess;
    for (int k = 0; k < 4; ++k) {
        const double here = gast_rad(jd_ut1, jd_tt) + site_lon_rad;
        jd_ut1 += std::remainder(last_rad - here, kTwoPi) / (kTwoPi * kSiderealPerSolar);
    }
    return jd_ut1;
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

// ---------------------------------------------------------------------------
// NutationInterpolator
// ---------------------------------------------------------------------------

namespace {
constexpr size_t kNutationCacheSize = 4096; // a power of two
} // namespace

struct NutationInterpolator::Node {
    long long index = std::numeric_limits<long long>::min();
    double n[6] = {0, 0, 0, 0, 0, 0};
};

NutationInterpolator::NutationInterpolator() : nodes_(new Node[kNutationCacheSize]) {}

NutationInterpolator::~NutationInterpolator() {
    delete[] nodes_;
}

const NutationInterpolator::Node& NutationInterpolator::node(long long index) {
    Node& slot = nodes_[size_t(index) & (kNutationCacheSize - 1)];
    if (slot.index != index) {
        slot.index = index;
        nutation_with_rates(double(index) * kNodeSpacingDays, slot.n);
        ++evaluations_;
    }
    return slot;
}

void NutationInterpolator::at(double jd_tt, double& dpsi, double& deps) {
    // Node k sits at k * 0.5 day, exactly representable; u in [0, 1).
    const double k = std::floor(jd_tt / kNodeSpacingDays);
    const auto index = static_cast<long long>(k);
    const double u = (jd_tt - k * kNodeSpacingDays) / kNodeSpacingDays;
    // Both nodes are read before either reference could be invalidated by
    // the other's lookup (they are different slots, or the same index).
    double a[6], b[6];
    std::copy(node(index).n, node(index).n + 6, a);
    std::copy(node(index + 1).n, node(index + 1).n + 6, b);
    const double h = kNodeSpacingDays;
    const double u2 = u * u, u3 = u2 * u, u4 = u3 * u, u5 = u4 * u;
    // Quintic Hermite basis on [0, 1]: value, first and second derivative
    // at each end.
    const double h00 = 1 - 10 * u3 + 15 * u4 - 6 * u5, h01 = 10 * u3 - 15 * u4 + 6 * u5;
    const double h10 = u - 6 * u3 + 8 * u4 - 3 * u5, h11 = -4 * u3 + 7 * u4 - 3 * u5;
    const double h20 = 0.5 * (u2 - 3 * u3 + 3 * u4 - u5), h21 = 0.5 * (u3 - 2 * u4 + u5);
    const auto interp = [&](int c) {
        return h00 * a[c] + h01 * b[c] + h * (h10 * a[c + 2] + h11 * b[c + 2]) +
               h * h * (h20 * a[c + 4] + h21 * b[c + 4]);
    };
    dpsi = interp(0);
    deps = interp(1);
}

} // namespace prometheia::frames
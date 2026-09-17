// SPDX-License-Identifier: GPL-2.0-or-later
//
// Frame-transformation tests. Structure checks (orthogonality, known
// argument values at J2000) run everywhere; the differential fixtures
// compare our IAU 2006 + full IAU 2000A chain against values produced by
// the locally installed Swiss Ephemeris (DE441-derived data) as an
// output-only oracle. Expected numbers were generated once by querying
// swe_calc with SEFLG_J2000|SEFLG_NONUT|SEFLG_TRUEPOS|SEFLG_NOABERR
// (geometric J2000 mean ecliptic), the same minus SEFLG_J2000 (mean
// ecliptic of date), and SEFLG_TRUEPOS|SEFLG_NOABERR alone (true
// ecliptic of date). Measured agreement is 0.0005 arcsec: SWE's default
// nutation is the 1 mas-class truncated series, we run the full
// 1365-term IAU 2000A.
#include <cmath>
#include <cstdio>

#include <prometheia/frames.hpp>

#include <doctest/doctest.h>

using namespace prometheia;
using namespace prometheia::frames;

namespace {

constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
constexpr double kRad2Deg = 180.0 / 3.14159265358979323846;

bool near(double a, double b, double tol) {
    return std::fabs(a - b) <= tol;
}

void vec_from_lonlat(double lon_deg, double lat_deg, double dist, double v[3]) {
    const double lo = lon_deg * kDeg2Rad, la = lat_deg * kDeg2Rad;
    const double cb = std::cos(la);
    v[0] = dist * cb * std::cos(lo);
    v[1] = dist * cb * std::sin(lo);
    v[2] = dist * std::sin(la);
}

// The fixtures are geometric J2000 *mean ecliptic* longitudes/latitudes;
// our matrices take ICRF (equatorial) vectors, so rotate about x by the
// J2000 mean obliquity (frame bias, ~0.02", is far below test tolerance).
void j2000_ecl_to_icrf(double v[3]) {
    const double eps = mean_obliquity(2451545.0);
    const double c = std::cos(eps), s = std::sin(eps);
    const double y = v[1], z = v[2];
    v[1] = c * y - s * z;
    v[2] = s * y + c * z;
}

void lonlat_from_vec(const double v[3], double& lon_deg, double& lat_deg) {
    lon_deg = std::atan2(v[1], v[0]) * kRad2Deg;
    if (lon_deg < 0.0)
        lon_deg += 360.0;
    lat_deg = std::atan2(v[2], std::hypot(v[0], v[1])) * kRad2Deg;
}

void apply(const double m[9], const double v[3], double out[3]) {
    for (int i = 0; i < 3; ++i) {
        out[i] = m[3 * i] * v[0] + m[3 * i + 1] * v[1] + m[3 * i + 2] * v[2];
    }
}

double lon_diff_deg(double a, double b) {
    double d = a - b;
    while (d > 180.0)
        d -= 360.0;
    while (d < -180.0)
        d += 360.0;
    return d;
}

bool orthogonal(const double m[9]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double dot = 0.0;
            for (int k = 0; k < 3; ++k)
                dot += m[3 * i + k] * m[3 * j + k];
            if (!near(dot, i == j ? 1.0 : 0.0, 1e-12))
                return false;
        }
    }
    return true;
}

// Fixtures from the installed Swiss Ephemeris (see file comment).
struct Fixture {
    double jd_tt;
    double lon_j2000, lat_j2000, dist; // geometric J2000 mean ecliptic
    double lon_mean, lat_mean;         // mean ecliptic of date
    double lon_true, lat_true;         // true ecliptic of date
    const char* name;
};

constexpr Fixture kFixtures[] = {
    {2451545.0, 280.37782479102, 0.00022732336, 0.983327677678, 280.37782479102, 0.00022732336,
     280.37395492188, 0.00022732336, "SUN"},
    {2451545.0, 223.31892706869, 5.17086922904, 0.002690202993, 223.31892706869, 5.17086922904,
     223.31505719955, 5.17086922904, "MOON"},
    {2461443.0, 317.08384623694, 0.00218856863, 0.986109860060, 317.46241481263, 0.00002427020,
     317.46582559256, 0.00002427020, "SUN"},
    {2461443.0, 315.26830270879, -0.44886547124, 0.002694159673, 315.64689276253, -0.45111732109,
     315.65030354246, -0.45111732109, "MOON"},
};

} // namespace

TEST_CASE("frames_fundamental_arguments_j2000") {
    double phi[14];
    fundamental_arguments(2451545.0, phi);
    const double deg = 180.0 / 3.14159265358979323846;
    // Delaunay arguments at J2000 (degrees): l, l', F, D, Omega.
    CHECK(near(phi[9] * deg, 134.96340, 1e-4));
    CHECK(near(phi[10] * deg, 357.52911, 1e-4));
    CHECK(near(phi[11] * deg, 93.27209, 1e-4));
    CHECK(near(phi[12] * deg, 297.85019, 1e-4));
    CHECK(near(phi[13] * deg, 125.04452, 1e-4));
    // Earth mean heliocentric longitude.
    CHECK(near(phi[2] * deg, 100.46646, 1e-4));
    // All within [0, 2pi).
    for (int j = 0; j < 14; ++j) {
        CHECK((phi[j] >= 0.0 && phi[j] < 2.0 * 3.14159265358979323846 + 1e-15));
    }
}

TEST_CASE("frames_nutation_values") {
    double dpsi, deps;
    nutation(2451545.0, dpsi, deps);
    // Full-series values at J2000; the SWE differential (true-minus-mean
    // ecliptic longitude) at the same epoch is -13.9315".
    CHECK(near(dpsi * kRad2Deg * 3600.0, -13.93200, 0.005));
    CHECK(near(deps * kRad2Deg * 3600.0, -5.76940, 0.005));
    // 2027 epoch: SWE differential is +12.27881".
    nutation(2461443.0, dpsi, deps);
    CHECK(near(dpsi * kRad2Deg * 3600.0, 12.27881, 0.005));
    CHECK(std::fabs(deps * kRad2Deg * 3600.0) < 10.0);
}

TEST_CASE("frames_obliquity") {
    // IAU 2006: 84381.406" at J2000 (swetest prints 23d26'21.4060").
    CHECK(near(mean_obliquity(2451545.0), 84381.406 * kDeg2Rad / 3600.0, 1e-12));
}

TEST_CASE("frames_matrices_are_rotations") {
    const double epochs[] = {2305424.5, 2415020.0, 2451545.0, 2461443.0, 2513392.5};
    for (double jd : epochs) {
        double m[9];
        mean_equator_of_date_matrix(jd, m);
        CHECK(orthogonal(m));
        mean_ecliptic_of_date_matrix(jd, m);
        CHECK(orthogonal(m));
        true_equator_of_date_matrix(jd, m);
        CHECK(orthogonal(m));
        true_ecliptic_of_date_matrix(jd, m);
        CHECK(orthogonal(m));
    }
    // Precession vanishes at J2000.
    double p[9];
    mean_equator_of_date_matrix(2451545.0, p);
    for (int i = 0; i < 9; ++i) {
        CHECK(near(p[i], (i == 0 || i == 4 || i == 8) ? 1.0 : 0.0, 1e-10));
    }
}

TEST_CASE("frames_mean_chain_matches_swe") {
    for (const Fixture& f : kFixtures) {
        double v[3];
        vec_from_lonlat(f.lon_j2000, f.lat_j2000, f.dist, v);
        j2000_ecl_to_icrf(v);
        double m[9];
        mean_ecliptic_of_date_matrix(f.jd_tt, m);
        double w[3];
        apply(m, v, w);
        double lon, lat;
        lonlat_from_vec(w, lon, lat);
        const double dlon = lon_diff_deg(lon, f.lon_mean) * 3600.0;
        const double dlat = (lat - f.lat_mean) * 3600.0;
        CHECK(std::fabs(dlon) < 0.01);
        CHECK(std::fabs(dlat) < 0.01);
        std::printf("  %s jd=%.0f mean: dlon=%+.5f\" dlat=%+.5f\"\n", f.name, f.jd_tt, dlon, dlat);
    }
}

TEST_CASE("frames_true_chain_matches_swe") {
    for (const Fixture& f : kFixtures) {
        double v[3];
        vec_from_lonlat(f.lon_j2000, f.lat_j2000, f.dist, v);
        j2000_ecl_to_icrf(v);
        double m[9];
        true_ecliptic_of_date_matrix(f.jd_tt, m);
        double w[3];
        apply(m, v, w);
        double lon, lat;
        lonlat_from_vec(w, lon, lat);
        const double dlon = lon_diff_deg(lon, f.lon_true) * 3600.0;
        const double dlat = (lat - f.lat_true) * 3600.0;
        CHECK(std::fabs(dlon) < 0.01);
        CHECK(std::fabs(dlat) < 0.01);
        std::printf("  %s jd=%.0f true: dlon=%+.5f\" dlat=%+.5f\"\n", f.name, f.jd_tt, dlon, dlat);
    }
}

TEST_CASE("frames_nutation_shift_property") {
    // Nutation shifts apparent longitudes by dpsi and leaves latitudes
    // unchanged: the defining property of the true-ecliptic frame.
    const double epochs[] = {2305424.5, 2451545.0, 2461443.0, 2513392.5};
    for (double jd : epochs) {
        double dpsi, deps;
        nutation(jd, dpsi, deps);
        const double v[3] = {0.3, -0.8, 0.52}; // arbitrary unit vector
        double mm9[9], tm9[9], wm[3], wt[3];
        mean_ecliptic_of_date_matrix(jd, mm9);
        true_ecliptic_of_date_matrix(jd, tm9);
        apply(mm9, v, wm);
        apply(tm9, v, wt);
        double lonm, latm, lont, latt;
        lonlat_from_vec(wm, lonm, latm);
        lonlat_from_vec(wt, lont, latt);
        CHECK(near(lon_diff_deg(lont, lonm) * kDeg2Rad, dpsi, 1e-12));
        CHECK(near(latt - latm, 0.0, 1e-9));
    }
}

TEST_CASE("frames_sidereal_time") {
    // ERA is a function of UT1 alone; at the J2000 instant, 0.779... turns.
    const double era = earth_rotation_angle(2451545.0);
    CHECK(near(era, 0.7790572732640 * 2.0 * 3.14159265358979323846, 1e-9));

    // GAST fixture: swe_sidtime(jd_ut1 = 2461442.7978) = 16.22733953801
    // hours (SWE returns hours) = 243.4100930715 deg. Measured agreement
    // of the full IAU 2000A chain: 0.0004".
    const double jd_ut1 = 2461442.7978;
    const double jd_tt = jd_ut1 + 69.5 / 86400.0; // dT ~69 s, only T depends on it
    const double gast_deg = gast_rad(jd_ut1, jd_tt) * kRad2Deg;
    const double swe_deg = 16.22733953801 * 15.0;
    CHECK(near(gast_deg, swe_deg, 0.01 / 3600.0));
    std::printf("  GAST ours=%.7f deg, SWE=%.7f deg, diff=%+.4f\"\n", gast_deg, swe_deg,
                (gast_deg - swe_deg) * 3600.0);

    // Equation of the equinoxes is dominated by dpsi cos(eps).
    double dpsi, deps;
    nutation(jd_tt, dpsi, deps);
    const double eq = equation_of_equinoxes_rad(jd_tt);
    CHECK(near(eq, dpsi * std::cos(mean_obliquity(jd_tt)), 2.5e-6));
}

TEST_CASE("frames_observer_geocentric") {
    // WGS84: equatorial radius 6378.137 km, polar 6356.752 km.
    double out[3];
    observer_geocentric(GeoSite{0.0, 0.0, 0.0}, 0.0, out);
    CHECK(near(out[0], 6378.137, 1e-3));
    CHECK(near(out[1], 0.0, 1e-9));
    CHECK(near(out[2], 0.0, 1e-9));
    // A quarter turn east puts the prime meridian on the +y axis.
    observer_geocentric(GeoSite{0.0, 0.0, 0.0}, 3.14159265358979323846 / 2.0, out);
    CHECK(near(out[0], 0.0, 1e-9));
    CHECK(near(out[1], 6378.137, 1e-3));
    // At the pole the geocentric distance is the polar radius.
    observer_geocentric(GeoSite{0.0, 3.14159265358979323846 / 2.0, 0.0}, 0.0, out);
    CHECK(near(out[0], 0.0, 1e-9));
    CHECK(near(out[1], 0.0, 1e-9));
    CHECK(near(out[2], 6356.752, 1e-3));
    // Longitude rotates with the site, not the sidereal time.
    observer_geocentric(GeoSite{3.14159265358979323846 / 2.0, 0.0, 0.0}, 0.0, out);
    CHECK(near(out[0], 0.0, 1e-9));
    CHECK(near(out[1], 6378.137, 1e-3));
}

namespace {

// Largest angle (arcsec) between corresponding rows of two rotation
// matrices: how far apart the two frames' axes are.
double frame_offset_arcsec(const double a[9], const double b[9]) {
    double worst = 0.0;
    for (int r = 0; r < 3; ++r) {
        const double* u = a + 3 * r;
        const double* v = b + 3 * r;
        const double c[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                             u[0] * v[1] - u[1] * v[0]};
        const double s = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        worst = std::max(worst, std::atan2(s, u[0] * v[0] + u[1] * v[1] + u[2] * v[2]));
    }
    return worst * 206264.80624709636;
}

} // namespace

TEST_CASE("ltp_series_reduce_to_j2000") {
    // Every Vondrak et al. (2011) series is constrained to IAU 2006 at J2000:
    // P_A = Q_A = X_A = Y_A = p_A = 0 and epsilon_A = 84381.406". The
    // polynomial constants cancel the periodic cosine amplitudes to the
    // published 6-decimal rounding (1e-6") — which also pins the
    // corrigendum's Q_A C7 = 198.296701 (the originally printed 198.296071
    // leaves 0.0006").
    constexpr double kJ2000 = 2451545.0;
    const double eps0 = 84381.406 / 206264.80624709636;
    double k[3], n[3];
    ltp_ecliptic_pole(kJ2000, k);
    ltp_equator_pole(kJ2000, n);
    constexpr double kRoundingArcsec = 5e-6;
    constexpr double kRoundingRad = kRoundingArcsec / 206264.80624709636;
    CHECK(std::fabs(k[0]) < kRoundingRad);
    CHECK(std::fabs(k[1] + std::sin(eps0)) < kRoundingRad);
    CHECK(std::fabs(k[2] - std::cos(eps0)) < kRoundingRad);
    CHECK(std::fabs(n[0]) < kRoundingRad);
    CHECK(std::fabs(n[1]) < kRoundingRad);
    CHECK(std::fabs(ltp_precession_in_longitude_deg(kJ2000)) * 3600.0 < kRoundingArcsec);
    CHECK(std::fabs(ltp_obliquity_series(kJ2000) - eps0) * 206264.80624709636 < kRoundingArcsec);
    CHECK(std::fabs(ltp_mean_obliquity(kJ2000) - eps0) * 206264.80624709636 < kRoundingArcsec);
    double m[9];
    ltp_mean_equator_of_date_matrix(kJ2000, m);
    const double identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    CHECK(frame_offset_arcsec(m, identity) < kRoundingArcsec);
}

TEST_CASE("ltp_agrees_with_iau2006_near_j2000") {
    // Within centuries of J2000 the long-term model reproduces IAU 2006
    // (it was fitted with overwhelming weight there); it departs slowly
    // beyond. Offsets printed for the record, gated loosely.
    std::printf("  %8s %14s %14s %14s\n", "years", "matrix", "obliquity", "p_A");
    for (double years :
         {-1000.0, -500.0, -200.0, -100.0, -10.0, 10.0, 100.0, 200.0, 500.0, 1000.0}) {
        const double jd = 2451545.0 + years * 365.25;
        double a[9], b[9];
        mean_equator_of_date_matrix(jd, a);
        ltp_mean_equator_of_date_matrix(jd, b);
        const double d_mat = frame_offset_arcsec(a, b);
        const double d_eps = (ltp_mean_obliquity(jd) - mean_obliquity(jd)) * 206264.80624709636;
        const double d_pa =
            (ltp_precession_in_longitude_deg(jd) - precession_in_longitude_deg(jd)) * 3600.0;
        std::printf("  %+8.0f %13.5f\" %13.5f\" %13.5f\"\n", years, d_mat, d_eps, d_pa);
        const double gate =
            std::fabs(years) <= 100.0 ? 0.002 : (std::fabs(years) <= 200.0 ? 0.01 : 1.0);
        CHECK(d_mat < gate);
        CHECK(std::fabs(d_eps) < gate);
        CHECK(std::fabs(d_pa) < gate);
    }
}

TEST_CASE("ltp_long_range_is_a_rotation") {
    // Orthonormal across the model's +-200 millennia. The pole-angle
    // obliquity (what the matrix implies, and what the engine uses) follows
    // the separately fitted epsilon_A series to a few arcseconds within
    // +-4000 years; beyond, the two published fits drift apart (hundreds of
    // arcseconds at +-200 millennia), which is reported, not gated.
    for (double cy : {-2000.0, -500.0, -130.0, -40.0, 40.0, 130.0, 500.0, 2000.0}) {
        const double jd = 2451545.0 + cy * 36525.0;
        double m[9];
        ltp_mean_equator_of_date_matrix(jd, m);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                const double dot =
                    m[3 * i] * m[3 * j] + m[3 * i + 1] * m[3 * j + 1] + m[3 * i + 2] * m[3 * j + 2];
                CHECK(std::fabs(dot - (i == j ? 1.0 : 0.0)) < 1e-14);
            }
        const double d = (ltp_mean_obliquity(jd) - ltp_obliquity_series(jd)) * 206264.80624709636;
        std::printf("  T=%+6.0f cy: pole-angle obliquity - epsilon_A series %.3f\"\n", cy, d);
        if (std::fabs(cy) <= 40.0)
            CHECK(std::fabs(d) < 5.0);
    }
}

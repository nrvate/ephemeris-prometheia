// SPDX-License-Identifier: GPL-2.0-or-later
//
// Apparent magnitudes, radii and poles (prometheia/phenomena.hpp), and
// Engine::phenomena (docs/PHENOMENA.md).
#include "prometheia/phenomena.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

#include "prometheia/engine.hpp"
#include "prometheia/stars.hpp"

namespace prometheia {

namespace phenomena {

namespace {

constexpr double kDeg = 3.14159265358979323846 / 180.0;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kJ2000 = 2451545.0;

double poly(double x, std::initializer_list<double> c) {
    double sum = 0.0, p = 1.0;
    for (double k : c) {
        sum += k * p;
        p *= x;
    }
    return sum;
}

Magnitude none(std::string_view why) {
    return {kNaN, why};
}

} // namespace

double mean_radius_km(long naif) {
    // The Sun: IAU 2015 Resolution B3's nominal radius. The others: the mean
    // radii of Archinal et al. 2018. Mars to Pluto answer as barycentres;
    // the disc is the planet's.
    switch (naif) {
    case 10:
        return 695700.0;
    case 1:
    case 199:
        return 2439.4;
    case 2:
    case 299:
        return 6051.8;
    case 399:
        return 6371.0084;
    case 301:
        return 1737.4;
    case 4:
    case 499:
        return 3389.5;
    case 5:
    case 599:
        return 69911.0;
    case 6:
    case 699:
        return 58232.0;
    case 7:
    case 799:
        return 25362.0;
    case 8:
    case 899:
        return 24622.0;
    case 9:
    case 999:
        return 1188.3;
    default:
        return 0.0;
    }
}

bool pole_icrf(long naif, double jd_tdb, double out[3]) {
    // Archinal et al. 2018, Table 1 (pck00011.tpc's BODY699_POLE_RA/DEC and
    // BODY799_POLE_RA/DEC): right ascension and declination of the north
    // pole, T in Julian centuries TDB from J2000.
    const double T = (jd_tdb - kJ2000) / 36525.0;
    double ra = 0.0, dec = 0.0;
    switch (naif) {
    case 6:
    case 699:
        ra = 40.589 - 0.036 * T;
        dec = 83.537 - 0.004 * T;
        break;
    case 7:
    case 799:
        ra = 257.311;
        dec = -15.175;
        break;
    default:
        return false;
    }
    out[0] = std::cos(dec * kDeg) * std::cos(ra * kDeg);
    out[1] = std::cos(dec * kDeg) * std::sin(ra * kDeg);
    out[2] = std::sin(dec * kDeg);
    return true;
}

Magnitude body_magnitude(long naif, const MagnitudeInputs& in) {
    const double a = in.phase_angle_deg;
    const double rd = 5.0 * std::log10(in.r_au * in.d_au);
    if (!(in.r_au > 0.0 && in.d_au > 0.0) || !(a >= 0.0 && a <= 180.0))
        return none("no magnitude: no distance or phase angle");
    switch (naif) {
    case 10:
        // Willmer 2018, Table 3: V = -26.76 (Vega) at 1 au.
        return {-26.76 + 5.0 * std::log10(in.d_au), "Willmer 2018"};
    case 301: {
        // The formula rests on whole-disc photometry over 6-120 deg of phase
        // (Lane & Irvine 1973, AJ 78, 267); beyond, nothing constrains the
        // quartic, which brightens toward new moon. No number is given there.
        if (a > 120.0)
            return none("no magnitude: Moon beyond 120 deg (Lane & Irvine 1973)");
        // Krisciunas & Schaefer 1991 eq. 9, at the mean distance 384,400 km
        // and 1 au; scaled by the inverse squares of the actual ones.
        const double mean_au = 384400.0 / 149597870.7;
        return {-12.73 + 0.026 * a + 4e-9 * std::pow(a, 4) +
                    5.0 * std::log10(in.r_au * in.d_au / mean_au),
                "Krisciunas & Schaefer 1991 eq. 9"};
    }
    case 1:
    case 199:
        return {rd + poly(a, {-0.613, 6.3280E-02, -1.6336E-03, 3.3644E-05, -3.4265E-07, 1.6893E-09,
                              -3.0334E-12}),
                "Mallama & Hilton 2018 eq. 2"};
    case 2:
    case 299:
        if (a <= 163.7)
            return {rd + poly(a, {-4.384, -1.044E-03, 3.687E-04, -2.814E-06, 8.938E-09}),
                    "Mallama & Hilton 2018 eq. 3"};
        if (a < 179.0)
            return {rd + poly(a, {236.05828, -2.81914E+00, 8.39034E-03}),
                    "Mallama & Hilton 2018 eq. 4"};
        return none("no magnitude: Venus beyond 179 deg (Mallama & Hilton 2018)");
    case 399:
        return {rd + poly(a, {-3.99, -1.060E-3, 2.054E-4}), "Mallama & Hilton 2018 eq. 5"};
    case 4:
    case 499:
        // The L(lambda_e) and L(Ls) terms are left out (docs/PHENOMENA.md).
        if (a <= 50.0)
            return {rd + poly(a, {-1.601, 0.02267, -0.0001302}), "Mallama & Hilton 2018 eq. 6"};
        return {rd + poly(a, {-0.367, -0.02573, 0.0003445}), "Mallama & Hilton 2018 eq. 7"};
    case 5:
    case 599: {
        if (a <= 12.0)
            return {rd + poly(a, {-9.395, -3.7E-04, 6.16E-04}), "Mallama & Hilton 2018 eq. 8"};
        if (a < 130.0) {
            const double x = a / 180.0;
            return {rd - 9.428 -
                        2.5 * std::log10(poly(x, {1.0, -1.507, -0.363, -0.062, 2.809, -1.876})),
                    "Mallama & Hilton 2018 eq. 9"};
        }
        return none("no magnitude: Jupiter beyond 130 deg (Mallama & Hilton 2018)");
    }
    case 6:
    case 699: {
        // The rings' effective inclination: the geometric mean of the
        // observer's and the Sun's latitudes over the ring plane when they
        // are on the same side, else 0 (the rings backlit).
        const double be = in.sub_observer_lat_deg, bs = in.sub_solar_lat_deg;
        const double beta = be * bs > 0.0 ? std::sqrt(be * bs) : 0.0;
        if (a <= 6.5 && beta <= 27.0) {
            const double sb = std::sin(beta * kDeg);
            return {rd - 8.914 - 1.825 * sb + 0.026 * a - 0.378 * sb * std::exp(-2.25 * a),
                    "Mallama & Hilton 2018 eq. 10"};
        }
        if (a > 6.0 && a < 150.0)
            return {rd + poly(a, {-8.94, 2.446E-4, 2.672E-4, -1.505E-6, 4.767E-9}),
                    "Mallama & Hilton 2018 eq. 12 (globe only)"};
        return none("no magnitude: Saturn beyond 150 deg (Mallama & Hilton 2018)");
    }
    case 7:
    case 799: {
        // Planetographic latitudes (eq. 13, flattening 0.0022927), averaged
        // in absolute value.
        const double f = 0.0022927;
        const auto graphic = [&](double lat) {
            return std::atan(std::tan(lat * kDeg) / ((1.0 - f) * (1.0 - f))) / kDeg;
        };
        const double phi = 0.5 * (std::fabs(graphic(in.sub_observer_lat_deg)) +
                                  std::fabs(graphic(in.sub_solar_lat_deg)));
        if (a <= 3.1)
            return {rd - 7.110 - 8.4E-04 * phi, "Mallama & Hilton 2018 eq. 14"};
        if (a <= 154.0)
            return {rd - 7.110 - 8.4E-04 * phi + 6.587E-3 * a + 1.045E-4 * a * a,
                    "Mallama & Hilton 2018 eq. 15"};
        return none("no magnitude: Uranus beyond 154 deg (Mallama & Hilton 2018)");
    }
    case 8:
    case 899: {
        const double year = 2000.0 + (in.jd_tt - kJ2000) / 365.25;
        if (a <= 1.9) {
            const double v10 = year < 1980.0    ? -6.89
                               : year <= 2000.0 ? -6.89 - 0.0054 * (year - 1980.0)
                                                : -7.00;
            return {rd + v10, "Mallama & Hilton 2018 eq. 16"};
        }
        if (year > 2000.0 && a <= 133.0)
            return {rd - 7.00 + 7.944E-3 * a + 9.617E-5 * a * a, "Mallama & Hilton 2018 eq. 17"};
        return none("no magnitude: Neptune outside Mallama & Hilton 2018");
    }
    default:
        return none("no magnitude model for this body");
    }
}

Magnitude hg_magnitude(double h, double g, double r_au, double d_au, double phase_angle_deg) {
    if (!(r_au > 0.0 && d_au > 0.0) || !std::isfinite(h) || !std::isfinite(g))
        return none("no magnitude: no H, G or distance");
    const double t = std::tan(0.5 * phase_angle_deg * kDeg);
    const double phi1 = std::exp(-3.33 * std::pow(t, 0.63));
    const double phi2 = std::exp(-1.87 * std::pow(t, 1.22));
    return {h + 5.0 * std::log10(r_au * d_au) - 2.5 * std::log10((1.0 - g) * phi1 + g * phi2),
            "H, G (Bowell et al. 1989)"};
}

} // namespace phenomena

namespace {

double angle_between(const double a[3], const double b[3]) {
    // atan2 of the cross and dot products: exact at every separation,
    // where acos loses half its digits near 0 and 180 deg.
    const double c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                         a[0] * b[1] - a[1] * b[0]};
    const double cross = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return std::atan2(cross, dot) * 180.0 / 3.14159265358979323846;
}

double norm(const double v[3]) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

} // namespace

Result<Engine::Phenomena> Engine::phenomena(const ObjectRef& object, double jd_tt,
                                            const CalcOptions& opts) {
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    // Vectors in ICRF axes, so that a pole (ICRF) can be applied; the
    // options' observer and corrections stand, and the frame does not change
    // an angle.
    CalcOptions o = opts;
    o.frame = Frame::ICRF;
    o.coords = Coords::Equatorial;
    o.sidereal = SiderealMode::Tropical;
    o.speed = false;
    o.sigma = false;
    auto body = calc_object(object, jd_tt, o);
    if (!body)
        return body.error();
    const double* p = body.value().pos.xyz_au;
    double s[3] = {0.0, 0.0, 0.0};
    const bool observer_is_sun =
        o.center == Center::Heliocentric || (o.center == Center::Body && o.center_body == 10);
    const bool is_sun = object.kind == ObjectRef::Kind::Body && object.id == 10;
    if (!observer_is_sun) {
        auto sun = calc(10, jd_tt, o);
        if (!sun)
            return sun.error();
        for (int k = 0; k < 3; ++k)
            s[k] = sun.value().pos.xyz_au[k];
    }
    Phenomena out;
    const double d = norm(p);
    // The phase angle is the angle at the body when the light left it:
    // toward the observer at the instant of observation (the light-time
    // position), toward the Sun at the same retarded instant. Aberration and
    // deflection bend the observer's view, not the angle at the body.
    const bool point = object.kind == ObjectRef::Kind::OrbitPoint ||
                       object.kind == ObjectRef::Kind::Star; // no phase
    double to_sun[3] = {s[0] - p[0], s[1] - p[1], s[2] - p[2]};
    double to_observer[3] = {-p[0], -p[1], -p[2]};
    if (!point && !is_sun) {
        CalcOptions lt = o;
        lt.deflection = false;
        lt.aberration = false;
        auto seen = calc_object(object, jd_tt, lt);
        if (!seen)
            return seen.error();
        const double tau = seen.value().provenance.light_time_days;
        CalcOptions bary = CalcOptions::geometric();
        bary.center = Center::Barycentric;
        bary.frame = Frame::ICRF;
        bary.coords = Coords::Equatorial;
        bary.speed = false;
        bary.sigma = false;
        auto b = calc_object(object, jd_tt - tau, bary);
        if (!b)
            return b.error();
        auto sun = calc(10, jd_tt - tau, bary);
        if (!sun)
            return sun.error();
        for (int k = 0; k < 3; ++k) {
            to_sun[k] = sun.value().pos.xyz_au[k] - b.value().pos.xyz_au[k];
            to_observer[k] = -seen.value().pos.xyz_au[k];
        }
    }
    const double r = norm(to_sun);
    out.elongation_deg = observer_is_sun ? kNaN : is_sun ? 0.0 : angle_between(p, s);
    if (is_sun)
        out.phase_angle_deg = 0.0;
    else if (!point)
        out.phase_angle_deg = angle_between(to_sun, to_observer);
    out.illuminated_fraction =
        0.5 * (1.0 + std::cos(out.phase_angle_deg * 3.14159265358979323846 / 180.0));

    constexpr double kAuKm = 149597870.7;
    const auto disc = [&](double radius_km) {
        return radius_km > 0.0 && d > 0.0
                   ? 2.0 * std::asin(std::min(1.0, radius_km / (d * kAuKm))) * 180.0 /
                         3.14159265358979323846 * 3600.0
                   : kNaN;
    };
    switch (object.kind) {
    case ObjectRef::Kind::Body: {
        out.diameter_arcsec = disc(phenomena::mean_radius_km(object.id));
        phenomena::MagnitudeInputs in;
        in.r_au = is_sun ? 1.0 : r;
        in.d_au = d;
        in.phase_angle_deg = out.phase_angle_deg;
        in.jd_tt = jd_tt;
        double pole[3];
        if (phenomena::pole_icrf(object.id, jd_tt, pole)) {
            const auto lat = [&](const double v[3]) {
                const double n = norm(v);
                return std::asin((pole[0] * v[0] + pole[1] * v[1] + pole[2] * v[2]) / n) * 180.0 /
                       3.14159265358979323846;
            };
            in.sub_observer_lat_deg = lat(to_observer);
            in.sub_solar_lat_deg = lat(to_sun);
        }
        phenomena::Magnitude m;
        if (object.id >= 1000000) { // a catalog body
            auto cb = catalog_body(int(object.id));
            if (!cb)
                return cb.error();
            const catalog::Record& rec = cb.value().record;
            if (rec.flags & catalog::RecordFlags::kDiameter)
                out.diameter_arcsec = disc(0.5 * double(rec.diameter_km));
            m = (rec.flags & catalog::RecordFlags::kHg)
                    ? phenomena::hg_magnitude(rec.h_mag, rec.g_slope, r, d, out.phase_angle_deg)
                    : phenomena::Magnitude{kNaN, "no magnitude: the record carries no H"};
        } else {
            m = phenomena::body_magnitude(object.id, in);
        }
        out.magnitude = m.v;
        out.magnitude_model = m.model;
        break;
    }
    case ObjectRef::Kind::Star: {
        out.magnitude = stars::at(size_t(object.id)).vmag;
        out.magnitude_model = "the star catalog's V";
        break;
    }
    case ObjectRef::Kind::Hypothetical:
        out.magnitude_model = "no magnitude: a hypothetical body";
        break;
    case ObjectRef::Kind::OrbitPoint:
        out.magnitude_model = "no magnitude: a point";
        break;
    }
    return out;
}

} // namespace prometheia

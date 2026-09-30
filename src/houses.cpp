// SPDX-License-Identifier: GPL-2.0-or-later
//
// House systems from their geometric definitions (docs/HOUSES.md). The
// working frame is the true equator and equinox of date: x to the equinox,
// z to the celestial pole. An ecliptic point is where the ecliptic plane
// meets a great circle, so most cusps are one cross product: the line
// common to two planes, taken on the side the definition names.

#include "prometheia/houses.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace prometheia::houses {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kDeg = kPi / 180.0;

struct V3 {
    double x, y, z;
};
V3 cross(const V3& a, const V3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double dot(const V3& a, const V3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
V3 scale(const V3& a, double k) {
    return {a.x * k, a.y * k, a.z * k};
}
V3 add(const V3& a, const V3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
double norm(const V3& a) {
    return std::sqrt(dot(a, a));
}

double wrap_deg(double d) {
    d = std::fmod(d, 360.0);
    if (d < 0.0)
        d += 360.0;
    return d >= 360.0 ? 0.0 : d;
}
double wrap_rad(double r) {
    r = std::fmod(r, kTwoPi);
    return r < 0.0 ? r + kTwoPi : r;
}

// The sky at one instant and place: the obliquity and the local axes.
struct Sky {
    double eps, sin_eps, cos_eps;
    double lat;
    double armc;
    V3 pole_ecl;                      // the ecliptic's pole
    V3 zenith, east, north, meridian; // meridian: the equator's point on the upper meridian

    Sky(double armc_rad, double eps_rad, double lat_rad)
        : eps(eps_rad), sin_eps(std::sin(eps_rad)), cos_eps(std::cos(eps_rad)), lat(lat_rad),
          armc(armc_rad) {
        pole_ecl = {0.0, -sin_eps, cos_eps};
        const double ca = std::cos(armc), sa = std::sin(armc);
        zenith = {std::cos(lat) * ca, std::cos(lat) * sa, std::sin(lat)};
        east = {-sa, ca, 0.0};
        north = cross(zenith, east);
        meridian = {ca, sa, 0.0};
    }

    // Ecliptic longitude (radians) of a direction in the equatorial frame.
    double longitude(const V3& d) const {
        return wrap_rad(std::atan2(d.y * cos_eps + d.z * sin_eps, d.x));
    }

    // Where the ecliptic meets the great circle with pole `normal`, on the
    // side of `side`. nullopt when the two circles coincide.
    std::optional<double> meet(const V3& normal, const V3& side) const {
        V3 d = cross(pole_ecl, normal);
        const double n = norm(d);
        if (n < 1e-12 * norm(normal))
            return std::nullopt;
        if (dot(d, side) < 0.0)
            d = scale(d, -1.0);
        return longitude(d);
    }

    // The ecliptic point on the eastern horizon of a place at latitude `phi`
    // whose ARMC is `r`: the Ascendant there, and the oblique ascension
    // r + 90 degrees under the pole `phi`.
    std::optional<double> ascendant(double r, double phi) const {
        const double ca = std::cos(r), sa = std::sin(r);
        const V3 z{std::cos(phi) * ca, std::cos(phi) * sa, std::sin(phi)};
        return meet(z, V3{-sa, ca, 0.0});
    }

    // The ecliptic point whose right ascension is `alpha` (the hour circle
    // through it meets the ecliptic there).
    double from_right_ascension(double alpha) const {
        return wrap_rad(std::atan2(std::sin(alpha), std::cos(alpha) * cos_eps));
    }

    // The ecliptic point on the circle of ecliptic latitude through the
    // equator's point at right ascension `alpha`.
    double from_equator_by_latitude(double alpha) const {
        return wrap_rad(std::atan2(std::sin(alpha) * cos_eps, std::cos(alpha)));
    }

    double declination(double lon) const { return std::asin(sin_eps * std::sin(lon)); }

    // The diurnal semi-arc of a declination (radians), nullopt where the
    // parallel never crosses the horizon.
    std::optional<double> semi_arc(double dec) const {
        const double c = -std::tan(lat) * std::tan(dec);
        if (!(std::fabs(c) < 1.0))
            return std::nullopt;
        return std::acos(c);
    }

    // A great circle through the horizon's north and south points and the
    // point `q`; the cusp is on q's half of it.
    std::optional<double> through_north_south(const V3& q) const {
        const V3 side = add(q, scale(north, -dot(q, north)));
        return meet(cross(north, q), side);
    }
};

Error undefined(System s, double lat_rad, double eps_rad) {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "%s cusps are undefined at latitude %.4f deg, inside the polar circle (%.4f deg "
                  "at this date), where part of the ecliptic never rises or sets. Porphyry, "
                  "Regiomontanus, Campanus, Equal, Whole Sign, Alcabitius, Morinus, Meridian and "
                  "Topocentric answer at this latitude",
                  std::string(name(s)).c_str(), lat_rad / kDeg, 90.0 - eps_rad / kDeg);
    return make_error(ErrorCode::ArgumentError, buf);
}

Error degenerate(System s) {
    return make_error(ErrorCode::ArgumentError,
                      std::string(name(s)) +
                          " cusps are undefined at this instant: the ecliptic lies along a "
                          "defining circle");
}

// Placidus: the point whose hour angle is the given fraction of its own
// semi-arc (diurnal above the horizon, nocturnal below), found by iterating
// on the right ascension that fraction implies.
std::optional<double> placidus_cusp(const Sky& sky, double fraction, bool above, bool& undefined) {
    // First guess: the point with the right ascension the equator's
    // trisection would give.
    double alpha = above ? sky.armc + fraction * kPi / 2.0 : sky.armc + kPi - fraction * kPi / 2.0;
    double lon = sky.from_right_ascension(alpha);
    for (int i = 0; i < 1000; ++i) {
        const auto sa = sky.semi_arc(sky.declination(lon));
        if (!sa) {
            undefined = true;
            return std::nullopt;
        }
        alpha = above ? sky.armc + fraction * *sa : sky.armc + kPi - fraction * (kPi - *sa);
        const double next = sky.from_right_ascension(alpha);
        double step = std::fabs(next - lon);
        step = std::min(step, kTwoPi - step);
        lon = next;
        if (step < 1e-12)
            return lon;
    }
    return std::nullopt;
}

} // namespace

std::optional<System> from_letter(char c) {
    switch (c) {
    case 'P':
        return System::Placidus;
    case 'K':
        return System::Koch;
    case 'O':
        return System::Porphyry;
    case 'R':
        return System::Regiomontanus;
    case 'C':
        return System::Campanus;
    case 'A':
    case 'E':
        return System::Equal;
    case 'W':
        return System::WholeSign;
    case 'B':
        return System::Alcabitius;
    case 'M':
        return System::Morinus;
    case 'X':
        return System::Meridian;
    case 'T':
        return System::Topocentric;
    default:
        return std::nullopt;
    }
}

namespace {
struct Entry {
    System s;
    char letter;
    std::string_view token, name;
};
constexpr Entry kEntries[] = {
    {System::Placidus, 'P', "placidus", "Placidus"},
    {System::Koch, 'K', "koch", "Koch"},
    {System::Porphyry, 'O', "porphyry", "Porphyry"},
    {System::Regiomontanus, 'R', "regiomontanus", "Regiomontanus"},
    {System::Campanus, 'C', "campanus", "Campanus"},
    {System::Equal, 'A', "equal", "Equal"},
    {System::WholeSign, 'W', "whole-sign", "Whole Sign"},
    {System::Alcabitius, 'B', "alcabitius", "Alcabitius"},
    {System::Morinus, 'M', "morinus", "Morinus"},
    {System::Meridian, 'X', "meridian", "Meridian"},
    {System::Topocentric, 'T', "topocentric", "Topocentric"},
};
const Entry& entry(System s) {
    for (const Entry& e : kEntries)
        if (e.s == s)
            return e;
    return kEntries[0];
}
} // namespace

std::optional<System> from_token(std::string_view t) {
    for (const Entry& e : kEntries)
        if (e.token == t)
            return e.s;
    return std::nullopt;
}
char letter(System s) {
    return entry(s).letter;
}
std::string_view token(System s) {
    return entry(s).token;
}
std::string_view name(System s) {
    return entry(s).name;
}

Result<Houses> compute(System s, double armc_rad, double eps_rad, double lat_rad) {
    if (!std::isfinite(armc_rad) || !std::isfinite(eps_rad) || !std::isfinite(lat_rad) ||
        !(std::fabs(lat_rad) < kPi / 2.0))
        return make_error(ErrorCode::ArgumentError,
                          "houses need a finite sidereal time and obliquity and a latitude "
                          "strictly between -90 and 90 degrees");
    const Sky sky(wrap_rad(armc_rad), eps_rad, lat_rad);
    // Inside a polar circle part of the ecliptic never rises or sets, and the
    // time-trisected systems lose their definition for every instant, not
    // only for the ones whose cusp degrees happen to be circumpolar: the
    // Ascendants Koch uses can be setting points there.
    if ((s == System::Placidus || s == System::Koch) && std::fabs(lat_rad) > kPi / 2.0 - eps_rad)
        return undefined(s, lat_rad, eps_rad);

    const auto asc = sky.ascendant(sky.armc, lat_rad);
    const auto mc = sky.meet(sky.east, sky.meridian);
    const auto vertex = sky.meet(sky.north, scale(sky.east, -1.0));
    if (!asc || !mc || !vertex)
        return degenerate(s);
    Houses h;
    h.angles.asc_deg = wrap_deg(*asc / kDeg);
    h.angles.mc_deg = wrap_deg(*mc / kDeg);
    h.angles.armc_deg = wrap_deg(sky.armc / kDeg);
    h.angles.vertex_deg = wrap_deg(*vertex / kDeg);
    h.angles.equatorial_asc_deg = wrap_deg(sky.from_right_ascension(sky.armc + kPi / 2.0) / kDeg);

    // c[k] is cusp k + 1, radians; the eastern half (10, 11, 12, 1, 2, 3)
    // is computed and the rest are its opposites.
    double c[12] = {};
    auto set_opposites = [&] {
        for (int k : {9, 10, 11, 0, 1, 2})
            c[(k + 6) % 12] = wrap_rad(c[k] + kPi);
    };
    switch (s) {
    case System::Equal:
    case System::WholeSign: {
        const double start =
            s == System::Equal ? *asc : std::floor(*asc / (kPi / 6.0)) * (kPi / 6.0);
        for (int k = 0; k < 12; ++k)
            c[k] = wrap_rad(start + k * kPi / 6.0);
        break;
    }
    case System::Porphyry: {
        const double q1 = wrap_rad(*asc - *mc);       // MC to Ascendant
        const double q2 = wrap_rad(*mc + kPi - *asc); // Ascendant to IC
        c[9] = *mc;
        c[10] = wrap_rad(*mc + q1 / 3.0);
        c[11] = wrap_rad(*mc + 2.0 * q1 / 3.0);
        c[0] = *asc;
        c[1] = wrap_rad(*asc + q2 / 3.0);
        c[2] = wrap_rad(*asc + 2.0 * q2 / 3.0);
        set_opposites();
        break;
    }
    case System::Regiomontanus:
    case System::Campanus: {
        // The circles through the horizon's north and south points and a
        // point 30 and 60 degrees from the meridian, measured on the equator
        // (Regiomontanus) or on the prime vertical from the zenith
        // (Campanus), above the horizon for 11 and 12, below for 2 and 3.
        const V3 from = s == System::Regiomontanus ? sky.meridian : sky.zenith;
        const int cusp[4] = {10, 11, 1, 2};
        const double at[4] = {30.0, 60.0, 120.0, 150.0};
        for (int i = 0; i < 4; ++i) {
            const double a = at[i] * kDeg;
            const auto l = sky.through_north_south(
                add(scale(from, std::cos(a)), scale(sky.east, std::sin(a))));
            if (!l)
                return degenerate(s);
            c[cusp[i]] = *l;
        }
        // Cusp 10 is the circle through the two points and the zenith (the
        // meridian), on the zenith's half: the ecliptic's meridian point above
        // the horizon. That is the MC except inside a polar circle, where the
        // MC can culminate below the horizon and cusp 10 is then the IC.
        const auto top = sky.through_north_south(sky.zenith);
        if (!top)
            return degenerate(s);
        c[9] = *top;
        c[0] = *asc;
        set_opposites();
        break;
    }
    case System::Topocentric: {
        // Oblique ascensions ARMC + 30k under poles tan(pole) = j/3 tan(lat).
        const double t = std::tan(lat_rad);
        const int cusp[4] = {10, 11, 1, 2};
        const double shift[4] = {-60.0, -30.0, 30.0, 60.0};
        const double j[4] = {1.0, 2.0, 2.0, 1.0};
        for (int i = 0; i < 4; ++i) {
            const auto l = sky.ascendant(sky.armc + shift[i] * kDeg, std::atan(j[i] / 3.0 * t));
            if (!l)
                return degenerate(s);
            c[cusp[i]] = *l;
        }
        c[9] = *mc;
        c[0] = *asc;
        set_opposites();
        break;
    }
    case System::Koch: {
        // The Ascendants at the times the MC degree's diurnal semi-arc,
        // trisected, carries the sky from that degree's rising (cusp 10)
        // through the present (cusp 1) to its setting (cusp 4).
        const auto sa = sky.semi_arc(sky.declination(*mc));
        if (!sa)
            return undefined(s, lat_rad, sky.eps);
        const int cusp[4] = {10, 11, 1, 2};
        const double k[4] = {-2.0, -1.0, 1.0, 2.0};
        for (int i = 0; i < 4; ++i) {
            const auto l = sky.ascendant(sky.armc + k[i] * *sa / 3.0, lat_rad);
            if (!l)
                return degenerate(s);
            c[cusp[i]] = *l;
        }
        c[9] = *mc;
        c[0] = *asc;
        set_opposites();
        break;
    }
    case System::Placidus: {
        const int cusp[4] = {10, 11, 1, 2};
        const double fraction[4] = {1.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 1.0 / 3.0};
        const bool above[4] = {true, true, false, false};
        for (int i = 0; i < 4; ++i) {
            bool undef = false;
            const auto l = placidus_cusp(sky, fraction[i], above[i], undef);
            if (undef)
                return undefined(s, lat_rad, sky.eps);
            if (!l)
                return make_error(ErrorCode::ArgumentError,
                                  "Placidus: numerical failure, a cusp did not converge");
            c[cusp[i]] = *l;
        }
        c[9] = *mc;
        c[0] = *asc;
        set_opposites();
        break;
    }
    case System::Alcabitius: {
        // The Ascendant's diurnal semi-arc (ARMC to its right ascension) and
        // nocturnal one trisected on the equator, carried to the ecliptic
        // along hour circles.
        const double ra_asc = std::atan2(std::sin(*asc) * sky.cos_eps, std::cos(*asc));
        const double d = wrap_rad(ra_asc - sky.armc);
        const double n = kPi - d;
        c[9] = *mc;
        c[10] = sky.from_right_ascension(sky.armc + d / 3.0);
        c[11] = sky.from_right_ascension(sky.armc + 2.0 * d / 3.0);
        c[0] = *asc;
        c[1] = sky.from_right_ascension(ra_asc + n / 3.0);
        c[2] = sky.from_right_ascension(ra_asc + 2.0 * n / 3.0);
        set_opposites();
        break;
    }
    case System::Morinus:
    case System::Meridian:
        for (int k = 0; k < 12; ++k) {
            // Cusp 10 at the ARMC, then 30 degrees of right ascension a house.
            const double alpha = sky.armc + ((k + 3) % 12) * kPi / 6.0;
            c[k] = s == System::Meridian ? sky.from_right_ascension(alpha)
                                         : sky.from_equator_by_latitude(alpha);
        }
        break;
    }
    for (int k = 0; k < 12; ++k)
        h.cusp_deg[k] = wrap_deg(c[k] / kDeg);
    return h;
}

} // namespace prometheia::houses

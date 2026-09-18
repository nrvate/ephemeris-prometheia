// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia-ut1 — Delta T (TT - UT1, seconds) that reproduces a published
// local apparent sidereal time, for the cross-test's topocentric anchor
// (docs/CROSS-TEST.md). Horizons prints its own LAST at the site; sending
// the Delta T solved from it makes both servers rotate the Earth as the
// anchor did, so the comparison does not test two Delta T models.
//
// Reads lines "jd_tt tdb_minus_ut_s last_hours lon_deg_east" on stdin
// (tdb_minus_ut only seeds the solve) and prints one Delta T per line.
#include <cmath>
#include <cstdio>

#include <prometheia/frames.hpp>
#include <prometheia/time.hpp>

int main() {
    using namespace prometheia;
    constexpr double kTwoPi = 6.283185307179586476925287;
    double jd_tt = 0.0, tdb_minus_ut = 0.0, last_hours = 0.0, lon_deg = 0.0;
    while (std::scanf("%lf %lf %lf %lf", &jd_tt, &tdb_minus_ut, &last_hours, &lon_deg) == 4) {
        const double guess = jd_tt - (tdb_minus_ut - time::tdb_minus_tt(jd_tt)) / 86400.0;
        const double jd_ut1 = frames::ut1_from_sidereal_time(
            guess, jd_tt, last_hours / 24.0 * kTwoPi, lon_deg / 360.0 * kTwoPi);
        std::printf("%.6f\n", (jd_tt - jd_ut1) * 86400.0);
    }
    return 0;
}

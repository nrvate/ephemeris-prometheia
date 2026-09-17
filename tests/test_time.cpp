// SPDX-License-Identifier: GPL-2.0-or-later
//
// Time-scale tests. Calendar anchors are fixed historical JDs (including
// the DE200 header epoch 1599-12-09 = 2305424.5); UTC/TAI/TT checks are
// exact identities around the leap-second table; TDB and Delta T carry
// their published accuracy classes.
#include <cmath>
#include <cstdio>

#include <prometheia/time.hpp>

#include "test_main.hpp"

using namespace prometheia;
using namespace prometheia::time;

namespace {

bool near(double a, double b, double tol) {
    return std::fabs(a - b) <= tol;
}

} // namespace

TEST(time_calendar_anchors) {
    CHECK(near(jd_from_civil(2000, 1, 1.0), 2451544.5, 0.0));
    CHECK(near(jd_from_civil(2000, 1, 1.5), 2451545.0, 0.0)); // J2000 noon
    CHECK(near(jd_from_ymdhms(2000, 1, 1, 12, 0, 0), 2451545.0, 0.0));
    CHECK(near(jd_from_ymdhms(2000, 1, 1, 0, 0, 0), 2451544.5, 0.0));
    CHECK(near(jd_from_civil(1599, 12, 9.0), 2305424.5, 0.0)); // DE200 start
    CHECK(near(jd_from_civil(1972, 1, 1.0), 2441317.5, 0.0));
    CHECK(near(jd_from_civil(1957, 10, 4.81), 2436116.31, 1e-9));

    Civil c = civil_from_jd(2451545.0);
    CHECK(c.year == 2000 && c.month == 1 && near(c.day, 1.5, 1e-12));
    c = civil_from_jd(2305424.5);
    CHECK(c.year == 1599 && c.month == 12 && near(c.day, 9.0, 1e-12));
    c = civil_from_jd(2441317.5);
    CHECK(c.year == 1972 && c.month == 1 && near(c.day, 1.0, 1e-12));
}

TEST(time_calendar_roundtrip) {
    const int month_len[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int checked = 0;
    for (int y = 1500; y <= 2300; ++y) {
        const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        for (int m = 1; m <= 12; ++m) {
            const int dmax = (m == 2 && leap) ? 29 : month_len[m - 1];
            for (int d = 1; d <= dmax; d += 7) {
                const double jd = jd_from_civil(y, m, double(d));
                CHECK(near(jd - std::floor(jd), 0.5, 0.0));
                const Civil c = civil_from_jd(jd);
                CHECK(c.year == y && c.month == m && near(c.day, double(d), 0.0));
                ++checked;
            }
        }
    }
    // Gregorian reform oddities stay proleptic: 1582-10-15 exists, and so
    // does 1582-10-10 (no calendar gap in our convention).
    CHECK(near(jd_from_civil(1582, 10, 15.0) - jd_from_civil(1582, 10, 4.0), 11.0, 0.0));
    std::printf("  round-trip checked %d dates\n", checked);
}

TEST(time_tai_utc_offsets) {
    CHECK(tai_minus_utc(1971, 12) == 0.0); // before the table
    CHECK(tai_minus_utc(1972, 1) == 10.0);
    CHECK(tai_minus_utc(1972, 6) == 10.0);
    CHECK(tai_minus_utc(1972, 7) == 11.0);
    CHECK(tai_minus_utc(1985, 7) == 23.0);
    CHECK(tai_minus_utc(1999, 1) == 32.0);
    CHECK(tai_minus_utc(2005, 12) == 32.0);
    CHECK(tai_minus_utc(2006, 1) == 33.0);
    CHECK(tai_minus_utc(2016, 6) == 36.0);
    CHECK(tai_minus_utc(2016, 12) == 36.0);
    CHECK(tai_minus_utc(2017, 1) == 37.0);
    CHECK(tai_minus_utc(2026, 9) == 37.0);
}

TEST(time_utc_tt_basics) {
    // TT - UTC at J2000 is 32.184 + 32 = 64.184 s, so the UTC instant
    // that maps to JD(TT) 2451545.0 is 2000-01-01 11:58:55.816.
    auto jd = utc_to_tt(2000, 1, 1, 11, 58, 55.816);
    CHECK(jd.ok());
    if (!jd.ok())
        return;
    CHECK(near(jd.value(), 2451545.0, 1e-7));
    auto back = tt_to_utc(jd.value());
    CHECK(back.ok());
    if (!back.ok())
        return;
    const Utc& u = back.value();
    CHECK(u.year == 2000 && u.month == 1 && u.day == 1);
    CHECK(u.hour == 11 && u.minute == 58);
    CHECK(near(u.second, 55.816, 1e-5));
    CHECK(u.tai_minus_utc == 32.0);

    // A whole-minute instant round-trips exactly.
    jd = utc_to_tt(1990, 6, 15, 13, 45, 0.0);
    CHECK(jd.ok());
    CHECK(near(jd.value(), jd_from_civil(1990, 6, 15.0) + (49500.0 + 25.0 + 32.184) / 86400.0,
               1e-12));
}

TEST(time_utc_leap_seconds) {
    // The 2016-12-31 leap: consecutive UTC labels stay 1 s apart in TT,
    // including the :60 label.
    const double s59 = utc_to_tt(2016, 12, 31, 23, 59, 59.0).value();
    const double s60 = utc_to_tt(2016, 12, 31, 23, 59, 60.0).value();
    const double next = utc_to_tt(2017, 1, 1, 0, 0, 0.0).value();
    // JD doubles quantize at ~40 us near the present epoch, so consecutive
    // seconds are recovered to within that class, not exactly.
    CHECK(near(s60 - s59, 1.0 / 86400.0, 1e-8));
    CHECK(near(next - s60, 1.0 / 86400.0, 1e-8));

    // Inverse labels the leap second correctly, with the old offset in
    // effect during it and the new one from midnight.
    auto u59 = tt_to_utc(s59);
    auto u60 = tt_to_utc(s60);
    auto u60h = tt_to_utc(s60 + 0.5 / 86400.0);
    auto unext = tt_to_utc(next);
    CHECK(u59.ok() && u60.ok() && u60h.ok() && unext.ok());
    if (!(u59.ok() && u60.ok() && u60h.ok() && unext.ok()))
        return;
    CHECK(u59.value().second == 59.0 && u59.value().tai_minus_utc == 36.0);
    CHECK(u60.value().day == 31 && u60.value().hour == 23 && u60.value().minute == 59 &&
          u60.value().second == 60.0);
    CHECK(u60.value().tai_minus_utc == 36.0);
    CHECK(near(u60h.value().second, 60.5, 1e-6));
    CHECK(unext.value().year == 2017 && unext.value().hour == 0 && unext.value().second == 0.0 &&
          unext.value().tai_minus_utc == 37.0);

    // One second before midnight of the 2017 entry the label is the leap.
    auto pre = tt_to_utc(next - 0.25 / 86400.0);
    CHECK(pre.ok());
    if (pre.ok()) {
        CHECK(pre.value().day == 31 && pre.value().second >= 60.0);
    }

    // Errors: no leap second on ordinary days, bad dates, pre-1972 UTC.
    CHECK(!utc_to_tt(2000, 1, 1, 23, 59, 60.0).ok());
    CHECK(!utc_to_tt(2000, 1, 1, 23, 59, 60.5).ok());
    CHECK(utc_to_tt(2016, 12, 31, 23, 59, 60.999).ok());
    CHECK(!utc_to_tt(2016, 12, 31, 23, 59, 61.5).ok());
    CHECK(!utc_to_tt(2016, 2, 30, 0, 0, 0).ok());
    CHECK(!utc_to_tt(1969, 7, 20, 20, 17, 0).ok());
    CHECK(!utc_to_tt(1971, 12, 31, 23, 59, 59).ok());
    CHECK(!tt_to_utc(2441317.5).ok()); // 1972-01-01 00:00 UTC is the earliest
    CHECK(tt_to_utc(2441317.5 + 0.01).ok());
}

TEST(time_utc_tt_sweep_roundtrip) {
    // Monthly samples across the table era, plus dense steps around three
    // leap events, all round-trip and stay strictly monotone in TT.
    double prev = -1e30;
    for (int y = 1972; y <= 2099; ++y) {
        for (int mo = 1; mo <= 12; ++mo) {
            const double jd = utc_to_tt(y, mo, 15, 6, 30, 0.0).value();
            CHECK(jd > prev);
            prev = jd;
            const auto u = tt_to_utc(jd);
            CHECK(u.ok());
            if (u.ok()) {
                const Utc& v = u.value();
                CHECK(v.year == y && v.month == mo && v.day == 15 && v.hour == 6 &&
                      v.minute == 30 && v.second == 0.0);
            }
        }
    }
    for (double sec = -5.0; sec <= 5.0; sec += 0.25) {
        const double base = utc_to_tt(2015, 6, 30, 23, 59, 55.0).value() + sec / 86400.0;
        const auto u = tt_to_utc(base);
        CHECK(u.ok());
    }
}

TEST(time_tdb_tt) {
    // Amplitude bound over the DE200 span, published-accuracy class.
    double max_abs = 0.0;
    double j2000_val = tdb_minus_tt(2451545.0);
    for (double jd = 2305424.5; jd <= 2513392.5; jd += 8.0) {
        max_abs = std::max(max_abs, std::fabs(tdb_minus_tt(jd)));
    }
    CHECK(max_abs < 1.8e-3);
    CHECK(max_abs > 1.4e-3); // dominant annual term must be present
    // J2000: near perihelion, sin(g) small and negative.
    CHECK(j2000_val < -8.0e-5 && j2000_val > -1.2e-4);

    // Inversion round-trip: evaluating the series at the TDB argument
    // inverts it to far below the series' own accuracy.
    for (double jd = 2305424.5; jd <= 2513392.5; jd += 37.0) {
        const double rt = tt_from_tdb(tdb_from_tt(jd));
        CHECK(near(rt, jd, 1e-11));
    }
}

TEST(time_delta_t) {
    // Anchors computed from the published polynomials (see docs/TIME.md),
    // sampled in early January where the segment argument is smallest.
    // Windows are generous to the model's own class but tight enough to
    // catch a wrong segment or coefficient.
    const EspenakMeeusDeltaT model;
    auto em = [&](int y) { return model.delta_t_seconds(jd_from_civil(y, 1, 1.0)); };
    CHECK(em(2000) > 63.3 && em(2000) < 64.4);       // 63.88 (SWE at J2000: 63.83)
    CHECK(em(1975) > 44.9 && em(1975) < 46.0);       // 45.49
    CHECK(em(1900) > -3.5 && em(1900) < -2.0);       // -2.73
    CHECK(em(2020) > 71.0 && em(2020) < 72.2);       // 71.63
    CHECK(em(2050) > 92.5 && em(2050) < 93.5);       // 93.01
    CHECK(em(2100) > 200.0 && em(2100) < 206.0);     // 202.79
    CHECK(em(1600) > 119.0 && em(1600) < 121.5);     // 119.96
    CHECK(em(1000) > 1570.0 && em(1000) < 1580.0);   // 1574.3
    CHECK(em(-500) > 17000.0 && em(-500) < 17400.0); // 17198.7
    CHECK(em(1820) > 11.5 && em(1820) < 12.2);       // 11.87

    // The pluggable interface dispatches to the same model.
    const DeltaTModel& iface = model;
    CHECK(near(iface.delta_t_seconds(2451545.0), model.delta_t_seconds(2451545.0), 0.0));

    // UT1 inversion round-trips well below the model's own accuracy. The
    // Espenak-Meeus decimal year has monthly granularity (year + (month -
    // 0.5)/12) and genuine jumps at segment boundaries, so inversion is
    // ill-defined within ~dT of those steps; sample mid-month, far from
    // any boundary.
    for (int y = 1600; y <= 2150; y += 17) {
        const double jd = jd_from_civil(y, 7, 15.0);
        const double rt = jd_tt_from_ut1(jd_ut1_from_tt(jd));
        CHECK(near(rt, jd, 1e-8));
    }
}

TEST(time_delta_t_observed) {
    // USNO samples are reproduced exactly (src/delta_t_table.inc). Pins
    // are historical values that a release refresh does not change.
    const ObservedDeltaT obs;
    CHECK(near(obs.delta_t_seconds(jd_from_civil(1973, 2, 1.0)), 43.4724, 1e-9));
    CHECK(near(obs.delta_t_seconds(jd_from_civil(2000, 1, 1.0)), 63.8285, 1e-9));
    CHECK(near(obs.delta_t_seconds(jd_from_civil(2020, 1, 1.0)), 69.3612, 1e-9));
    CHECK(near(obs.delta_t_seconds(jd_from_civil(1900, 1, 1.0)), -2.70, 1e-9));
    CHECK(near(obs.delta_t_seconds(jd_from_civil(1700, 1, 1.0)), 21.0, 1e-9));
    // Linear between monthly samples.
    const double j0 = jd_from_civil(2000, 1, 1.0), j1 = jd_from_civil(2000, 2, 1.0);
    CHECK(near(obs.delta_t_seconds(0.5 * (j0 + j1)), 0.5 * (63.8285 + 63.8557), 1e-9));
    // J2000 noon: SWE reports 63.8289 s.
    CHECK(near(obs.delta_t_seconds(2451545.0), 63.829, 0.002));
    // The default convenience function is this model.
    CHECK(near(delta_t(2461300.5), obs.delta_t_seconds(2461300.5), 0.0));

    // Coverage: 1657 onward, observed within the last year or so.
    const double first = ObservedDeltaT::table_first_jd();
    const double last = ObservedDeltaT::table_last_jd();
    CHECK(near(first, jd_from_civil(1657, 1, 1.0), 1e-6));
    CHECK(last > jd_from_civil(2026, 1, 1.0));

    // Continuous across both table ends and the extrapolation blends.
    for (double edge : {first, last}) {
        CHECK(near(obs.delta_t_seconds(edge - 1e-6), obs.delta_t_seconds(edge), 1e-6));
    }
    // (From 1601: Espenak-Meeus itself has a 0.25 s seam at 1600.0 between
    // its published segments, inside the pre-table blend.)
    for (double jd = jd_from_civil(1601, 1, 1.0); jd < last + 40000.0; jd += 0.73) {
        CHECK(near(obs.delta_t_seconds(jd + 0.73), obs.delta_t_seconds(jd), 0.02));
    }

    // Before the table: Espenak-Meeus a century out (continuous-year
    // evaluation differs from its mid-month convention by < 1 s there).
    const EspenakMeeusDeltaT em;
    const double jd1500 = jd_from_civil(1500, 1, 1.0);
    CHECK(near(obs.delta_t_seconds(jd1500), em.delta_t_seconds(jd1500), 0.5));
    // After the table: near-flat at first, then the tidal parabola.
    const double jd2030 = jd_from_civil(2030, 1, 1.0);
    CHECK(obs.delta_t_seconds(jd2030) > 66.0 && obs.delta_t_seconds(jd2030) < 73.0);
    const double jd2300 = jd_from_civil(2300, 1, 1.0);
    const double u = (2000.0 + (jd2300 - 2451545.0) / 365.25 - 1820.0) / 100.0;
    CHECK(near(obs.delta_t_seconds(jd2300), -20.0 + 32.0 * u * u, 1e-9));

    // The UT1 inversion round-trips everywhere, including the table ends
    // (the model is continuous, unlike the Espenak-Meeus segments).
    for (double jd : {first, last, first + 0.5, last + 0.5, 2451545.0, 2305447.5, 2488069.5}) {
        const double rt = jd_tt_from_ut1(jd_ut1_from_tt(jd));
        CHECK(near(rt, jd, 1e-8));
    }
}

int main() {
    return ptest::run_all();
}
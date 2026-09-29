// SPDX-License-Identifier: GPL-2.0-or-later
//
// Time scales: calendar <-> JD, UTC <-> TAI <-> TT <-> TDB, and Delta T.
//
// Calendar is proleptic Gregorian, JD runs noon-to-noon as usual. The
// civil-date arithmetic is the exact branch-free algorithm published by
// Howard Hinnant (the family behind C++ std::chrono), valid for the full
// double-precision JD range; sources and accuracy notes in docs/TIME.md.
//
// UTC support covers the integer-leap-second era from 1972-01-01
// onward. Earlier UTC (the 1961-1972 linear-drift era) is rejected with
// a clear error: supply JD(TT) directly for those epochs.
#ifndef PROMETHEIA_TIME_HPP
#define PROMETHEIA_TIME_HPP

#include <array>
#include <cstdint>

#include "prometheia/error.hpp"

namespace prometheia::time {

// --- Calendar <-> JD -------------------------------------------------------

// JD at 00:00 of the given proleptic Gregorian date (so the result is
// always X.5). day may be fractional (day = 1.5 = noon of the 1st).
double jd_from_civil(int year, int month, double day);

struct Civil {
    int year = 0;
    int month = 0;
    double day = 0.0; // 1-based, fractional
};

Civil civil_from_jd(double jd);

// JD from a wall-clock civil date and time. Any continuous scale; the
// caller declares which one the result is in.
double jd_from_ymdhms(int year, int month, int day, int hour, int minute, double second);

// --- UTC <-> TAI <-> TT ----------------------------------------------------

// TT is TAI + 32.184 s exactly.
inline constexpr double kTtMinusTaiSeconds = 32.184;

// The TAI-UTC offset in seconds in effect at 00:00 UTC of the given
// month (post-1972 era; the table tracks the USNO tai-utc.dat list,
// currently ending at 37 s effective 2017-01-01). Later months keep the
// last offset: no further leap seconds are assumed (docs/TIME.md).
double tai_minus_utc(int year, int month);

// Converts a civil UTC instant to JD(TT). second may be 60.x during an
// inserted leap second (only on days the table has one); values before
// 1972-01-01 UTC, invalid dates, or bogus leap seconds are errors.
Result<double> utc_to_tt(int year, int month, int day, int hour, int minute, double second);

struct Utc {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    double second = 0.0;        // may be 60.x on a leap-second day
    double tai_minus_utc = 0.0; // offset in effect at this instant
};

// Inverse of utc_to_tt. The result labels the leap second as 23:59:60.x
// where the table has one. Errors for input before 1972-01-01 00:00:00
// UTC or non-finite jd.
Result<Utc> tt_to_utc(double jd_tt);

inline double tt_from_tai(double jd_tai) {
    return jd_tai + kTtMinusTaiSeconds / 86400.0;
}
inline double tai_from_tt(double jd_tt) {
    return jd_tt - kTtMinusTaiSeconds / 86400.0;
}

// --- TT <-> TDB -------------------------------------------------------------

// Geocentric TDB-TT in seconds: the full Fairhead & Bretagnon (1990)
// series, 787 terms (docs/TIME.md, "TDB"), within +/- 3 ns of integrated
// time ephemerides over 1950-2050, and carrying the secular decline of the
// Earth's eccentricity that epochs millennia away need. ~12 us a call:
// code that converts many instants keeps a TdbInterpolator.
double tdb_minus_tt(double jd_tt);

// The same, with its rate in seconds per day, from the series' analytic
// derivative.
void tdb_minus_tt_with_rate(double jd_tt, double& value, double& rate_per_day);

// TDB-TT for code that converts many instants (the Engine keeps one): nodes
// one day apart, each holding the value and rate of the series' 136 terms
// that can move it by 0.1 us anywhere in DE441's span, with the cubic
// Hermite polynomial matching both between two nodes. Within 1.55 us of all
// 787 terms over -13000..17000 (0.94 us over 1550-2650), below the 10-80 us
// a JD double can hold there, at a tenth of the cost (docs/TIME.md). The
// value depends only on the instant, never on query history. Nodes are kept
// in a direct-mapped cache of 256. Not thread-safe; one per thread.
class TdbInterpolator {
public:
    double tdb_minus_tt(double jd_tt);
    double tdb_from_tt(double jd_tt) { return jd_tt + tdb_minus_tt(jd_tt) / 86400.0; }
    // Series sums so far (for tests and benchmarks).
    unsigned long long evaluations() const { return evaluations_; }
    static constexpr double kNodeSpacingDays = 1.0;

private:
    struct Node {
        long long index = -(1LL << 62);
        double value = 0.0, rate = 0.0;
    };
    const Node& node(long long index);
    std::array<Node, 256> nodes_{};
    unsigned long long evaluations_ = 0;
};

inline double tdb_from_tt(double jd_tt) {
    return jd_tt + tdb_minus_tt(jd_tt) / 86400.0;
}

// Inverse; evaluating the series at the TDB argument inverts it to a
// few nanoseconds (TDB-TT changes by under 3e-8 s per second).
inline double tt_from_tdb(double jd_tdb) {
    return jd_tdb - tdb_minus_tt(jd_tdb) / 86400.0;
}

// --- Delta T = TT - UT1 ------------------------------------------------------

class DeltaTModel {
public:
    virtual ~DeltaTModel() = default;
    virtual double delta_t_seconds(double jd_tt) const = 0;
};

// The Espenak-Meeus piecewise polynomials (NASA "Five Millennium Canon"
// dataset, eclipse.gsfc.nasa.gov), covering -500 to +2150 with parabolic
// extrapolation outside. Modern-era accuracy is seconds: the 2005-2050
// segment runs ~6 s high in the 2020s. Kept for reproducing results made
// with it; ObservedDeltaT can use it as its pre-telescopic branch.
class EspenakMeeusDeltaT final : public DeltaTModel {
public:
    double delta_t_seconds(double jd_tt) const override;
};

// The Stephenson-Morrison-Hohenkerk reconstruction from ancient and
// medieval eclipses and telescopic occultations: the cubic spline of
// Stephenson, Morrison & Hohenkerk (2016), Proc. R. Soc. A 472: 20160404,
// in the v. 2020 coefficients of Morrison, Stephenson, Hohenkerk &
// Zawilski (2021), Proc. R. Soc. A 477: 20200776 (both CC BY 4.0).
// The spline covers -720.0 to 2019.0 (decimal Julian years); outside it,
// the paper's long-term parabola -320 + 32.5 u^2, u = (year - 1825) / 100,
// shifted by a constant to meet the spline at each end. The pre-telescopic
// branch of ObservedDeltaT by default.
class StephensonMorrisonHohenkerkDeltaT final : public DeltaTModel {
public:
    double delta_t_seconds(double jd_tt) const override;
    static double spline_first_year(); // -720.0
    static double spline_last_year();  // 2019.0
};

// Default model: observed Delta T from the USNO series compiled into the
// library (src/delta_t_table.inc, refreshed at each release by
// tools/gen/gen_earth_orientation.py): half-yearly 1657-1972, monthly
// from 1973, linearly interpolated.
//   - Before the table: Stephenson-Morrison-Hohenkerk (or Espenak-Meeus,
//     if constructed with Early::kEspenakMeeus), shifted by a continuity
//     offset that fades out linearly over the preceding century.
//   - After the table: the trend of its last two years, blended by a
//     smoothstep over the following century into the Morrison-Stephenson
//     (2004) long-term parabola -20 + 32 u^2, u = (year - 1820) / 100.
// Continuous everywhere; see docs/TIME.md for accuracy.
class ObservedDeltaT final : public DeltaTModel {
public:
    enum class Early { kStephensonMorrisonHohenkerk, kEspenakMeeus };
    explicit ObservedDeltaT(Early early = Early::kStephensonMorrisonHohenkerk) : early_(early) {}
    double delta_t_seconds(double jd_tt) const override;
    // Coverage of the observed samples (JD).
    static double table_first_jd();
    static double table_last_jd();
    Early early() const { return early_; }

private:
    Early early_;
};

// Convenience: the default (ObservedDeltaT) model.
double delta_t(double jd_tt);

inline double jd_ut1_from_tt(double jd_tt) {
    return jd_tt - delta_t(jd_tt) / 86400.0;
}
double jd_tt_from_ut1(double jd_ut1); // one-step Newton inverse

} // namespace prometheia::time

#endif // PROMETHEIA_TIME_HPP
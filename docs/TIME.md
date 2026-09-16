# Time scales

`prometheia::time` (in `include/prometheia/time.hpp`, `src/time.cpp`)
covers calendar↔JD, UTC↔TAI↔TT↔TDB, and Delta T (TT−UT1). The ephemeris
side of the house (DE files, EPM1 epochs, the integrator) runs in TDB
JD; the engine layer converts at its boundary.

## Calendar

Proleptic Gregorian, JD runs noon-to-noon. The civil-date arithmetic is
Howard Hinnant's published branch-free algorithms (`days_from_civil` /
`civil_from_days`, the family behind C++ `std::chrono`), which are exact
over ±328,000 years — round-trip tested over 1500–2300 in `test_time`,
plus fixed anchors: J2000 noon = 2451545.0, the DE200 start epoch
1599-12-09 = 2305424.5, 1972-01-01 = 2441317.5. There is no 1582
calendar gap in our convention (proleptic Gregorian throughout).

Earlier prototyping with the classic Meeus formulas showed their
well-known calculator artifacts outside the modern era (a floor/trunc
ambiguity that misplaces dates by one day in some years); Hinnant's
algorithms were adopted instead after cross-verifying the two
implementations against each other and against `datetime`-derived
anchors.

## UTC / TAI / TT

- **TT = TAI + 32.184 s** exactly (by definition of TT).
- **TAI−UTC** comes from a built-in table of the USNO `tai-utc.dat`
  integer-leap entries, 1972-01-01 (10 s) through 2017-01-01 (37 s);
  update the table when the IERS announces the next leap second. The
  table is keyed by the calendar month of the UTC day, which makes the
  leap second 23:59:60 belong to the day that contains it, under the
  offset still in effect.
- The 1961–1972 linear-drift era of UTC is rejected with a clear error;
  supply JD(TT) directly for those epochs.
- `utc_to_tt` / `tt_to_utc` round-trip every monthly sample from 1972 to
  2099 and keep consecutive UTC labels — including the `:60` label —
  exactly 1 s apart in TT.
- Leap labeling: during the inserted second, the nominal UTC rendering
  lands in [0 s, 1 s) of the *next* day, which `tt_to_utc` relabels as
  23:59:60.x of the leap day. The comparison instants are kept in the TT
  domain (no intermediate subtraction) so the exact-midnight case is
  exact.

**Precision note:** a JD double quantizes at ~40 µs near the present
epoch (ulp ≈ 4.7e-10 day at JD 2.46e6). `tt_to_utc` snaps rendered
seconds to 0.1 ms and rolls overflow; tests use tolerances above the
quantization floor. This is inherent to JD-as-double, not to UTC.

## TDB

`tdb_minus_tt` is the truncated Fairhead–Bretagnon series published in
USNO Circular 179 (eq. 2.6), maximum error **~10 µs over 1600–2200**;
`tt_from_tdb` inverts it by evaluating the series at the TDB argument
(inversion error ~nanoseconds, far below the series' own accuracy).
Amplitude is the familiar 1.657 ms annual term; the J2000 value is about
−96 µs (perihelion-side anomaly). For reference-grade pulsar timing this
would need the Harada–Fukushima series; for ephemeris lookup and
astrology it is orders of magnitude beyond sufficient.

## Delta T (TT − UT1)

`DeltaTModel` is a virtual interface; the default implementation is the
**Espenak–Meeus piecewise polynomial set** (NASA Five Millennium Canon
dataset, `eclipse.gsfc.nasa.gov/SEhelp/deltatpoly2004.html`), covering
−500 to +2150 with parabolic extrapolation outside.

Accuracy and caveats, stated honestly:

- The polynomials reproduce observed ΔT to **seconds** in the modern
  era (63.88 s at J2000 vs 63.83 s observed; SWE reports 63.828914 s at
  the same epoch). The 2005–2050 segment already drifts a few seconds
  high against observation in the 2020s — the dataset's own known
  limitation. If sub-second UT1 matters (sidereal time, houses), plug an
  observed table (e.g. IERS EOP C04) in via `DeltaTModel`; a built-in
  observed-table model is queued for the engine milestone.
- The decimal year has monthly granularity (`year + (month−0.5)/12`),
  so ΔT steps by ~0.03 s per month by construction; segment boundaries
  (e.g. 1986.0, 2005.0) have genuine jumps of ~2 s. Inversion
  (`jd_tt_from_ut1`) is therefore ill-defined within ~ΔT of those steps
  and converges to fp-exactness elsewhere (one refinement step).

## Sources

- USNO Circular 179 (Kaplan, "The IAU Resolutions on Astronomical
  Reference Systems, Time Scales, and Earth Rotation Models") — TDB−TT
  series; public-domain USNO publication.
- USNO `maia.usno.navy.mil/ser7/tai-utc.dat` — leap-second table.
- NASA `eclipse.gsfc.nasa.gov/SEhelp/deltatpoly2004.html` — Espenak–Meeus
  polynomials (public-domain NASA publication).
- Howard Hinnant's chrono date algorithms (public domain).
- Fairhead & Bretagnon (1990), A&A 229, 240 — the full TDB−TT series
  the Circular 179 truncation derives from.
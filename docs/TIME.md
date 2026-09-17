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
  integer-leap entries, 1972-01-01 (10 s) through 2017-01-01 (37 s),
  generated into `src/leap_second_table.inc` and refreshed at each
  release with the ΔT table (below). The
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

`DeltaTModel` is a virtual interface with three built-in implementations.

### Default: `ObservedDeltaT`

Observed ΔT from the **US Naval Observatory** series, compiled into the
library as `src/delta_t_table.inc` (1272 samples as of the 2026-09-16
refresh):

- `historic_deltat.data`: half-yearly values 1657.0–1972.5, each with the
  USNO's stated uncertainty (±12 s in 1657, ±1.9 s in 1800, ±0.07 s in
  1900, 1 ms from the 1950s);
- `deltat.data`: monthly values from 1973-02-01 to the latest published
  month (2026-04-01 at this refresh).

The two series agree to < 0.05 s where they overlap (1973–1984; the
generator enforces it). Values between samples are linear interpolation,
which is accurate to milliseconds at these spacings.

Outside the table:

- **Before 1657:** the Stephenson–Morrison–Hohenkerk spline (below),
  plus an offset that makes it meet the first observation (+6.48 s: the
  USNO table has 44.0 s at 1657.0, the spline 37.52 s) and fades linearly
  to zero over the preceding century. `ObservedDeltaT(Early::kEspenakMeeus)`
  uses the Espenak–Meeus polynomials there instead (offset +3.7 s),
  evaluated at a continuous decimal year; they have their own 0.25 s seam
  at 1600.0 between two published segments, inside that blend.
- **After the last sample:** a least-squares trend over the table's last
  two years, blended by a smoothstep over the following century into
  the Morrison & Stephenson (2004) long-term parabola
  ΔT = −20 + 32u² s, u = (year − 1820)/100. The join is continuous in
  value. The near term stays close to the recent trend (69.12 s in
  2026-09, 69.2 s in 2030), and the far future follows tidal braking
  (203 s in 2100, the parabola from 2126). Predictions degrade with
  distance: expect tenths of a second within a year of the table's end
  and seconds within a decade. That drift is why the table is refreshed
  at every release ([INGESTION.md](INGESTION.md)).

Comparison at the engine fixture epochs:

| epoch | observed model | Espenak–Meeus | Swiss Ephemeris |
|-------|---------------:|--------------:|----------------:|
| 1800-01-01 | 12.60 s (USNO ±1.9) | 13.71 s | 18.90 s |
| J2000 | 63.829 s | 63.874 s | 63.829 s |
| 2026-09-16 | 69.117 s | 75.511 s | 68.822 s |
| 2100-01-01 | 202.8 s | 202.8 s | 93.2 s |

In the modern era the observed model and SWE agree to the millisecond at
J2000. The Espenak–Meeus 2005–2050 segment runs 6.4 s high in 2026,
which is 3.5″ of lunar longitude and about 96″ of Earth rotation for
anything computed from UT. Before about 1955 the published
reconstructions disagree by several seconds (1800: USNO 12.6 ± 1.9 s,
Espenak–Meeus 13.7 s, SWE 18.9 s); USNO's own stated uncertainty is the
honest bound. Any future value is a model choice.

### `EspenakMeeusDeltaT`

The **Espenak–Meeus piecewise polynomial set** (NASA Five Millennium
Canon dataset, `eclipse.gsfc.nasa.gov/SEhelp/deltatpoly2004.html`),
covering −500 to +2150 with parabolic extrapolation outside. It is kept
for reproducing results computed with it. Its decimal year has monthly
granularity (`year + (month−0.5)/12`), so ΔT steps by ~0.03 s per month
by construction, and segment boundaries (e.g. 1986.0, 2005.0) have
genuine jumps of ~2 s. Inversion (`jd_tt_from_ut1`) is therefore
ill-defined within ~ΔT of those steps.

### `StephensonMorrisonHohenkerkDeltaT`

The modern reconstruction of Earth rotation from ancient and medieval
eclipses and telescopic lunar occultations: the cubic spline of
Stephenson, Morrison & Hohenkerk (2016), in its **v. 2020 coefficients**
from the addendum by Morrison, Stephenson, Hohenkerk & Zawilski (2021).
It is the pre-1657 branch of `ObservedDeltaT`, and usable on its own.

- **Data:** Table S15 v. 2020, 58 cubic segments from −720.0 to 2019.0,
  compiled in as `src/delta_t_smh_table.inc`. ΔT = a₀ + a₁t + a₂t² + a₃t³
  with t = (Y − Kᵢ)/(Kᵢ₊₁ − Kᵢ), Y a continuous decimal Julian year
  (2000 + (JD − 2451545)/365.25).
- **Outside the spline:** the paper's long-term parabola (eq. 4.1)
  ΔT = −320.0 + 32.5 u² s, u = (Y − 1825)/100, shifted by a constant so
  the value is continuous: −358.4 s before −720, +267.0 s after 2019. The
  parabola's slope differs from the spline's by 0.4 s/yr at −720 and
  1 s/yr at 2019. After 2019 the standalone model is a rough tidal trend
  (111 s in 2050); `ObservedDeltaT` never uses that side.
- **Continuity:** the published knots agree to their 3-decimal rounding
  (the generator enforces ≤ 0.01 s).
- **Against Espenak–Meeus:** −254 s at −720, −265 s at −500, −143 s at
  year 0, +76 s at 1000, +94 s at 1500, −6 s at 1650, −0.07 s at 2000.
  Hundreds of seconds is ~0.1° of Earth rotation, which is what the
  2016 analysis corrected with new eclipse data.

**Licence.** Both papers and the figshare supplement (item 13885863)
are CC BY 4.0: free to use with credit, which this section, the header
comment and the generated table give. The HM Nautical Almanac Office's
web copies of the same tables are Crown copyright and are not used.

### Refreshing the tables

`tools/gen/gen_smh_delta_t.py` builds `src/delta_t_smh_table.inc` from
the supplement zip (`https://ndownloader.figshare.com/files/26513569`,
sha256 `1a649a16…cf37`, checked). It is a fixed publication, not part of
release ingestion; rerun it only if the authors publish a new version.

`tools/gen/gen_earth_orientation.py` regenerates `src/delta_t_table.inc`
and `src/leap_second_table.inc` from USNO's files. It records each
source's URL and SHA-256 and validates the data: dates ascending, no
jumps over 2 s, overlap agreement, and leap seconds a +1 s sequence from
1972-01-01 at 10 s. `--check` reports whether the committed tables are
stale. The release procedure is in [INGESTION.md](INGESTION.md).

## Sources

- USNO Circular 179 (Kaplan, "The IAU Resolutions on Astronomical
  Reference Systems, Time Scales, and Earth Rotation Models") — TDB−TT
  series; public-domain USNO publication.
- USNO `maia.usno.navy.mil/ser7/tai-utc.dat` — leap-second table.
- USNO `maia.usno.navy.mil/ser7/historic_deltat.data` and `deltat.data` —
  observed ΔT (public domain, US Government work).
- F. R. Stephenson, L. V. Morrison & C. Y. Hohenkerk (2016),
  "Measurement of the Earth's rotation: 720 BC to AD 2015", Proc. R. Soc.
  A 472: 20160404, https://doi.org/10.1098/rspa.2016.0404 (CC BY 4.0) —
  the spline and the long-term parabola.
- L. V. Morrison, F. R. Stephenson, C. Y. Hohenkerk & M. Zawilski (2021),
  "Addendum 2020 to 'Measurement of the Earth's rotation: 720 BC to AD
  2015'", Proc. R. Soc. A 477: 20200776,
  https://doi.org/10.1098/rspa.2020.0776; supplementary Table S15 v. 2020,
  figshare 10.6084/m9.figshare.c.5300925 (CC BY 4.0) — the coefficients.
- Morrison & Stephenson (2004), J. Hist. Astron. 35, 327 — the long-term
  ΔT parabola −20 + 32u².
- NASA `eclipse.gsfc.nasa.gov/SEhelp/deltatpoly2004.html` — Espenak–Meeus
  polynomials (public-domain NASA publication).
- Howard Hinnant's chrono date algorithms (public domain).
- Fairhead & Bretagnon (1990), A&A 229, 240 — the full TDB−TT series
  the Circular 179 truncation derives from.
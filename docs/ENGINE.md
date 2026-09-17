# Engine: positions API and apparent place

`prometheia::Engine` (`include/prometheia/engine.hpp`, `src/engine.cpp`)
is the query surface: one planetary ephemeris in, `calc(body, time,
options)` out. The correction formulas live in
`include/prometheia/apparent.hpp` as pure functions; the frame chain is
[FRAMES.md](FRAMES.md); the ephemeris readers are [DE.md](DE.md) and
[SPK.md](SPK.md).

```cpp
auto engine = prometheia::Engine::open("ephe/linux_p1550p2650.440").value();
auto moon = engine.calc(prometheia::body::kMoon, jd_tt);        // apparent, ecliptic of date
auto mars = engine.calc_ut(prometheia::body::kMars, jd_ut1, {    // any preset or field
    .center = prometheia::Center::Topocentric,
    .site = {lon_rad, lat_rad, height_m}});
```

## Contract

- **Ephemeris:** a JPL DE binary or a DAF/SPK kernel, detected by content
  (`DAF/SPK ` identification word), not extension. Both give identical
  answers for the same DE (DE440 binary vs `de440s.bsp`: ≤ 4 µas through
  the whole pipeline).
- **Bodies** are NAIF IDs, the same key the catalog uses: 10 Sun, 301
  Moon, 199 Mercury, 299 Venus, 399 Earth, 3 Earth-Moon barycentre, 0
  solar-system barycentre, and 4–9 for Mars…Pluto. JPL's planetary files
  carry only *system barycentres* beyond Venus; the planet-centre IDs
  (499…999) are answered by an ephemeris that has them and are otherwise
  `NotFound` — never substituted. Bodies the ephemeris does not know are
  looked up in the catalogs added with `add_catalog()` (below).
- **Time:** `calc` takes JD(TT); `calc_ut` takes JD(UT1) and converts with
  the engine's `DeltaTModel` (observed USNO ΔT, `time::ObservedDeltaT`,
  unless set; [TIME.md](TIME.md)). TDB for the
  ephemeris argument comes from `time::tdb_from_tt`.
- **Output** (`Position`): longitude/latitude (ecliptic) or right
  ascension/declination (equatorial) in degrees, distance in AU (IAU 2012
  AU, 149,597,870.7 km), their daily rates, and the same vector in
  rectangular form. `Provenance` names the ephemeris, DE number and the
  light time applied. `sigma_arcsec` is empty for planets — the DE files
  publish no per-epoch covariance — and is filled by the catalog overlay
  for small bodies (a later M4 increment).
- **Threading:** an `Engine` caches ephemeris records and the frame
  matrices of the last three epochs; it is not safe for concurrent use.
  One engine per thread; there is no global state.

## Options

| field | values | default |
|-------|--------|---------|
| `center` | `Geocentric`, `Topocentric` (+ `site`, WGS84), `Heliocentric`, `Barycentric` | geocentric |
| `frame` | `ICRF`, `J2000` (ICRF + frame bias), `MeanOfDate` (+ IAU 2006 precession), `TrueOfDate` (+ IAU 2000A nutation) | true of date |
| `coords` | `Ecliptic`, `Equatorial` | ecliptic |
| `light_time`, `deflection`, `aberration` | independent switches | all on |
| `speed` | rates by central difference (3× the work) | on |

Presets: `CalcOptions::apparent()` (the defaults), `astrometric()` (light
time only), `geometric()` (no corrections).

Ecliptic output in `ICRF` and `J2000` uses the J2000 mean obliquity
(84381.406″); in the date frames the mean obliquity of date. The true
ecliptic of date is the mean ecliptic plane with the true equinox — the
ecliptic does not nutate, only the equinox slides by Δψ ([FRAMES.md](FRAMES.md)).

## Pipeline

For a TT epoch *t*:

1. **Observer** barycentric state at TDB(*t*): the Earth, the Sun, the
   barycentre (zero), or for topocentric the Earth plus the WGS84 site
   rotated by GAST (UT1 from the ΔT model) into the true equator of date
   and carried to ICRF by (N·P·B)ᵀ, with site velocity ω × r (ω = the ERA
   rate, 2π · 1.00273781191135448 rad/day). Polar motion is neglected
   (≤ 0.3″ in the site, sub-mas at the Moon).
2. **Light time:** the body's barycentric position at *t* − τ with
   τ = |x_body(t − τ) − x_obs(t)| / c, iterated to |Δτ| < 10⁻¹⁵ day (the
   iteration contracts by ~v/c per pass; 3–4 passes). The retarded epoch
   `jd − τ` is formed with its exact rounding error recovered (TwoSum) and
   applied through the body's velocity: a JD double near the present
   resolves only ~40 µs, which would otherwise put metre-class noise into
   the differenced rates.
3. **Gravitational deflection** by the Sun (skipped when the body is the
   Sun or the observer is the Sun or the barycentre): with unit vectors
   p (observer→body), q (Sun→body), e (Sun→observer) and E = |Sun→observer|,

   p₁ = p + (2GM/(c²E)) / (1 + q·e) · ((p·q) e − (e·p) q)

   GM/c² = 1.476625 km (DE440 GM☉). Checks: 4.07 mas at 90° elongation,
   1.75″ at the limb.
4. **Aberration**, relativistic, with the observer's barycentric velocity
   V = v/c and β⁻¹ = √(1 − V²):

   p′ = (β⁻¹ p + (1 + p·V / (1 + β⁻¹)) V) / (1 + p·V)

   Topocentric velocity includes the diurnal term (≤ 0.32″). For a
   heliocentric observer the Sun's own barycentric velocity applies
   (~0.01″); barycentric has none.
5. **Frame:** ICRF → B (frame bias) → P (IAU 2006) → N (IAU 2000A) as the
   frame requires, then R1(ε) for ecliptic coordinates.
6. **Rates:** the whole pipeline at *t* ± 0.001 day; the vector
   difference over the actual rounded step gives the rectangular
   velocity, from which the spherical rates follow analytically. Rates
   therefore describe the *apparent* coordinates (aberration and nutation
   changes included) and are consistent with differencing positions.

Cost (`-O2`, DE440): 6 µs per position, 19 µs with rates; a ten-body
chart at one instant ~0.2 ms (nutation is evaluated once per epoch and
cached).

## Small bodies: the catalog overlay

`add_catalog(path)` stacks EPM1 containers ([FORMAT.md](FORMAT.md));
`calc` resolves a body the planetary ephemeris does not know (by
SPK-ID, the catalog's key) through on-demand integration. Several
catalogs may be stacked; the newest wins for a given body, and adding
one invalidates the memoized trajectories.

- **Seed:** the record's osculating elements (heliocentric, ecliptic
  and equinox of J2000, TDB epoch) become a Cartesian state, rotated
  to ICRF by (R1(ε̄₀)·B)ᵀ and translated by the Sun's barycentric state
  at the epoch.
- **Force model** (`include/prometheia/forces.hpp`,
  `BarycentricForce`): barycentric point masses — the Sun, Mercury…
  Pluto at their *system barycentres* (where the DE GMs live), Earth and
  Moon split from the Earth-Moon barycentre by EMRAT. States are
  sampled from the engine's own ephemeris onto cubic-Hermite tables,
  128 samples per 365.25-day block, extended lazily as integration
  windows march; masses the opened ephemeris does not carry are skipped
  (a kernel without the Moon perturbs without it). GMs come from the
  ephemeris's own constants when it publishes them (DE headers carry
  GM1…GM9, GMB, GMS in AU³/day²), else the DE440 values in `forces.hpp`
  (relative differences ~1e-9).
- **Memo:** one M2 `WindowMemo` per body — year windows of integrated
  samples, warm evaluations are spline reads. The integration error is
  ~1e-8 AU over ±26 yr (0.001″); practical accuracy is set by the
  catalog's elements. Keplerian singulars (e = 1) are rejected at the
  container, so the engine never sees them.
- **Provenance:** catalog answers name the overlay
  ("… + EPM1 catalog(s) […]"), `sigma_arcsec` is filled by a later M4
  increment.
- Costs of the query epoch outside the planetary ephemeris's coverage,
  or a body absent everywhere, are errors — never extrapolations.

## Validation

`tests/test_engine.cpp`.

**Synthetic, everywhere (CI):** closed-form checks of the primitives
(20.49″ aberration perpendicular to 29.79 km/s, none along it; 4.07 mas
and 1.75″ deflection; frame-bias matrix orthogonal with the published
offsets), then the full engine on a synthetic SPK kernel whose bodies
move linearly, where every answer is exact: geometric vectors and rates,
the light-time equation |x(t − τ) − x_obs(t)| = cτ, the apparent vector
rebuilt from the astrometric one through the primitives (< 10⁻⁷″),
J2000 = B·ICRF, true − mean ecliptic longitude = Δψ with equal latitudes,
equatorial of date = ecliptic of date rotated by the true obliquity,
heliocentric/barycentric/topocentric centres, ΔT handling in `calc_ut`,
and the error paths (observer as body, unknown body, coverage, non-finite
time, unopened engine, missing and non-ephemeris files).

**DE440 against the Swiss Ephemeris** (output-only oracle,
`tests/engine_fixtures.inc` from `tools/gen/gen_engine_fixtures.py`):
`swetest` runs on the *same* DE440 binary (`-ejpl<file>`), so residuals
are model differences alone. Ten bodies at 1800, 2000, 2026 and 2100,
sexagesimal output resolved to 0.0001″:

| mode | Sun & planets | Moon |
|------|--------------:|-----:|
| apparent, J2000 ecliptic | 0.0001″ | 0.0011″ |
| geometric, J2000 | 0.0001″ | 0.0007″ |
| barycentric geometric, J2000 | 0.0002″ | 0.0001″ |
| heliocentric geometric, J2000 | 0.0001″ | 0.0001″ |
| apparent, true ecliptic of date | 0.0024″ | 0.0025″ |
| apparent, true equator of date | 0.0031″ | 0.0031″ |
| topocentric apparent (SWE's ΔT) | 0.0027″ | 0.134″ |

Distances agree to ≤ 5.5 × 10⁻¹⁰ AU (SWE prints 10⁻⁹ AU), topocentric to
2.2 × 10⁻⁹ AU. Rates match differenced SWE positions to 2 × 10⁻⁶ °/day
(the fixtures' resolution).

Understood differences — ours follows the published definitions in each
case:

- **Date frames, ~2–6 mas:** SWE's default precession is not IAU 2006
  (it uses a long-term model); the residual is uniform across bodies and
  grows away from J2000 (6 mas at 1600). The J2000-frame rows show the
  correction pipeline itself agrees at the print resolution.
- **Topocentric Moon, ≤ 0.13″:** SWE's site is offset from ours by about
  the nutation angle at the poles (±0.27 km in the Moon distance at
  latitude ±90°), i.e. it rotates the site about the mean rather than the
  true pole. Planets are unaffected (0.003″).
- **Printed rates:** SWE's own speed columns disagree with differences of
  its own positions (Sun latitude rate −0.0021″/day printed vs −0.025″/day
  differenced at J2000; Uranus distance rate off by 3 × 10⁻⁵ AU/day, the
  Earth's acceleration times Uranus' light time). The gate therefore
  differences SWE *positions*.
- **Heliocentric apparent, up to 0.8″ (Mercury):** SWE's heliocentric
  light time is ~1% larger than the Sun→body distance gives. Geometric
  heliocentric positions agree to 0.0001″.
- **ΔT:** SWE's ΔT differs from ours outside the observed era (68.82 vs
  69.12 s in 2026-09, 93 vs 203 s at 2100; see [TIME.md](TIME.md)).
  The topocentric fixtures therefore run with SWE's values through a
  fixed `DeltaTModel`.

A JPL Horizons corpus (M5) will be the independent referee for the
topocentric and heliocentric cases.

**Catalog overlay, DE440 against the Swiss Ephemeris asteroid file**
(`tests/test_engine_catalog.cpp`, `PROMETHEIA_DE440`; fixtures from
`tools/gen/gen_catalog_fixtures.py`, which runs swetest on the same
DE440 with SWE's independent `seas_18.se1` Ceres — an output-only
oracle, offline): Ceres from `tests/data/sample-100.epm` at J2000,
the catalog epoch, 2026-09, and ±11 yr:

| quantity | residual |
|----------|----------|
| apparent, ecliptic of date | 0.19″ |
| geometric, ecliptic J2000 | 0.19″ |
| distance | 2.2 × 10⁻⁶ AU |

The residual is dominated by the difference between SBDB's osculating
elements (our seed) and SWE's stored integration (their source); the
engine's own integration contributes ~0.001″ over the same span. The
synthetic CI tests (same file, no data files needed) gate the force
model against the closed-form two-body solution and the whole overlay
pipeline against an independent integration of the same force model to
10⁻⁸ AU, in both time directions.

# Engine: positions API and apparent place

`prometheia::Engine` (`include/prometheia/engine.hpp`, `src/engine.cpp`)
is the query surface: one planetary ephemeris in, `calc(body, time,
options)` out. The correction formulas live in
`include/prometheia/apparent.hpp` as pure functions; the frame chain is
[FRAMES.md](FRAMES.md); the ephemeris readers are [DE.md](DE.md) and
[SPK.md](SPK.md). C callers and bindings use the same engine through
[C_API.md](C_API.md).

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
  for small bodies whose records carry a full covariance (below).
  `ayanamsa_deg` carries the longitude shift applied for a sidereal
  request (absent for a tropical one).
- **Threading:** an `Engine` caches ephemeris records and the frame
  matrices of the last three epochs; it is not safe for concurrent use.
  One engine per thread; there is no global state.

## Options

| field | values | default |
|-------|--------|---------|
| `center` | `Geocentric`, `Topocentric` (+ `site`, WGS84), `Heliocentric`, `Barycentric` | geocentric |
| `frame` | `ICRF`, `J2000` (ICRF + frame bias), `MeanOfDate` (+ IAU 2006 precession), `TrueOfDate` (+ IAU 2000A nutation) | true of date |
| `coords` | `Ecliptic`, `Equatorial` | ecliptic |
| `sidereal` | `Tropical`, `FaganBradley`, `Lahiri`, `User` (+ `sidereal_epoch_jtdb`, `sidereal_ayanamsa_deg`), and eleven zodiacs defined at the instant: `TrueCitra` … `GalacticEquatorMula` ([FRAMES.md](FRAMES.md), "Zodiacs defined at the instant") | tropical |
| `sidereal_plane` | `EclipticOfDate`, `EclipticOfAnchor`, `Invariable` (fixed planes: [FRAMES.md](FRAMES.md), "Sidereal planes") | ecliptic of date |
| `precession` | `IAU2006`, `Vondrak2011` (long-term; [FRAMES.md](FRAMES.md)) | IAU 2006 |
| `light_time`, `deflection`, `aberration` | independent switches | all on |
| `speed` | rates by central difference (3× the work) | on |
| `sigma` | catalog bodies' `sigma_arcsec` (12 extra integrations per body) | on |

Presets: `CalcOptions::apparent()` (the defaults), `astrometric()` (light
time only), `geometric()` (no corrections).

Ecliptic output in `ICRF` and `J2000` uses the J2000 mean obliquity
(84381.406″); in the date frames the mean obliquity of date. The true
ecliptic of date is the mean ecliptic plane with the true equinox — the
ecliptic does not nutate, only the equinox slides by Δψ ([FRAMES.md](FRAMES.md)).

## Sidereal output (ayanamshas)

`CalcOptions::sidereal` selects a sidereal zodiac for the output:
`FaganBradley` and `Lahiri` (numbered like swetest's `-ay<mode>`), or
`User`, anchored at `sidereal_epoch_jtdb` / `sidereal_ayanamsa_deg` —
the **mean** ayanamsha at that TT epoch, the same convention as
swetest's `-sidudef`. The ayanamsha at the query epoch is the anchor
value plus the IAU 2006 general precession in longitude accumulated
since (mean), plus the nutation in longitude (true), measured on the
ecliptic of date — the traditional realization
([FRAMES.md](FRAMES.md)).

Applied to the output:

- the **of-date frames** rotate the ecliptic longitude zero point west
  by the ayanamsha — the true ayanamsha in the true frames, the mean in
  the mean frames. Latitude and distance are untouched; the daily rates
  carry the ayanamsha motion (~50.3″/yr).
- **ICRF / J2000** output places the zodiac's zero point at its fixed
  longitude on the mean ecliptic of J2000: one constant offset from the
  tropical output at every epoch.
- **equatorial** output is the same longitude rotation about the
  ecliptic pole: (λ − ayanamsha, β) converted through the frame's
  obliquity.

`CalcResult::ayanamsa_deg` reports the shift that was applied. Two
deliberate differences from the Swiss Ephemeris, both measured on its
output: its sidereal right ascension/declination columns are not
shifted at all (the ayanamsha reaches only the ecliptic longitude, and
the positions drop nutation — they are mean-of-date tropical), and its
sidereal J2000 output subtracts ayanamsha(t) + p_A(t), the of-date
ayanamsha inside a J2000 frame; ours keeps the fixed zero point.

The anchor instants are the published definitions — Fagan/Bradley,
1 Jan 1950; Lahiri, 21 Mar 1956 0h TD (the Calendar Reform Committee
epoch, IAE 1985). The tabulated anchor *values* are the Swiss Ephemeris
output at those instants: the constants quoted in the literature for
the same instants (24°02′31.36″ and 23°15′00″.658) differ from its own
output by 3.7″ and 0.14″ — more than any model difference over ±200 yr.

## Pipeline

For a TT epoch *t*:

1. **Observer** barycentric state at TDB(*t*): the Earth, the Sun, the
   barycentre (zero), any body (`Center::Body` with
   `CalcOptions::center_body`: a planet-centred observer, from the
   ephemeris or a catalog, moving with the body; the Sun's gravitational
   deflection is skipped only when that body is the Sun or the
   barycentre), or for topocentric the Earth plus the WGS84 site
   rotated by GAST (UT1 from the ΔT model) into the true equator of date
   and carried to ICRF by (N·P·B)ᵀ, with site velocity ω × r (ω = the ERA
   rate, 2π · 1.00273781191135448 rad/day). Polar motion is neglected
   (≤ 0.3″ in the site, sub-mas at the Moon).
2. **Light time:** the body's barycentric position at *t* − τ with
   τ = |x_body(t − τ) − x_obs(t)| / c, solved by Newton's method (the
   derivative from the body's velocity) to |Δτ| < 10⁻¹⁵ day, usually in
   2–3 ephemeris reads. The retarded epoch
   `jd − τ` is formed with its exact rounding error recovered (TwoSum) and
   applied through the body's velocity: a JD double near the present
   resolves only ~40 µs, which would otherwise put metre-class noise into
   the differenced rates. Orbit points do the same through their focus
   (ORBIT-POINTS.md, "How it is computed").
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
6. **Rates:** the whole pipeline at *t* ± 1/4096 day (21 s); the vector
   difference over the actual rounded step gives the rectangular
   velocity, from which the spherical rates follow analytically. Rates
   therefore describe the *apparent* coordinates (aberration and nutation
   changes included) and are consistent with differencing positions.
   - **Measured** against a five-point difference of the engine's own
     positions (2026-09-18; Sun, Moon, Mercury, Mars, Jupiter; four epochs,
     hourly over a day). Geocentric: 3.4e-8 °/day for the Moon, 2e-10 or
     better for the others. Topocentric: 2.9e-6 °/day for the Moon, 1e-8
     for the others.
   - **Why this step:** a topocentric Moon's diurnal parallax sets the
     three-point truncation. At 0.001 day it was 2.6e-5 °/day; smaller
     steps than 1/4096 lose to roundoff. The cross-test's `rates` leg
     checks this through the server.

Cost (`-O2`, DE440, measured 2026-09-17 on ten bodies × 10,000 hourly
instants):
- **Per position:** 1.9 µs without rates, 4.0 µs with rates when each
  body is swept over the window in turn. Evaluating every body at one
  instant before the next brings it to 2.95 µs, which is what prometheiad
  does. It was 53 and 55 µs before the changes below.
- **Nutation** is interpolated from nodes on a fixed half-day grid
  (`frames::NutationInterpolator`). Each node holds the 1365-term series
  and its analytic first and second derivatives, and an epoch takes the
  quintic Hermite polynomial through the two surrounding nodes. The error
  is 0.004 µas. The value depends only on the epoch. The engine keeps 4,096
  nodes (about 5.6 years), so a many-body sweep sums the series once per
  half day in total.
  - The previous scheme, a Taylor step from a 0.05-day grid, was 0.68 µas.
    It re-summed the series almost every hourly row, which was 92% of the
    cost.
- **Light time** is solved by Newton's method (step 2). The rate stencil
  starts from the centre's τ.
- **Per-instant memos:** the observer's state and the Sun's state are kept
  for the last three instants, keyed exactly (topocentric entries also on
  ΔT), so bodies at one instant and its stencil read them once.

## Nodes and apsides

`calc_orbit_point(body, point, elements, jd_tt, opts)` (and `_ut`) answers
a node or apsis of a body's orbit as a point in space. It is seen by the
options' observer in the options' frame and zodiac, with rates by the same
central differences as `calc`.

- **The orbit.**
  - For planets (system barycentres for Mars–Pluto), the Earth, the
    Earth–Moon barycentre and catalog bodies: heliocentric.
  - For the Moon: geocentric.
  - `OrbitElements::Interpolated` is the Moon's natural apogee and perigee,
    interpolated between its actual passages
    ([ORBIT-POINTS.md](ORBIT-POINTS.md), "The natural apsides").
  - The Sun and the barycentre have none (ArgumentError).
- **The reference plane:** the mean ecliptic of date, in every frame. A
  node is where the orbit crosses it; the output frame gives only the
  coordinates the point is expressed in. This is protocol v4 §3.5a as
  amended on 2026-09-18, approved by both maintainers. Before that, a node
  asked in J2000 or ICRF lay on the J2000 ecliptic, a different point: up
  to 680″ away on the Moon's mean node at 1800 and 2100. For an astrologer
  the node of date is the one that matters, because it is where eclipses
  fall.
- **The corrections apply as sent, exactly as to a body.** Light time (the
  point's slow fixed-point), deflection (Sun-observer skip) and aberration
  mean the same here as in `calc`, and `CalcOptions::geometric()` answers
  the bare point, unchanged to the bit. A point exists to be compared with
  apparent positions, so it answers in the frame those are in; see
  [ORBIT-POINTS.md](ORBIT-POINTS.md) for the rule, the measured magnitudes
  (the observer-velocity term is 21″ on Jupiter's node where light time is
  0.0003″) and the joint version-4 settlement. `sigma_arcsec` is never set.
- **Points:**
  - ascending node, the northward crossing;
  - descending node;
  - perihelion (perigee for the Moon);
  - aphelion (apogee).
- **Errors (ArgumentError):**
  - nodes of an orbit lying in the ecliptic;
  - apsides of a circular orbit;
  - the aphelion of an open orbit;
  - a node an open orbit never reaches.

**Osculating** (`OrbitElements::Osculating`): the conic through the body's
geometric state at the instant, relative to the Sun (Earth for the Moon).
- **Mass:** μ is the sum of the two masses (the ephemeris' GMs, else
  built-in values).
- **Method:** with the state rotated into the reference ecliptic, h = r × v
  and the eccentricity vector e = v × h / μ − r̂. The ascending node lies
  along ẑ × h and the perihelion along e. A point in direction û is at
  distance p / (1 + e·û), with p = |h|² / μ.
- **Validation:**
  - Ceres at its catalog element epoch reproduces its SBDB elements: node
    0.16″ (the output frame's J2000 ecliptic is 0.04″ from JPL's),
    perihelion direction 0.012″, perihelion distance 1e-9 AU.
  - Synthetic-kernel tests check the conic's own relations: nodes on the
    ecliptic and opposite, apsides opposite, (q + Q)/2 equal to vis-viva's a.
  - Mars on DE440 at 2026-09-17: node 49.48°, perihelion longitude 336.12°,
    q 1.3813 AU, Q 1.6660 AU (J2000 ecliptic).
- **Behaviour:** the Moon's osculating perigee swings by degrees a day,
  which is the physical orbit's behaviour; the mean elements smooth it.

**Mean** (`OrbitElements::Mean`): the orbit without its periodic terms.
It is available for the Moon and the major planets (NotFound otherwise).
- **Moon.**
  - The node is the fundamental argument Ω and the perigee's longitude is
    ϖ = L − l, with L = F + Ω. These are the Simon et al. (1994) expressions
    in USNO Circular 179, the same ones the nutation series uses, referred to
    the mean ecliptic and equinox of date.
  - Inclination 5.1453964°, eccentricity 0.0549006 and mean distance
    384,399 km are fixed values; they only set the points' distances and
    latitudes.
  - On the mean ecliptic of date the mean node's longitude equals Ω to
    1e-6″. Its rate is −0.05295° a day.
- **Planets** (Mercury, Venus, the Earth–Moon barycentre also answering for
  the Earth, and the Mars–Pluto systems; planet-centre IDs share their
  system's elements).
  - The fit: a quadratic in T for each element, fitted by
    `prometheia-gen-mean-elements` (`tools/gen/gen_mean_elements.cpp`) to
    DE440's own osculating heliocentric elements every 4 days over
    1550–2650. The coefficients are compiled in as `src/mean_elements.inc`.
  - The elements fitted are a and the non-singular h = e sin ϖ,
    k = e cos ϖ, p = sin i sin Ω, q = sin i cos Ω. These stay well defined
    for the Earth–Moon barycentre's near-zero inclination to the J2000
    ecliptic and for small eccentricities.
  - Short-period terms average out over 1,100 years, and what is left is
    the secular trend. The table header and the generator's output record
    each fit's residual rms.
  - At J2000 the fit gives the familiar mean elements: Mercury a 0.387098
    AU, e 0.205632, i 7.00498°, Ω 48.3309°, ϖ 77.4561°; the EMB's ϖ
    102.9371°; Mars Ω 49.5581°; Pluto ϖ 224.08°. Jupiter's ϖ is about 0.2°
    from classical values: the 900-year great inequality leaks into a fit
    this short.
  - The elements are referred to the J2000 ecliptic. For output in the date
    frames the orbit is carried to the ecliptic of date before its nodes are
    taken.
  - The Earth's own orbit defines the ecliptic, so its nodes on the ecliptic
    of date are nearly degenerate and move quickly.
  - Regenerate the table with `prometheia-gen-mean-elements DE_FILE
    src/mean_elements.inc` (2.7 s) when the planetary ephemeris changes.

## Fixed stars

`calc_star(index, jd_tt, opts)` computes an object of the compiled-in
fixed-star and Messier catalog (`prometheia/stars.hpp`: lookup by any name
or designation). Space motion, parallax, the Sun's deflection and aberration
lead into the same output frames and zodiacs as bodies. The details and the
validation against ERFA (0.34 mas) are in [STARS.md](STARS.md).

## Small bodies: the catalog overlay

`add_catalog(path)` stacks EPM1 containers ([FORMAT.md](FORMAT.md));
`calc` resolves a body the planetary ephemeris does not know (by
SPK-ID, the catalog's key) through on-demand integration. Several
catalogs may be stacked; the newest wins for a given body, and adding
one invalidates the memoized trajectories.

Loading a catalog streams and CRC-verifies the whole container (0.56 s,
17 MB for the 1.57M-body catalog). Its primary designations and proper
names are indexed on the first `lookup()` (0.7 s, ~25 MB: sorted 64-bit
name hashes, each hit confirmed against the record's own names, so hash
collisions cannot answer a wrong body):
`Engine::lookup(name)` — `lookup("Ceres")`, `lookup("1")`,
`lookup("ceres")`, ASCII-case-insensitive — returns the SPK-ID to feed
`calc`. A shared name is answered by the newest catalog; planets are
not indexed (address those by their NAIF IDs).

- **Seed:** the record's osculating elements (heliocentric, JPL's
  ecliptic and equinox of J2000, TDB epoch) become a Cartesian state,
  rotated to ICRF by R1(84381.448″)ᵀ and translated by the Sun's
  barycentric state at the epoch. JPL's J2000 ecliptic (SBDB, Horizons,
  SPICE `ECLIPJ2000`) is the ICRF rotated by the IAU 1976 obliquity
  with no frame bias — not the IAU 2006 mean ecliptic of the engine's
  `J2000` output; using the latter misplaced seeds by ~50 km
  ([VALIDATION.md](VALIDATION.md)).
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
  (relative differences ~1e-9). The Sun also contributes its
  post-Newtonian (Schwarzschild, PPN β = γ = 1) term in test-particle form,
  μ/(c²r³)[(4μ/r − v²) **r** + 4(**r**·**v**) **v**], relative to the Sun:
  the relativistic perihelion advance (tested against the closed form
  6πμ/(c²a(1−e²)) per orbit to 1 part in 10⁴).
- **Asteroid perturbers** (`add_perturbers(path)`, optional): an SPK
  kernel of heliocentric numbered-asteroid segments (JPL's
  `sb441-n16.bsp`: Ceres, Pallas, Juno, Vesta, Iris, Hygiea, Eunomia,
  Psyche, Euphrosyne, Europa, Cybele, Sylvia, Thisbe, Camilla, Davida,
  Interamnia — what Horizons integrates with). Each body with a known mass
  (the DE file's `MAnnnn` constant, else the DE440 values built in) is
  sampled into the perturber tables like a planet, its barycentric state
  being the kernel's heliocentric one plus the ephemeris's Sun. A catalog
  body never perturbs itself (its SBDB SPK-ID 20000000 + n matches the
  kernel's 2000000 + n). The kernel's own trajectories are not used as
  answers: they come from 2021 orbit solutions and measured 1–125 km off
  Horizons' current ones near the present, while integrating the current
  catalog elements with the kernel as perturbers stays within ~5 km there
  and gains 20–500× at ±100 yr ([VALIDATION.md](VALIDATION.md)). Twelve
  more perturbers make integration ~2× slower.
- **Memo:** one M2 `WindowMemo` per body — year windows of integrated
  samples, warm evaluations are spline reads. The integration error is
  ~1e-8 AU over ±26 yr (0.001″); practical accuracy is set by the
  catalog's elements. Each force evaluation reads all perturber tables through one
  interval lookup: the tables share uniformly spaced epochs, so the
  sample index is computed rather than searched and one set of Hermite
  weights serves every mass (2.75× faster than per-body binary searches,
  bit-identical results). Within a window, each of the 128 sample
  segments starts the step-size controller at the full segment span
  rather than a hundredth of it, which removes the step ramp-up at every
  sample: 2.4× fewer seconds for the same ±100-year arcs, with the memo's
  closed-form error falling from 1.1e-9 to 8.9e-11 AU over 1000 days. Keplerian singulars (e = 1) are rejected at the
  container, so the engine never sees them.
- **Uncertainty (`sigma_arcsec`):** records carrying the orbit
  solution's full covariance (EPM1 `kCovariance`: JPL's 6×6 in cometary
  elements e, q, tp, Ω, ω, i at its own epoch; [FORMAT.md](FORMAT.md))
  give `CalcResult::sigma_arcsec`: the square root of the larger
  eigenvalue of the position covariance projected on the sky plane
  (perpendicular to the observer→body line), divided by the
  observer→body distance.
  - *Propagation:* the covariance is scaled to a correlation matrix
    (element variances span ~20 decades), decomposed into principal
    axes (Jacobi), and each axis scaled to one sigma becomes a pair of
    seeds at the covariance epoch, x₀ ± s·v_k, integrated in their own
    windowed memos beside the nominal one. The central differences'
    outer products sum to J·C·Jᵀ exactly in the linear regime. s = 1
    (the one-sigma points), raised when that moves the seed by less than
    1e-7 AU (double and integrator noise) and lowered to keep e on its
    side of 0 and 1 and q positive.
  - *Absent* for records without a covariance — per-element sigmas
    (`kSigmas`) are uncorrelated summaries that measured 10–1000× too
    large against Horizons, so they are not used — and for planetary
    bodies; zero for an all-zero covariance; absent, with the position
    intact, when a perturbed track cannot reach the epoch.
  - Eigenvalues are rotation-invariant, so the value is frame-independent
    and the light-optics corrections never enter. It costs up to twelve
    extra integrations per body; `CalcOptions::sigma = false` skips
    them.
- **Provenance:** catalog answers name the overlay
  ("… + EPM1 catalog(s) […]").
- Costs of the query epoch outside the planetary ephemeris's coverage,
  or a body absent everywhere, are errors — never extrapolations.

## Performance

`prometheia-engine-bench DE_FILE [CATALOG.epm] [--only NAME]` times the
public calls, per call, over 200 instants scrambled across 1900–2100, as the
median of five runs. The small-body scenario is 20 instants. Measured on
this machine (i7-8700K) with DE440 and the full SBDB catalogue, 2026-09-19.
Runs vary by about 20% with the CPU's clock, so compare within one sitting.

| scenario | µs per call |
|---|---|
| planets, apparent (rates on) | 3.9 |
| planets, rates off | 2.5 |
| planets, topocentric / Lahiri / true Citra | 4.1 / 4.0 / 5.4 |
| ten stars, apparent | 3.2 |
| the Moon's mean or osculating node and apogee | 18–21 |
| the Moon's natural apogee | 33 |
| the eight Hamburg points | 7.4 |
| Ceres, Eris, Sedna (after their first integration) | 9.4 |

At a fresh instant most of a chart's cost is nutation. The interpolator
evaluates the full 1365-term series, with rates, at the two half-day nodes
around the instant: 18.4 µs each. Under `prometheia-load` it was 43% of the
server's CPU (2026-09-19).

`add_catalog` of the full catalogue (137 MB) takes 315 ms: it decompresses
and CRC-checks every chunk, so a corrupt file fails there.

**Found and fixed, 2026-09-19** (the same outputs, byte for byte):
- **A small body's record was decoded on every call.** The record cache
  hit looked the record up again to learn its catalogue. Ceres, Eris and
  Sedna went from 2,712 µs a call to 14.
- **Walking backward in time was quadratic.** The trajectory memo and the
  perturbers' table both kept one ascending array and inserted each
  earlier window at its front, moving everything already built. A memo now
  keeps two runs that meet at its seed, and the table keeps headroom at
  its front.
- **CRC-32 went a byte at a time.** Slicing-by-8 halves `add_catalog`
  (634 → 315 ms).
- Together: one JSON call for three distant small bodies at 1,000 instants
  over 1,000 years went from 19.2 s to 4.4 s, with the same 2.2 MB answer.
- **The natural apsides' passages** are scanned in fixed blocks and kept
  (ORBIT-POINTS.md): 266 → 33 µs at scattered instants.
- **The nutation series with rates** reuses each term's value and
  quadrature for its derivatives, and reads signed multiples without a
  branch: 22.6 → 18.4 µs a node. The values and first derivatives are
  bit-identical, and the second derivatives agree to 2e-22 rad/day².

## Validation

`tests/test_engine.cpp`.

**Synthetic, everywhere (no data files):** closed-form checks of the primitives
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
  heliocentric positions agree to 0.0001″. JPL Horizons agrees with ours
  (Sun-centred astrometric to 6 µas), so the difference is SWE's.
- **ΔT:** SWE's ΔT differs from ours outside the observed era (68.82 vs
  69.12 s in 2026-09, 93 vs 203 s at 2100; see [TIME.md](TIME.md)).
  The topocentric fixtures therefore run with SWE's values through a
  fixed `DeltaTModel`.

JPL Horizons is the independent referee
([VALIDATION.md](VALIDATION.md)): astrometric positions agree to 6 µas
(topocentric 11 µas), apparent place of date to ~1 mas once Horizons'
documented IAU 1976/80 equinox offset is removed, and the topocentric
Moon to 0.008″ — the SWE differences above are SWE's.

**Sidereal ayanamshas** (`tests/sidereal_fixtures.inc`, from
`tools/gen/gen_sidereal_fixtures.py`; swetest `-ay<mode>` and
`-sid<mode>` on the same DE440, output-only oracle): the ayanamshas
themselves agree to **0.0026″** over 1800–2200 (0.001″ since 1900 —
the residual is SWE's long-term precession model against IAU 2006, a
time-only difference identical for both modes), and apparent sidereal
positions of the Sun, Moon and planets to **0.0034″**. The synthetic
tests additionally pin the conventions: sidereal longitude is the
tropical minus the ayanamsha exactly, the mean frames shift by the mean
value, equatorial sidereal output is the same rotation about the
ecliptic pole, and J2000 sidereal keeps one fixed offset from J2000
tropical.

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
synthetic tests (same file, no data files needed) gate the force
model against the closed-form two-body solution and the whole overlay
pipeline against an independent integration of the same force model to
10⁻⁸ AU, in both time directions.

**sigma_arcsec** (same file), on the synthetic kernel:
- a tp-only covariance reproduces the closed form σ_tp·|u × v|/|r_bary|
  at the covariance epoch (1.153272″ both, 1e-4 gate);
- a fully correlated covariance published 300 days before the element
  epoch matches an independent oracle that never decomposes it — J built
  by per-element finite differences (free-running dp54 against the kernel
  read per evaluation), then J·C·Jᵀ explicitly — to all printed digits
  at 0, +700 and −900 d (2e-3 gate);
- anticorrelated node and argument of perihelion on a nearly coplanar
  orbit (their sum well known) shrink the answer from 29.4″ to 0.09″,
  the effect the per-element sigmas miss;
- a q-only covariance grows along-track near-symmetrically in time; a
  100× covariance rescales the answer by 9.999999;
- element sigmas without a covariance stay absent; a zero covariance
  gives exactly zero.

# Validation against JPL Horizons

JPL Horizons is the independent referee (M5). The Swiss Ephemeris comparisons
in [ENGINE.md](ENGINE.md) show agreement with another implementation. Horizons
is JPL's own reduction of the same DE ephemerides and SBDB orbits, so it
separates *our* errors from *model choices*.

That distinction is the whole reason this corpus exists, and it is why
[CROSS-TEST.md](CROSS-TEST.md) makes Horizons the adjudicator rather than the
other server: agreement with another implementation is evidence, but two
implementations can converge on the same wrong answer, and once they have,
nothing in either codebase can tell. Note in particular the two rows below
that are *not* usable as an anchor — Horizons' apparent place carries a
documented −53 mas equinox offset and a 42 mas obliquity difference, and its
Sun-centred "apparent" is referred to the Sun's equator.

- **Corpus:** 39 requests, declared in `tools/fetch/horizons_fetch.py` and
  fetched on 2026-09-17, strictly sequentially with 5 s pauses.
  `tools/gen/gen_horizons_corpus.py` turns the responses into
  `tests/horizons_corpus.inc`: 245 observer rows and 63 vector rows, with
  per-request provenance. Horizons output is a US-government work.
- **Test:** `tests/test_horizons.cpp`, in the pre-commit gate. It needs the
  DE440 binary under `ephe/` and SKIPs without it. It takes 0.2 s.
- **Horizons' sources:** DE441 for planets. For small bodies, it integrates
  SBDB solutions (Ceres: JPL#48, the same solution as our sample catalog) with
  16 asteroid perturbers (SB441-N16) and relativity. Earth orientation comes
  from IERS EOP, 1962-01-20 to 2026-09-16.

## Results

### Positions that depend on no model choice

These are gated at every epoch (1800–2100) and from all observers.

| quantity | measured max | gate |
|---|---:|---:|
| Sun & planets, astrometric RA/Dec (ICRF, light time), geocentric + Sun-centred | **6 µas** | 100 µas |
| Sun & planets, astrometric, topocentric (3 sites) | **11 µas** | 100 µas |
| Moon, astrometric, geocentric | 0.021″ (at 1800; ≤ 0.006″ 1900–2100) | 0.03″ |
| Moon, astrometric, topocentric (1981–2026) | 0.008″ | 0.015″ |
| light-time range, Sun & planets geo/helio · topo · Moon | 1.1 m · 3.6 m · 5.7 m | 3 · 10 · 10 m |

- **Light time and frame:** the astrometric agreement means the light-time
  solution, observer geometry and ICRF frame agree with JPL at the µas level.
  What remains is DE440 vs DE441.
- **Topocentric observers:** these include Longyearbyen at 78°N and Paranal at
  2.6 km. They run with Horizons' own Earth rotation. UT1 is solved from its
  local apparent sidereal time, because its TDB−UT column is TDB−UTC after
  1962.

### Apparent place of date (1962–2026)

Horizons refers apparent RA/Dec to the EOP-corrected IAU 1976/80 equinox,
which it documents as offset −53 mas from the IAU 2006/2000A equinox we use.
Its ecliptic of date also uses the IAU 1980 obliquity, 42 mas larger. Once
those published offsets are removed:

| quantity | measured max | gate |
|---|---:|---:|
| dRA − 51.8 mas (Sun & planets) | 1.0 mas | 3 mas |
| dDec (Sun & planets) | 0.8 mas | 2 mas |
| Moon dRA − offset · dDec (geo + 3 topocentric sites) | 3.4 · 8.0 mas | 6 · 10 mas |
| ecliptic dLon·cos b − 48 mas · dLat | 8.7 · 45.7 mas | 12 · 50 mas |

- **Precession and nutation:** these confirm our IAU 2006/2000A
  implementation, aberration, deflection and topocentric reduction to the
  mas level.
- **Outside Horizons' EOP era (1800, 1900, 2050, 2100):** Horizons falls back to
  plain IAU 1976/80, and the apparent positions differ from ours by up to
  0.52″. That is the older precession model, and those rows are reported only.
- **Sun-centred observers:** Horizons refers "apparent" RA/Dec to the **Sun's
  equator**. Latitudes match a rotation to the solar pole (286.13°, 63.87°)
  exactly, and longitudes differ by a constant 14.56°. Those quantities are not
  comparable, so only the Sun-centred astrometric rows are used.

### Small bodies

Six numbered asteroids, all from `tests/data/sample-100.epm` — Ceres, Pallas,
Vesta, Iris, Hygiea and Cybele — plus the scattered-disk TNO 145451 Rumina
(elements and covariance from `tests/data/covariance-7.epm`; within ±100 yr
it agrees with Horizons to ≤ 21 km, 0.0005″, since planets dominate out there). Epochs are the element epoch (2026-06-08) and
±10, 25, 50 and 100 years.

| span from element epoch | astrometric, rms · max over 6 bodies | gate |
|---|---:|---:|
| 0 | max 0.0016″; heliocentric 0.1–4 km | 0.005″; 10 km |
| ±10 yr | 0.041″ · 0.12″ | 0.15″ |
| ±25 yr | 0.17″ · 0.39″ | report |
| ±50 yr | 0.89″ · 2.5″ (Hygiea) | report |
| ±100 yr | 2.9″ · 8.7″ (Hygiea), 4.4″ (Pallas), others ≤ 1.7″ | report |

With the Sun's post-Newtonian term (added after the first measurement), the
rms fell from 0.066″ to 0.041″ at ±10 yr and from 0.22″ to 0.17″ at ±25 yr.
Beyond that, the missing asteroid perturbers dominated.

**With JPL's asteroid perturbers** (`add_perturbers("sb441-n16.bsp")`, the
16 masses Horizons integrates with):

| span from element epoch | astrometric, max over 7 bodies | heliocentric, typical | gate |
|---|---:|---:|---:|
| 0 | 0.0016″ | 0.1–4 km | 0.005″ |
| ±10 yr | **0.0053″** (was 0.12″) | 0–13 km | 0.01″ |
| ±25 yr | 0.019″ (was 0.39″) | 1–25 km | report |
| ±50 yr | 0.024″ (was 2.5″) | 1–60 km | report |
| ±100 yr | 0.071″ (was 8.7″) | 13–154 km (Hygiea 27,814 → 49 km, Pallas 8,644 → 49 km) | report |

Serving the 16 kernel bodies straight from the kernel was also measured and
rejected: its trajectories come from 2021 solutions, 1–125 km off Horizons
near the present (Pallas 42 km, Cybele 125 km at the epoch), while
integrating the current catalog elements with the other 15 as perturbers is
within 5 km there. The gate `horizons_small_bodies_sb441` (0.13 s) runs when
`ephe/sb441-n16.bsp` (or `$PROMETHEIA_SB441`) is present and SKIPs
otherwise; the long-arc report prints both columns.

- **Frame fix at the element epoch:** the first measurement showed 40–100 km
  here. SBDB elements are in **JPL's J2000 ecliptic**: the ICRF rotated by the
  IAU 1976 obliquity 84381.448″, with no frame bias. The engine had seeded them
  through the IAU 2006 mean ecliptic plus frame bias, 0.042″ + 23 mas apart. It
  now uses JPL's definition, and the residual at the element epoch fell to
  metres–kilometres.
- **Long arcs:** growth beyond ±10 years is our force model. The Sun and planets
  act as point masses plus the Sun's relativistic term, with no asteroid
  perturbers. Horizons integrates with the 16 SB441-N16 asteroids. Hygiea's jump between +25 and +50 years is consistent with a
  close approach to one of the big perturbers.
- **Uncertainties.**
  - *The first measurement* propagated SBDB's per-element 1σ sigmas as
    uncorrelated and gave values roughly 10–1000× larger than Horizons' own
    full-covariance 3σ figures (Ceres 0.04″ against < 0.0005″, Pallas 1.8″
    against 0.014″; Rumina a flat 30–60″ against 0.05–8″). The correlations
    the bulk SBDB query does not return are what constrain these orbits.
  - *Now* `sigma_arcsec` comes only from JPL's full covariance
    (fetched on demand, EPM1 `kCovariance`; absent otherwise). With the
    covariances of the six corpus asteroids and Rumina
    (`tests/data/covariance-7.epm`), JPL's POS_3sigma divided by our
    3·`sigma_arcsec` is 1.00–1.05 at most epochs over ±100 years and never
    outside [0.84, 1.43]. That spread is expected: POS_3sigma is the RSS
    of both error-ellipse semi-axes, ours the major one (ratio in [1, √2]),
    and JPL prints 0.001″ steps. Rumina: 7.98″ against 7.985″ at −100 yr,
    0.04″ against 0.053″ near its observed arc, 6.25″ against 6.251″ at
    +100 yr. Ceres stays below 0.0005″ on both sides.
  - *Gate* (`horizons_sigma_calibration`, 0.4 s): all seven bodies within
    10 years of the element epoch — JPL/ours in [0.97, 1.45] where JPL
    reports ≥ 0.02″, and consistent within JPL's rounding below.
- **Running the report:** the long-arc rows and the uncertainty comparison are
  a report case, skipped by default because the ±100-year and sigma
  integrations take ~4 s (Release):
  `build/test_horizons -tc=horizons_small_bodies_long_arc_report --no-skip`.

## Findings carried back to the Swiss Ephemeris comparison

- **Topocentric Moon:** ours matches Horizons to 0.008″. The 0.13″ SWE
  difference is on SWE's side, consistent with a site rotated about the mean
  pole.
- **Heliocentric light time:** ours matches Horizons' Sun-centred astrometric
  positions to 6 µas. SWE's ~1% longer heliocentric light time, up to 0.8″, is
  on SWE's side.
- **Moon in the J2000 frame:** the 0.0011″ Moon residual against SWE had no
  explanation. Our geocentric astrometric Moon matches Horizons to < 0.0001″
  at 1975, 2000 and 2026, so that residual is SWE's as well.

## Refreshing the corpus

This is a verification fixture, not part of release ingestion: a catalog or ΔT
release does not change it. Refresh it deliberately when what it pins changes:
a new DE, a rebuilt `sample-100.epm`, or a Horizons model change you want to
track. To refresh:

1. Delete `horizons-raw/`.
2. Run `tools/fetch/horizons_fetch.py` (39 requests, ~4 minutes; check
   `--list` first).
3. Run `tools/gen/gen_horizons_corpus.py`.
4. Re-run the gate and review any moved residual before committing.

`--check` on the generator exits 1 when the committed `.inc` differs from the
cache.

# Validation against JPL Horizons

JPL Horizons is the independent referee (M5). The Swiss Ephemeris comparisons
in [ENGINE.md](ENGINE.md) show agreement with another implementation. Horizons
is JPL's own reduction of the same DE ephemerides and SBDB orbits, so it
separates *our* errors from *model choices*.

- **Corpus:** 37 requests, declared in `tools/fetch/horizons_fetch.py` and
  fetched on 2026-09-17, strictly sequentially with 5 s pauses.
  `tools/gen/gen_horizons_corpus.py` turns the responses into
  `tests/horizons_corpus.inc`: 236 observer rows and 54 vector rows, with
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

Six numbered asteroids, all from `tests/data/sample-100.epm`: Ceres, Pallas,
Vesta, Iris, Hygiea and Cybele. Epochs are the element epoch (2026-06-08) and
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
Beyond that, the missing asteroid perturbers dominate, and the 50- and
100-year rms did not move (0.88″ and 2.9″ before).

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
- **Uncertainties:** propagating SBDB's 1-σ element sigmas as uncorrelated
  gives `sigma_arcsec` values roughly 10–1000× larger than Horizons' own full-covariance
  3-σ figures. Ceres: ours 0.04″ against JPL's < 0.0005″. Pallas: 1.8″ against
  0.014″. The correlations the bulk SBDB query does not return are what
  constrain these orbits. Treat `sigma_arcsec` as a loose, pessimistic bound,
  not a calibrated uncertainty.
- **Running the report:** the long-arc rows and the uncertainty comparison are
  a report case, skipped by default because the ±100-year and sigma
  integrations take ~30 s:
  `build/test_horizons -tc=horizons_small_bodies_long_arc_report --no-skip`.

## Findings carried back to the Swiss Ephemeris comparison

- **Topocentric Moon:** ours matches Horizons to 0.008″. The 0.13″ SWE
  difference is on SWE's side, consistent with a site rotated about the mean
  pole.
- **Heliocentric light time:** ours matches Horizons' Sun-centred astrometric
  positions to 6 µas. SWE's ~1% longer heliocentric light time, up to 0.8″, is
  on SWE's side.

## Refreshing the corpus

This is a verification fixture, not part of release ingestion: a catalog or ΔT
release does not change it. Refresh it deliberately when what it pins changes:
a new DE, a rebuilt `sample-100.epm`, or a Horizons model change you want to
track. To refresh:

1. Delete `horizons-raw/`.
2. Run `tools/fetch/horizons_fetch.py` (37 requests, ~4 minutes; check
   `--list` first).
3. Run `tools/gen/gen_horizons_corpus.py`.
4. Re-run the gate and review any moved residual before committing.

`--check` on the generator exits 1 when the committed `.inc` differs from the
cache.

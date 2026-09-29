# Fixed stars — the back burner

**Parked 2026-09-29 by the maintainer:** "we barely use Stars." Effort goes
to solar-system objects. Nothing here is lost: each item says where its
evidence lives and what finishing it would take, so it can be picked up cold.
The Astrolog side parked its own star work the same day, in its
`STARS_BACKLOG.md`; the shared items below point at it rather than repeat it.

What is **not** parked is anything that serves every object and only
happened to be found on a star: the rate-bound reading (|distance-rate
error| / max(1 AU, r), §3.5a since Astrolog `5f11726`) and the rate sweep
that grades it.

## Done, for the record

- **A star's distance rate is computed in closed form** (`dd3d14d`, v0.7.1;
  ENGINE.md "Rates"). It had missed by one f64 ulp of the position, 2.3e-5
  AU/day on Polaris. Confirmed independently by the Astrolog side.
- **Vega's radial velocity** kept at −13.5 km/s, re-confirmed against SIMBAD
  2026-09-29; −20.6 recorded as SIMBAD's 2018 value (STARS.md).
- **α Cen**: closed at `c1cfc4b`. Not to be reopened.
- **Binary-star orbits** for Sirius, Procyon and α Cen A/B (STARS.md).

## Ours to do, if stars come back

1. **SIMBAD has moved since the catalogue's pin.** A re-query on 2026-09-29
   found 250 radial velocities changed and 52 stars newly given one, since
   the 2026-09-17 pin. None adopted. Finishing it: refetch with
   `tools/fetch/stars_fetch.py --force --only simbad-rv`, regenerate
   `src/star_catalog.inc`, review the diff, re-pin (STARS.md, "Re-creating
   the data"). It moves only star distance rates.
2. **No instrument compares two engines on star rates.** Named, deliberately
   unfilled (CROSS-TEST.md, "The instrument gap this fell through"): the
   `stars` leg compares positions, `ratesweep.py` compares one engine with
   itself, and Horizons serves no fixed stars. Filling it needs an outside
   reference for a star's distance rate.
3. **`stars_fk5.py` is run by nothing.** It needs a live server, `stars-raw/`
   and pyerfa; `tools/check/runners.py` names it on every run. Its selftest
   (`starstest.py`) is scheduled; the comparison itself is by hand before a
   release.
4. **Model limits, documented and not worked** (STARS.md, "Known limits"):
   stars without a radial velocity move in proper motion only; stars with
   unseen companions wobble about their straight lines (Achernar 1.0″,
   Polaris 0.78″ from the FK5 at 2100); deep-sky objects are their SIMBAD
   centres.
5. **Anchors for the zodiacs defined at the instant.** The star anchors and
   Sgr A\* come from two catalogues on the two sides, graded at an estimated
   0.1″ (`INSTANT_ANCHOR_BAND`, CROSS-TEST.md). The 15 standing
   `galequ-iau1958` rows are the IAU 1958 pole's ICRS transfer, theirs to
   decide.

## Theirs, parked on their side (Astrolog `STARS_BACKLOG.md`)

Recorded here so a cross-test reader knows what the star rows mean until
they move:

- Their star rates are passed through from their library, not differenced
  from their positions; their advertised 4e-3 AU/day does not cover
  topocentric star rows (their registry §2.11a, §2.12). **Say so beside any
  published comparison of star rates.**
- Vega's radial velocity in their catalogue (−20.6), Toliman and Proxima on
  one catalogue, their α Cen mass split, and their missing FK5 and
  swetest star legs.

## Where the evidence lives

- ENGINE.md "Rates"; STARS.md; CROSS-TEST.md from "The distance tolerance
  cannot be met for a star" through "The instrument gap this fell through".
- `docs/crosstest/2026-09-29-ratesweep-ours-graded.tsv` (stars within the
  default bound under the max(1 AU, r) reading).

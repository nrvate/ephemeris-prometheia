# JPL DE binary reader

`prometheia::de` (in `include/prometheia/de.hpp`, `src/de.cpp`) reads
JPL planetary ephemeris binaries ("DExxx" files). Reading a DE file
means evaluating *their* Chebyshev representation — nothing here
contradicts our own storage policy, which keeps Chebyshev out of the
formats we define (EPM1 stores initial conditions). The same ephemerides
in NAIF's SPK container are read by `prometheia::spk`
([SPK.md](SPK.md)).

## Test corpus

| file | source | size | notes |
|------|--------|------|-------|
| `lnxm1600p2170.200` | JPL, via the Swiss Ephemeris download page | 43 MB | DE200, 1600–2170; tracked at `/shares/swisseph/ephe/de200.eph` (`PROMETHEIA_DE200`) |
| `linux_p1550p2650.440` | `ssd.jpl.nasa.gov/ftp/eph/planets/Linux/de440/` | 98 MB | DE440, 1550–2650 (`PROMETHEIA_DE440`) |
| `testpo.440` | same directory | 838 KB | JPL's official test points for DE440 (`PROMETHEIA_TESTPO440`) |
| `header.440`, `header.200` | JPL ASCII headers | 22 KB | published GROUP 1050 pointer tables, used to pin the tests |
| `linux_m13000p17000.441` | `ssd.jpl.nasa.gov/ftp/eph/planets/Linux/de441/` | 2.79 GB | DE441, −13200..+17191 per its header (`PROMETHEIA_DE441`) |
| `testpo.441` | same directory | 24 MB | JPL's test points for DE441, 364,687 of them (`PROMETHEIA_TESTPO441`) |

The DE440 files live in the repo's gitignored `ephe/` directory (see
`ephe/SHA256SUMS` locally); real-file tests print SKIP when a file is
absent, so a machine without them runs the synthetic part only. All of
them are acquired, and checked against pinned SHA-256s, by
`tools/fetch/de_fetch.py` (`--list`, `--probe` for sizes by HEAD alone,
`--verify`); DE441 was fetched with it on 2026-09-29, to `/nvm/work/ephe/`
on this machine and linked into `ephe/`.

## DE441

JPL's long-span companion to DE440: the same integration, fitted without
the lunar core-mantle damping that DE440 carries, so that it can be run
over 30,000 years. Inside DE440's span JPL recommends DE440.

- **Against JPL's test points**: all 360,001 inside the file's span
  (`testpo.441`'s header claims a wider range than the binary holds) —
  bodies 2.8e-14 AU(/day), nutations 8e-20 rad, librations 5 ulp. Two
  things in that file would fool a quick reader. The first three fields run
  together for a negative year or JD (`441-13200.09.01-3099998.5`), so the
  test scans them rather than splitting on whitespace; and the libration
  angle ψ reaches 1.2 × 10⁶ rad by the ends, where one f64 ulp is
  2.3e-10, so libration errors are graded in ulps of each component's
  largest value rather than absolutely. The gate checks every 20th point
  (`de441_real_matches_jpl_testpo_sampled`); all of them run with
  `-tc=de441_real_matches_jpl_testpo_all --no-skip`.
- **Against JPL Horizons outside DE440's span** (which Horizons also
  answers from DE441; `horizons_astrometric_de441_era`, 10 bodies at
  Julian years −3000, −1000, 0, 1000, 1500, 2700, 5000 and 9000, TT,
  geocentric astrometric ICRF): the Sun and planets agree to ≤ 8.3 µas,
  at Horizons' 3.6 µas print quantum, and 3.8 m in range. The Moon agrees
  to 3–31 µas through year 5000 and 124 µas at 9000, and 0.15 m. The
  Moon's residual is along-track, with a sign that varies by epoch;
  as a timing offset it is 10–50 µs, 0.2 ms at 9000. A difference in
  the TDB−TT series at extreme epochs would fit, but that is an
  estimate, not investigated. The ext-geo requests are declared in
  `tools/fetch/horizons_fetch.py`.
- **Against DE440 where both run** (61 epochs 1550–2650, astrometric
  ICRF): the Sun and every planet agree to ≤ 0.013 mas and a few metres.
  **The Moon does not**: 0.36″ at 1550, 0.08″ at 1700, 0.003″ at 1900,
  < 1 mas 1950–2026, 0.006″ at 2100, 0.06″ at 2650 — the core damping,
  growing away from the era of lunar laser ranging.
- **So the engine serves both, each where it is best**
  (`Engine::add_ephemeris`, C `prometheia_engine_add_ephemeris`,
  `--ephemeris` given twice to `prometheiad` and `prometheia-json`, `-e`
  twice to `ephem`): DE440 first answers 1550–2650 exactly as it does
  alone (tested to the bit), and DE441 behind it answers outside. A chart
  carried across 1550 sees the Moon step by ≤ 0.36″ (0.06″ at 2650) and
  the planets by < 0.02 mas. Provenance names the file that answered.
- **What else changes out there**: the precession model, now Vondrák 2011
  by default everywhere (FRAMES.md), and the planets' mean orbit points, which are fitted to DE440
  over 1550–2650 and are refused outside it rather than extrapolated
  (ENGINE.md).

## Byte map

Both files — the 1981 DE200 and the 2021 DE440 — use one self-describing
layout. Records are `NCOEFF` f64 words; the first two records are
headers, the rest data. Header record 1 (offsets verified on both files):

| offset | type | field |
|-------:|------|-------|
| 0 | 3 × 84 chars | title lines |
| 252 | 400 × 6 chars | constant names 1–400 |
| 2652 | 3 × f64 | SS: start JED, end JED, interval (days) |
| 2676 | i32 | NCON, number of constants |
| 2680 | f64 | AU (km) |
| 2688 | f64 | EMRAT, Earth/Moon mass ratio |
| 2696 | 12 × 3 × i32 | pointer table columns 1–12 |
| 2840 | i32 | NUMDE, the DE number |
| 2844 | 3 × i32 | pointer table column 13 |
| 2856 | (NCON−400) × 6 chars | constant names 401..NCON (when NCON > 400) |
| then | 3 × i32, 3 × i32 | pointer table columns 14, 15 |

Header record 2 holds the NCON constant values, DENUM first. A pointer
triple is (offset, ncoeff, nsubint): the 1-based word offset of the
column's first coefficient counting the record's two epoch doubles, the
coefficients per component per subinterval, and subintervals per record.
A column is absent when ncoeff is 0 (JPL writes the next free offset with
zero counts).

`NCOEFF` is not stored in the binary; the reader derives it as the last
word any column occupies, which reproduces the published headers exactly
(DE200: 826, DE440: 1018). The file must then hold `(records + 2) ×
NCOEFF × 8` bytes, and the first data record must start at SS.

| column | content | components | units |
|-------:|---------|-----------:|-------|
| 1–9 | Mercury … Pluto (barycentric; 3 = Earth-Moon barycentre) | 3 | km, km/day |
| 10 | Moon, geocentric | 3 | km, km/day |
| 11 | Sun, barycentric | 3 | km, km/day |
| 12 | nutations Δψ, Δε (IAU 1980) | 2 | rad, rad/day |
| 13 | lunar mantle librations (Euler angles) | 3 | rad, rad/day |
| 14 | lunar mantle angular velocity | 3 | rad/day |
| 15 | TT−TDB at the geocentre | 1 | s |

Pointer tables as read from the binaries, matching the published ASCII
headers:

| column | DE200 | DE440 |
|--------|-------|-------|
| Mercury | 3/12/4 | 3/14/4 |
| Venus | 147/12/1 | 171/10/2 |
| EMB | 183/15/2 | 231/13/2 |
| Mars | 273/10/1 | 309/11/1 |
| Jupiter | 303/9/1 | 342/8/1 |
| Saturn | 330/8/1 | 366/7/1 |
| Uranus | 354/8/1 | 387/6/1 |
| Neptune | 378/6/1 | 405/6/1 |
| Pluto | 396/6/1 | 423/6/1 |
| Moon | 414/12/8 | 441/13/8 |
| Sun | 702/15/1 | 753/11/2 |
| Nutations | 747/10/4 | 819/10/4 |
| Librations | — | 899/10/4 |
| Mantle ω, TT−TDB | — | — |

Either byte order is accepted: the reader probes little-endian first,
then byte-swapped, and accepts the order under which the header is
plausible, DENUM matches NUMDE, and the first data record starts at SS.

### Correction to the first reader increment

The first increment (7ddc606) treated `lnxm1600p2170.200` as an
"old-format" file without an embedded pointer table, supplied the table
from `header.200`, and labelled column 12 a libration block absent from
the file. All three were wrong: the binary embeds the full table at the
offsets above, column 12 holds DE200's nutations (the block ends exactly
on word 826), and DE200's planets are barycentric (heliocentric
positions differ from DE440 by ~1.1 × 10⁶ km; barycentric ones agree to
~860 km). The separate old-format code path was removed.

## Evaluation

For a target at time *t*: record `j = floor((t − SS) / interval)`;
subinterval `s` of width `interval / nsubint`; normalised argument
`τ = 2(t − t_sub_start)/width − 1`. Within a subinterval the layout is
component-major. Value `Σ c_k T_k(τ)`, rate `Σ c_k k U_{k−1}(τ) · 2/width`.

`DeFile::state(Body, jed, out)` returns a raw column (unused components
zero). `DeFile::relative_state(Target, Target, jed, out)` composes bodies
in JPL's customary target numbering (1–11 as above with 3 = Earth, 12 =
solar-system barycentre, 13 = Earth-Moon barycentre):
`Earth = EMB − Moon/(1 + EMRAT)`, `Moon = Earth + geocentric Moon`, and
Earth/Moon/EMB pairs directly from the geocentric Moon column so no
precision is lost through barycentric magnitudes.

A `DeFile` caches its last record and is not safe for concurrent use.

## Validation

`tests/test_de.cpp`:

- **Synthetic** (runs everywhere, no data files needed): a DE-layout file with 450
  constants, eight-subinterval Moon, nutations, librations, TT−TDB and an
  absent column, in both byte orders; every column checked at
  record/subinterval boundaries and the end epoch (exact, |Δ| = 0);
  composition formulas; error paths (missing file, garbage, DENUM
  mismatch, truncation mid-record and by a whole record, out-of-range
  and absent-column queries).
- **DE440 vs JPL's `testpo.440`**: all 13,201 test points inside the
  file's coverage — worst |Δ| 1.4 × 10⁻¹⁴ AU (AU/day) for bodies,
  4 × 10⁻²⁰ rad for nutations, 1.5 × 10⁻¹¹ rad for librations (angles of
  thousands of radians, so a larger absolute print-noise floor).
- **DE200 and DE440**: headers and pointer tables pinned against the
  published ASCII headers; polynomial joins at subinterval and record
  boundaries ~1–2 mm (each side carried to the boundary with its own
  velocity from the exactly-rounded probe epochs); nutation columns
  against our IAU 2000A series (≤ 0.009″, the IAU 1980 vs 2000A model
  difference); Moon geocentric distance 402,448.6 km at J2000 (SWE's
  DE441-derived value: 402,448.9 km); heliocentric planet distances;
  obliquity from Earth's orbital pole 23.439°.
- **DE200 vs DE440**: geocentric directions at 1900, 2000, 2026, 2100
  agree to ≤ 2.14″ for the Sun, Moon and planets (Neptune worst, Moon
  1.49″); Pluto 18″ — the 1981 fit predates much of its astrometry.

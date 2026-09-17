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

The DE440 files live in the repo's gitignored `ephe/` directory (see
`ephe/SHA256SUMS` locally); real-file tests print SKIP when a file is
absent, so a machine without them runs the synthetic part only.

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

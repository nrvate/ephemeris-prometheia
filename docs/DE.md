# JPL DE binary reader

`prometheia::de` (in `include/prometheia/de.hpp`, `src/de.cpp`) reads
JPL planetary ephemeris binaries ("DExxx" files). Reading a DE file
means evaluating *their* Chebyshev representation — nothing here
contradicts our own storage policy, which keeps Chebyshev out of the
formats we define (EPM1 stores initial conditions).

## What is supported

The **old-generation binary layout** (DE200 era), as republished by JPL
for modern machines — verified end-to-end against
`lnxm1600p2170.200` (the 43 MB file JPL's Swiss-Ephemeris download page
links to; tracked at `/shares/swisseph/ephe/de200.eph` on this machine,
set `PROMETHEIA_DE200` to point the tests elsewhere):

- little-endian f64 throughout, records of `NCOEFF` = 826 doubles
  (6608 bytes), `KSIZE` = 1652 4-byte words;
- two header records: title (3 × 84 bytes), 400 six-character constant
  name slots (junk-padded past the real `NVALS` = 200 names — the
  blank-scan trap), then the epoch triple SS/FF/NN; the second header
  record holds the constant values, DENUM first;
- data records `[start JED, end JED, coefficients...]` of 32 days;
- DE200 bodies: heliocentric planets, geocentric Moon, barycentric Sun;
  **no lunar libration block** (the pointer table in the published ASCII
  `header.200` lists one, but it would overflow the 826-double record;
  the republished binary simply does not carry it).

The modern header layout (DE405 and later, which embeds the pointer
table in header record 2) is **not implemented yet**; `DeFile::open`
reports a clear error for such files. Adding it is queued work, to be
validated against a real DE441/de440s binary.

## Where the pointer table comes from

Old-format binaries do not embed the GROUP 1050 Chebyshev pointer table,
so it is supplied per-DENUM from the ephemeris' published ASCII header —
public-domain JPL data (cleanroom-safe; it is data, not code):

- `https://ssd.jpl.nasa.gov/ftp/eph/planets/ascii/de200/header.200`

DE200 rows (offset is 1-based counting the two epoch doubles; bodies
listed in GROUP 1050 order):

| body                 | offset | ncoeff | nsubint |
|----------------------|--------|--------|---------|
| Mercury              |      3 |     12 |       4 |
| Venus                |    147 |     12 |       1 |
| Earth-Moon barycentre|    183 |     15 |       2 |
| Mars                 |    273 |     10 |       1 |
| Jupiter              |    303 |      9 |       1 |
| Saturn               |    330 |      8 |       1 |
| Uranus               |    354 |      8 |       1 |
| Neptune              |    378 |      6 |       1 |
| Pluto                |    396 |      6 |       1 |
| Moon (geocentric)    |    414 |     12 |       8 |
| Sun                  |    702 |     15 |       1 |

The table is gapless — each offset is exactly the previous offset plus
`ncoeff × nsubint × 3` — which doubles as a self-consistency check.

## Geometry and evaluation

For a target at time *t*: record `j = floor((t - SS) / NN)`;
subinterval `s` of width `NN / nsubint` days inside the record; normalised
argument `τ = 2(t - t_sub_start)/width - 1`. Within a subinterval the
layout is component-major (`x`, `y`, `z` blocks of `ncoeff`
coefficients). Position is `Σ c_k T_k(τ)`, velocity is
`Σ c_k k U_{k-1}(τ) · 2/width` — i.e. **km and km/day** for this file
(old DE200 units differ from the modern AU / AU-day convention; callers
convert with the file's own `AU` constant).

The reader decodes records lazily and caches the most recent one; the
record length is not self-described in old-format files, so `open()`
matches the file against the built-in per-DENUM specs (geometry sanity
plus DENUM identity) before trusting it.

## Validation

`tests/test_de.cpp` has two parts:

- **Synthetic** (runs everywhere, CI included): a DE200-layout file is
  generated with a known degree-3 Chebyshev field; every body is checked
  at record/subinterval boundaries and the end epoch to 1e-9, plus
  error paths (missing file, unsupported DENUM, truncation, out-of-range
  queries, absent bodies).
- **Real file** (skipped when absent, enabled via `PROMETHEIA_DE200`):
  header and constants pinned against the known DE200 values; Earth
  reconstructed as `EMB - Moon/(1+EMRAT)` with Earth-Sun 0.98333 AU at
  J2000; Moon geocentric distance 402,448.6 km at J2000 — SWE
  (DE441-derived data) inverts to 402,448.9 km at the same epoch, so the
  two ephemerides agree to ~0.3 km; polynomial-segment joins below
  1.5 km across all bodies (measured worst: Mercury 0.80 km); analytic
  velocities against finite differences to 1e-5; obliquity from Earth's
  orbital pole 23.439°; heliocentric planet distances in their physical
  ranges.
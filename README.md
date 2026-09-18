# Ephemeris Prometheia

*Prometheia* (προμήθεια) — Greek for **forethought**, the quality Prometheus
personifies.

A cleanroom solar-system ephemeris built on an inverted paradigm: instead of
precomputing and storing positions (the Swiss-Ephemeris bake-then-read
model, where file size grows with objects × time-span until 800,000+
asteroids become tens of gigabytes across thousands of files), Prometheia
stores **initial conditions** and integrates on demand. File size scales
with the number of objects, period — the whole 1.56-million-body catalog is
a ~40 MB file with no time axis at all.

**License: GPL-2.0-or-later** (matching Astrolog, this project's host
application). The ephemeris *data* it ingests is US-government public domain
(JPL SBDB / Horizons / DE files).

## Status

All six milestones complete, plus protocol version 4 (2026-09-17):

- **EPM1 catalog container** — indexed, zstd-chunked, CRC-checked, no time
  axis; ~25 bytes/record synthetic, 87 B/body for the real 1.57M-body
  catalog (136.7 MB vs the 48 GB of equivalent `.se1` files). Spec:
  [docs/FORMAT.md](docs/FORMAT.md).
- **Ingest pipeline** — resumable, strictly-sequential, rate-limited bulk
  pull from the JPL SBDB Query API → EPM1, plus freshness machinery: slim
  identity sweeps, `orbit_id` delta overlays, hot-subset sweeps. Server
  etiquette is policy: [docs/DESIGN.md](docs/DESIGN.md), details in
  [docs/INGESTION.md](docs/INGESTION.md).
- **Mechanics core** — Kepler engine (elliptic + hyperbolic, closed-form
  propagation as the test oracle), adaptive DP5(4) and an IAS15-class
  Radau-15 collocation integrator (constants derived from the Jacobi
  recurrence, verified against the published paper), cubic-Hermite
  perturber trajectory tables, heliocentric N-body force model, windowed
  memo cache.
- **Planetary ephemeris readers** — cleanroom readers for JPL DExxx
  binaries (self-describing layout, DE200 through DE440+, nutation,
  libration and TT−TDB columns, either byte order) and NAIF SPK kernels
  (`.bsp`, type 2/3 segments with segment chaining). DE440 reproduces all
  13,201 of JPL's official `testpo.440` points to 1.4e-14 AU, and
  `de440s.bsp` agrees with the DE440 binary to 4 cm.
  Details: [docs/DE.md](docs/DE.md), [docs/SPK.md](docs/SPK.md).
- **Time scales** — exact proleptic-Gregorian calendar ↔ JD; UTC ↔ TAI ↔
  TT with the USNO leap-second table and correct `23:59:60` labeling in
  both directions; TT ↔ TDB (published truncated series, ~10 µs class);
  Delta T (TT−UT1) pluggable, defaulting to observed USNO values (1657 to
  the latest month, refreshed each release), the Stephenson–Morrison–Hohenkerk
  eclipse reconstruction before 1657 and a tidal trend after. Sources and
  accuracy notes: [docs/TIME.md](docs/TIME.md).
- **Frames** — IAU 2006 precession + the complete IAU 2000A nutation
  series (1365 terms, parsed from the public-domain USNO Circular 179),
  ICRF → equatorial/ecliptic-of-date matrices, ERA/GMST/GAST, WGS84
  topocentric helper. Differentially validated against the installed
  Swiss Ephemeris to 0.0005". Sources and conventions:
  [docs/FRAMES.md](docs/FRAMES.md).
- **Engine API** — `prometheia::Engine::calc(body, time, options)` over a
  DE binary or SPK kernel: geocentric, topocentric, heliocentric or
  barycentric; light time, solar light deflection and relativistic
  aberration; ICRF, J2000, mean or true equinox of date, ecliptic or
  equatorial; rates and provenance. Agrees with the Swiss Ephemeris run
  on the same DE440 file to 0.0001″ in the J2000 frame (Moon 0.001″;
  0.003″ in date frames, their precession model). ~6 µs per position. Details and the
  understood differences: [docs/ENGINE.md](docs/ENGINE.md).
- **Small bodies and sidereal output (M4)** — `add_catalog()` stacks
  EPM1 containers; catalog bodies answer `calc()` through on-demand
  integration (barycentric point-mass field sampled from the engine's
  own ephemeris, windowed memo per body, newest catalog wins), carry
  `sigma_arcsec` propagated from the SBDB element sigmas, and resolve
  by designation or proper name through `Engine::lookup`. Sidereal
  zodiacs (Fagan/Bradley, Lahiri, user-anchored ayanamshas) shift
  ecliptic longitudes by the anchor + IAU 2006 precession (+ nutation).
  Against swetest on the same DE440: Ceres 0.19″ (the source-elements
  difference; our integration 0.001″), ayanamshas 0.0026″ over
  1800–2200, sidereal positions 0.0034″. Details:
  [docs/ENGINE.md](docs/ENGINE.md), [docs/FORMAT.md](docs/FORMAT.md).
- **C interface** — `prometheia/prometheia.h`: opaque engine handle,
  status codes with an optional caller-owned error struct, validated
  option selectors, sigma/ayanamsha presence flags, a function-pointer
  ΔT hook and the time-scale helpers; no exception or global state
  crosses it. Bit-identical to the C++ engine, tested from strict C99.
  Details: [docs/C_API.md](docs/C_API.md).
- **`ephem` CLI** — positions from the command line for planets and
  catalog bodies (by name, designation or SPK-ID): UTC with leap
  seconds, TT or UT1 input, series, topocentric/helio/barycentric,
  every frame, sidereal zodiacs; table, CSV or JSON output. Written in
  C99 over the C interface. Details: [docs/EPHEM.md](docs/EPHEM.md).
- **Whole catalog** (`prometheia-catalog-bench`, 1,566,773 SBDB bodies,
  each queried one year past its element epoch, 10 threads on a shared
  12-core machine): 285 s wall, 1.58 ms per body per core, 2.5 GB peak;
  with JPL's 16 asteroid perturbers 373 s, 2.22 ms, 2.8 GB. No failures.
- **Measured on real data** (`prometheia-bench`, 100-body SBDB fixture,
  Jupiter + Saturn perturbers): cold 79 bodies ±1 yr = **10.6 ms**;
  warm memoized evaluation = **18 ns**; 100 bodies × 10 yr = **69 ms**;
  Radau-15 55-yr arc = 671 steps at 3.7e-10 AU.
- **Fixed stars** — the naked-eye sky (9,096 Bright Star Catalogue stars
  with Hipparcos astrometry, 84 more IAU-named stars) and the 110 Messier
  objects, found by IAU or traditional name, Bayer, Flamsteed, HR, HD, HIP or
  Messier designation. Apparent places with space motion, deflection and
  aberration, validated against ERFA to 0.34 mas.
  Constellation of any position. [docs/STARS.md](docs/STARS.md).
- **Nodes and apsides** — osculating and mean ascending/descending nodes,
  perihelia and aphelia (perigee/apogee for the Moon) as points seen from any
  observer; mean planetary elements fitted to DE440 itself. Planet-centred
  observers. [docs/ENGINE.md](docs/ENGINE.md).
- **`prometheiad` and protocol version 4** — the WebSocket daemon speaks
  the ephemeris protocol co-designed (and locked from both sides) with the
  Astrolog project: NAIF body IDs, profiles, instant lists, batched LOOKUP,
  per-object META with a truthful `corrApplied`, and SEGDATA — Chebyshev
  segments fitted on a server-owned 32-day lattice with the ayanamsa as its
  own scalar series — plus block-wise compute with CANCEL and priority,
  cell caches shared across requests, and a reference wire client.
  [docs/SERVER.md](docs/SERVER.md), [docs/SEGMENTS.md](docs/SEGMENTS.md).
- **Tools** — `ephem`, `prometheia-fetch` (Python), `prometheia-convert`, `prometheia-spk-trim`,
  `prometheia-info`, `prometheia-bench`; `prometheiad` and `prometheia-wire-client`
  ([docs/SERVER.md](docs/SERVER.md)). Twenty-one test suites (a ~20 s local gate, `tools/gate.sh`), clean under
  ASan/UBSan/LeakSan.

Design rationale, evidence from the Swiss Ephemeris source, and the full
decision record: [docs/DESIGN.md](docs/DESIGN.md). Roadmap: M0–M3 done
(DE + SPK readers, time scales, frames); M4 done (engine,
catalog overlay with sigma and name lookup, sidereal ayanamshas, C
ABI, `ephem` CLI); M5 in progress (JPL Horizons corpus: planets to
6 µas, small bodies 0.04″ rms within 10 years with the Sun's relativistic
term, 0.005″ with JPL's asteroid perturbers, uncertainties matching JPL's
from on-demand covariances, see [docs/VALIDATION.md](docs/VALIDATION.md);
full-catalog benchmark; optional Vondrák 2011 long-term precession);
M6 in progress
(`prometheiad`, serving Astrolog's binary ephemeris protocol over WebSocket,
[docs/SERVER.md](docs/SERVER.md)).

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest
```

Requires C++20, CMake ≥ 3.20, `libzstd` and `pkg-config`; OpenSSL, if found, enables wss:// in `prometheiad`. Sanitizer build:
`-DPROMETHEIA_SANITIZE=ON`.

Install (library, C and C++ headers, `ephem`, `prometheia-info`,
`prometheia-convert`, and a relocatable pkg-config file):

```sh
cmake --install build --prefix /usr/local
cc -std=c99 app.c $(pkg-config --static --cflags --libs prometheia)
```

There is no hosted CI. Before committing, run the local gate —
`tools/gate.sh` (clang-format 14 check, Release and ASan+UBSan builds, all
tests; `CLANG_FORMAT=/path/to/clang-format` to pick the binary).

Tests use [doctest](https://github.com/doctest/doctest) 2.5.3, vendored
under `third_party/` (MIT, test code only — the library itself depends
only on zstd). Each suite is a doctest binary: `./build/test_engine
-tc='engine_synthetic*'` runs matching cases, `-s` shows passing
assertions.

## First catalog

```sh
python3 tools/fetch/sbdb_fetch.py --out-dir sbdb-raw-100 --kinds a \
        --page-size 100 --max-pages 1        # one small, polite request
./build/prometheia-convert sbdb-raw-100 -o ceres-100.epm
./build/prometheia-info ceres-100.epm --sample 4
```

Data files (`*.epm`, `sbdb-raw*/`) are gitignored; only code ships here.
Full-catalog pulls are deliberate, announced runs — never a build side
effect.

## Cleanroom

No Swiss Ephemeris code, headers, or tables were read or used; SWE is cited
in the docs as observed behavior only, and M5 will verify against SWE
output-only. All astronomy comes from published, citable sources (JPL DE
documentation, IAU standards, published integration algorithms).

Not affiliated with, endorsed by, or connected to NASA/JPL/Caltech, or to
Astrodienst AG.

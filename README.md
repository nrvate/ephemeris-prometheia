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

Milestones 0–2 of 6 complete (2026-09-16):

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
- **DE binary reader** — cleanroom reader for JPL DExxx planetary
  ephemeris binaries (old DE200-generation format; modern DE405+ layout
  queued). Validated against a synthetic DE200-layout file in CI and
  against the real 43 MB `lnxm1600p2170.200`: its Moon distance at J2000
  agrees with SWE's DE441-derived value to ~0.3 km.
  Details: [docs/DE.md](docs/DE.md).
- **Measured on real data** (`prometheia-bench`, 100-body SBDB fixture,
  Jupiter + Saturn perturbers): cold 79 bodies ±1 yr = **10.6 ms**;
  warm memoized evaluation = **18 ns**; 100 bodies × 10 yr = **69 ms**;
  Radau-15 55-yr arc = 671 steps at 3.7e-10 AU.
- **Tools** — `prometheia-fetch` (Python), `prometheia-convert`,
  `prometheia-info`, `prometheia-bench`. Seven test suites, clean under
  ASan/UBSan/LeakSan.

Design rationale, evidence from the Swiss Ephemeris source, and the full
decision record: [docs/DESIGN.md](docs/DESIGN.md). Roadmap: DE binary
reader + time/frames (M3), engine API + ayanamsas (M4), validation gates
(M5), transports (M6).

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest
```

Requires C++20, CMake ≥ 3.20, `libzstd` and `pkg-config`. Sanitizer build:
`-DPROMETHEIA_SANITIZE=ON`.

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

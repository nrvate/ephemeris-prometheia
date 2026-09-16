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

Milestone 1 (of 6) complete:

- **EPM1 catalog container** — indexed, zstd-chunked, CRC-checked, no time
  axis; ~25 bytes/record at scale. Spec: [docs/FORMAT.md](docs/FORMAT.md).
- **Ingest pipeline** — resumable, strictly-sequential, rate-limited bulk
  pull from the JPL SBDB Query API → EPM1. Server etiquette is policy:
  [docs/DESIGN.md](docs/DESIGN.md).
- **Tools** — `prometheia-fetch` (Python), `prometheia-convert`,
  `prometheia-info`.
- Verified end-to-end on real data: first 100 numbered asteroids from SBDB
  (Ceres…), full precision, element sigmas, H/G, diameters.

Design rationale, evidence from the Swiss Ephemeris source, and the full
decision record: [docs/DESIGN.md](docs/DESIGN.md). Container byte spec:
[docs/FORMAT.md](docs/FORMAT.md). Data pipeline in full detail:
[docs/INGESTION.md](docs/INGESTION.md). Roadmap: integrator core
(M2), DE binary reader + time/frames (M3), engine API + ayanamsas (M4),
validation gates (M5), transports (M6).

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

# Ephemeris Prometheia — data ingestion

**Status:** the M1 pipeline described here is implemented and verified on
real data (first 100 numbered asteroids). Normative for everything that
produces an `.epm` file. Companion documents: the container byte spec is
[FORMAT.md](FORMAT.md); policy background is [DESIGN.md](DESIGN.md).

## Principles

1. **Open sources, no strings attached** ([DESIGN.md](DESIGN.md)
   decision 5). The small-body catalog pipeline ingests from JPL/Caltech
   public services: SBDB (orbit solutions), Horizons (validation
   sampling), DE binaries (planetary truth, M3). These are US-government
   work, so the *data* in a catalog is public domain regardless of the
   code's GPL license. MPC data is excluded (attribution conditions).
2. **One format encoder.** The Python fetcher never writes EPM1; it
   produces a neutral intermediate (`sbdb-raw v1` TSV). Only the C++
   converter encodes the container, so the byte format has exactly one
   implementation and one spec.
3. **Provenance all the way down.** Every stage records what it did, when,
   and from where: per-page records in `manifest.json`, a flat
   `provenance.txt` for the converter, and a CBOR metadata map inside every
   `.epm`. A catalog built two years from now can be audited without
   guessing.
4. **Be a polite guest** — see Etiquette below. Binding policy, not
   aspiration.

## Pipeline

```
JPL SBDB Query API (HTTPS, JSON)
        │  prometheia-fetch (tools/fetch/sbdb_fetch.py)
        │  sequential GETs, 2 s spacing, resumable
        ▼
sbdb-raw v1:  part-<kind>-<limit_from>.tsv  +  manifest.json  +  provenance.txt
        │  prometheia-convert (tools/convert/convert_main.cpp)
        │  parse → convert units → sort by spkid → dedupe → validate
        ▼
catalog.epm   (EPM1 container)
        │  prometheia-info (tools/info/info_main.cpp)
        ▼
inspection: counts, coverage, bytes/record, sample records
```

## Stage 1 — `prometheia-fetch` (sbdb-raw v1)

### Request

```
GET https://ssd-api.jpl.nasa.gov/sbdb_query.api
    ?fields=<20 fields, comma-joined>
    &sb-kind=a            (or c for comets; one kind per query)
    &limit=50000
    &limit-from=<0|50000|100000|...>
    &full-prec=1
```

Headers on the wire:

```
User-Agent: prometheia-fetch/0.1.0 (Ephemeris Prometheia catalog build; strictly sequential, rate-limited bulk pull)
Accept-Encoding: identity
Connection: close
```

- `full-prec=1` matters: without it SBDB trims numerics for display. The
  catalog stores f64 elements, so we require full doubles (e.g. Ceres σₐ =
  1.01×10⁻¹¹ AU survives; a display-trimmed value would be zero).
- `Accept-Encoding: identity` is deliberate: no gzip CPU spent server-side.
- Responses are checked for the embedded-error convention (`"code"` /
  `"message"` in a 200 body) before being trusted.
- Transport failures retry with 5/10/20/40 s backoff, 4 tries, then abort
  the run (the manifest makes the abort resumable).

### Fields requested (20)

| SBDB field | meaning | unit as delivered |
|---|---|---|
| `spkid` | JPL SPK-ID — primary key of everything | integer |
| `pdes` | primary designation ("1", "2003 SB220") | text |
| `name` | proper name ("Ceres"), null for most | text |
| `class` | SBDB orbit class (MBA, NEO, …) | text — informational |
| `epoch` | epoch of the element set | JD, TDB |
| `e` `a` | eccentricity; semimajor axis (negative when hyperbolic) | — ; AU |
| `i` `om` `w` `ma` | inclination; ascending node; arg. of perihelion; mean anomaly | degrees |
| `sigma_e` `sigma_a` `sigma_i` `sigma_om` `sigma_w` `sigma_ma` | 1-sigma of the elements | as the elements (angles in degrees; the converter stores radians) |
| `H` `G` | absolute magnitude; slope parameter | mag ; — |
| `diameter` | effective diameter, null for most | km |

### sbdb-raw v1 (the intermediate)

Per page, one file `part-<kind>-<limit_from:09d>.tsv`:

- First line: `# fields: kind<TAB><field>...` — the kind column is
  prepended by the fetcher (`a`/`c`), so downstream stages need no side
  channel to know what a row is.
- One body per line: `kind`, then the 20 fields, TAB-separated, in field
  order. JSON `null` → empty cell. UTF-8. No quoting needed (SBDB text
  fields contain no tabs or newlines; the converter rejects any it finds).
- `manifest.json`: schema tag, base URL, fields, page size, and one record
  per completed page (`kind`, `limit_from`, `count`,
  `total_reported`, `file`, `fetched_at_utc`). Re-running the fetcher
  skips completed pages — resume is free.
- `provenance.txt`: flat `key=value` (`schema`, `source`, `url`,
  `started_utc`, `finished_utc`, `fields`, `kinds`, `page_size`, `pages`,
  `count`, `tool`) — deliberately trivial so the C++ converter can read it
  without a JSON parser.

### Etiquette (binding; full policy in DESIGN.md)

Strictly sequential — one request in flight, no threads or pools. ≥2 s
between pages (`--delay`), more for big pulls. Identifying User-Agent.
Backoff-only retries. Development pulls stay tiny (`--page-size 100
--max-pages 1`); a full-catalog pull (~34 asteroid pages + 1 comet page ≈
350 MB, 5–10 min) is a deliberate, announced run, never a build side
effect. If JPL publishes guidance that conflicts, their guidance wins.

## Stage 2 — `prometheia-convert`

Reads `provenance.txt`, every `part-*.tsv` shard (sorted, so page order is
deterministic), and emits one EPM1 container.

### Unit conversions (the only place units ever change)

| field | conversion |
|---|---|
| `i`, `om`, `w`, `ma` | degrees → radians (× π/180); stored as f64 |
| `a`, `e`, `epoch` | none (AU, dimensionless, JD TDB) |
| `sigma_i`, `sigma_om`, `sigma_w`, `sigma_ma` | degrees → radians, then narrowed to f32 |
| `H`, `G`, `diameter`, angular-free sigmas (`sigma_a`, `sigma_e`) | narrowed to f32 |

f64 element precision is preserved bit-exactly through parsing
(`strtod`); only sigmas and physicals are f32 in the record, which is
comfortably beyond their observational accuracy.

### Record flags derived from field presence

| condition | EPM1 record flags |
|---|---|
| all six sigmas present and ≥ 0 (missing treated as 0) | `kSigmas` |
| `H` or `G` present | `kHg` |
| `diameter` present and > 0 | `kDiameter` |
| `name` non-empty | `kHasName` (also derived by the writer) |

### Validation and cleanup

- Rows with missing/bad `spkid`, `epoch`, `a` or `e` are counted and
  skipped, never silently dropped (count lands in metadata as
  `rows_skipped`).
- Rows sorted by `spkid`; duplicate spkids keep the first occurrence,
  count in metadata as `spkid_dupes`.
- The EPM1 writer re-validates every record (finiteness, `e ≠ 1`,
  sign(a) consistent with e, ascending order) — the container never
  stores what it would refuse to read.
- Hyperbolic comets (`e > 1`, `a < 0`) are legal and expected; parabolic
  (`e = 1`) is rejected (singular elements).

### Metadata written into the catalog

`format`, `generator`, `frame` (`ICRF-equinox-J2000`), `time_scale`
(`TDB`), `elements` (order and unit note), `source`, `source_url`,
`fetched_started_utc`, `fetched_finished_utc`, `sbdb_count` (what SBDB
reported), `rows_skipped`, `spkid_dupes`. The engine treats metadata as
descriptive; correctness never depends on it.

### Reference build (100-body sample)

```sh
python3 tools/fetch/sbdb_fetch.py --out-dir sbdb-raw-100 --kinds a \
        --page-size 100 --max-pages 1
./build/prometheia-convert sbdb-raw-100 -o ceres-100.epm
./build/prometheia-info ceres-100.epm --sample 4
```

Measured: 100 bodies, 9,919 bytes total (99 B/body is fixed-overhead
dominated at this size; steady-state is ~25 B/record at 50k scale).

## Asteroid perturber kernel

The small-body force model can include JPL's 16 most massive main-belt
asteroids (docs/ENGINE.md, `add_perturbers`). Their trajectories come from
JPL's SB441-N16 kernel, a US-government work:

```sh
# once, 616 MB, sequential single download (announce it; see Etiquette)
curl -f -A 'prometheia-fetch/0.1.0' -o ephe/sb441-n16.bsp \
  https://ssd.jpl.nasa.gov/ftp/eph/small_bodies/asteroids_de441/sb441-n16.bsp
# the DE440 span only: 41.8 MB, bit-identical inside it
./build/prometheia-spk-trim ephe/sb441-n16.bsp ephe/sb441-n16-de440span.bsp \
  --from 2287184.5 --to 2688976.5
```

- The kernel carries no masses; the engine takes them from the DE file's
  `MAnnnn` constants (DE440 carries all 16) or its built-in DE440 table.
  JPL documents the set in IOM 392R-21-005 (Farnocchia 2021).
- Releases ship the trimmed kernel as an asset beside the catalogs, with its
  SHA-256 (the full kernel: 919d612c…fd90; the DE440-span cut:
  a31b839a…2376). It changes only when JPL publishes a new perturber
  set.

## Orbit covariance: on demand

The bulk SBDB query returns per-element sigmas only, and those are
uncorrelated summaries that overstate sky-plane uncertainty 10–1000×
(docs/VALIDATION.md). The full 6×6 covariance comes from the single-object
API, one request per body (`sbdb.api?sstr=<des>&cov=mat&full-prec=1`) —
at SBDB's polite pace far too slow for the whole catalog, so it is fetched
for the bodies someone asks about:

```sh
python3 tools/fetch/sbdb_fetch.py --out-dir cov-raw --covariance 1 2 4 145451
python3 tools/fetch/sbdb_fetch.py --out-dir cov-raw --covariance-file mine.txt
./build/prometheia-convert cov-raw -o covariance-overlay.epm
```

- The shard `covariance.tsv` carries the standard 20 columns plus
  `cov_epoch`, the cometary elements at that epoch (`cov_e`, `cov_q`,
  `cov_tp`, `cov_om`, `cov_w`, `cov_i`; angles in degrees as delivered)
  and the packed upper triangle `cov_00 … cov_55` in {e, q, tp, node,
  peri, i}. Non-gravitational parameters beyond the six (comets) are
  marginalized by dropping their rows and columns.
- The converter stores angles in radians (squared terms scaled
  accordingly) as the EPM1 `kCovariance` block. The overlay stacks on a
  base catalog like any other (newest wins), so those bodies answer
  `sigma_arcsec`; bodies without a covariance answer none.
- Etiquette as for delta refetches: one request at a time, `--delay`
  (2 s) between them, resumable through `manifest-covariance.json`.
- Proper names: `sbdb.api` has no bare name field; the row's `name` is
  derived from `shortname` minus the designation ("1 Ceres" → "Ceres"),
  matching the bulk query (this also fixed delta overlays, which carried
  "1 Ceres").

## Freshness: catalogs are stacked, not rebuilt

Orbit solutions improve constantly, and unnumbered objects change most.
Ingestion is therefore incremental by design: fetch a small, recent catalog
(a day of SBDB updates, or the top-N brightest), and the engine overlays
catalogs by priority — newer wins, older answers remain available beneath.
No rebake of the 1.56M-object base catalog is ever required to correct one
orbit. (Overlay mechanics land with the engine, M4.)

## Freshness: catching orbit updates in hours or days

SBDB is a **living database, not a release train**: JPL's orbit-determination
pipelines fold in new astrometry continuously (observations flow
observatory → MPC daily batches → JPL; solutions refresh within hours for
active objects), and SBDB exposes **no change feed, webhook, or
modified-since filter** (verified against the filter docs 2026-09-16).
Freshness is therefore *our* mechanism, and the tiered machinery below is
**implemented in `prometheia-fetch`** as a first-class intended use case.

**Three observing tiers, each sized to its patience:**

1. **Hot subset (hourly, tiny):** the objects a deployment actually cares
   about, swept by class with full precision — a few hundred KB per run.

   ```sh
   sbdb_fetch.py --out-dir hot-cen --kinds a --sb-class CEN,TJN
   sbdb_fetch.py --out-dir hot-named --kinds a --fields <fields> --sb-class ...
   ```

2. **Delta sweep (daily):** a slim identity sweep of *all* bodies —
   `kind,spkid,orbit_id` only, ~2 MB per 50k page — diffed against the
   previous slim run. `orbit_id` is the orbit-solution identifier; a moved
   value means a new solution for that body, and only those bodies are
   re-fetched, one request each via the single-object endpoint
   (capped by `--max-delta`; beyond the cap the tool demands a full pull).
   Output is an **overlay catalog** in the standard 21-column layout, so
   the unmodified converter turns it into a stackable `.epm`.

   ```sh
   sbdb_fetch.py --slim --out-dir slim-20260916 --page-size 200000
   sbdb_fetch.py --slim --out-dir slim-20260917 --delta-prev slim-20260916 \
                 --delta-out overlay-20260917
   prometheia-convert overlay-20260917 -o sbdb-delta-20260917.epm
   ```

   Delta-run characteristics, measured on the smoke test: 1 slim sweep
   (~32 pages at 200k/page) + 1 request per changed body. A quiet day
   touches dozens of orbits; a busy one, a few thousand.

   Format nuance learned in the smoke test: the query API reports
   `orbit_id` as e.g. `48` / `JPL 74` while the single-object API reports
   `74` — the diff compares sweep-to-sweep only, so the inconsistency
   never crosses a comparison. The single-object `kind` is a subtype code
   (`an`/`au`/`cn`/`cu` = numbered/unnumbered asteroid/comet); the
   converter classifies by leading letter.

3. **Consolidated base (weekly/monthly):** the full 33-page pull as the
   authoritative base catalog; tiers 1–2 produce small overlay catalogs
   that stack on top by priority until the next base.

**Nightly releases — the noted idea (not built).** The pipeline is
headless and resumable, so a scheduled job on our own machine (the project
uses no hosted CI) could run tier 2 nightly and publish the overlay as a
release asset (`sbdb-delta-YYYYMMDD.epm` + sha256), tier 3 weekly/monthly.
Etiquette binds a scheduled job like any other runner:
sequential, delay-bearing, announcing itself. Deliberately parked until
someone actually wants the cadence.

## Distribution: releases when the data changes

SBDB orbit solutions change continuously, so catalogs are **published as
tagged GitHub release assets, not repo-tree files**:

- One asset per catalog build: `sbdb-full-YYYYMMDD.epm` plus a `.sha256`
  sidecar (the swisseph-fork release pattern: SHA256SUMS, nothing binary in
  the tree). Release assets allow 2 GB each; the full catalog is ~40 MB.
- Each release is immutable and pinned — downstreams can reproduce any
  historical answer by pinning the tag. Provenance travels inside the
  container's CBOR metadata (source, pull window, SBDB count).
- The release cadence is "when the data changes" — typically after any
  significant SBDB refresh worth adopting. A release is always produced by
  the committed `sbdb_fetch.py` + `prometheia-convert` of its own tag, so
  the pipeline that made a catalog ships with it.
- The **only** data files in the repo tree are two small fixtures built
  by this pipeline: `tests/data/sample-100.epm` (~10 KB, the first 100
  numbered asteroids), so the tests exercise the reader against real JPL
  full-precision data, not just synthetic records (its Ceres elements are
  pinned in `test_catalog.cpp`); and `tests/data/covariance-7.epm` (~3 KB,
  a covariance overlay for Ceres, Pallas, Vesta, Iris, Hygiea, Cybele and
  145451 Rumina — the Horizons corpus bodies — from `sbdb_fetch.py
  --covariance`). Rebuild them only when refreshing a fixture
  intentionally.
- The container is already chunked-zstd with its own index — **never**
  re-bucket or re-compress catalogs for distribution; that would break
  single-file random access and re-introduce the per-file juggling this
  project exists to eliminate.

## Release ingestion: Earth-orientation tables

Two small tables that the library needs are compiled into the source
rather than downloaded at run time, so they go stale between releases.
Both are refreshed as a step of every release:

- `src/delta_t_table.inc`: observed ΔT (TT − UT1) from USNO,
  `historic_deltat.data` (1657–1972) and `deltat.data` (monthly, 1973
  to the latest published month);
- `src/leap_second_table.inc`: TAI − UTC leap seconds from USNO
  `tai-utc.dat`.

USNO data is US Government work and in the public domain, which meets
the no-strings rule (DESIGN.md decision 5). Each generated file records
the source URLs, their SHA-256 and the retrieval date.

**Before tagging a release:**

```sh
tools/gen/gen_earth_orientation.py --fetch --raw-dir eop-raw   # 3 sequential requests
cmake --build build -j && ctest --test-dir build              # tests pin historical values only
git add src/delta_t_table.inc src/leap_second_table.inc
git commit -m "data: refresh Delta T and leap seconds (USNO, through YYYY-MM)"
```

`--check` (without `--fetch`, against a fresh `eop-raw/`) exits 1 when
the committed tables differ from the sources, so a release checklist can
gate on it. The retrieval date alone never counts as a change.

The fetch follows the same etiquette as SBDB: one request at a time, a
3 s pause, a `prometheia-gen/<version>` User-Agent, and backoff-only
retries. `eop-raw/` is gitignored.

**What a stale table costs:** ΔT beyond the last sample is extrapolated
(TIME.md). Expect tenths of a second after a year and seconds after a
decade. Each second of ΔT is 0.55″ of lunar longitude and 15″ of Earth
rotation for UT inputs. A new leap second missing from the table makes
UTC conversions after its date wrong by 1 s. IERS announces leap seconds
about six months ahead in Bulletin C, so a release in that window should
include it.

## Future ingestion paths (tracked, not yet built)

| milestone | ingestion | notes |
|---|---|---|
| M3 | JPL DE binary files (planetary + Moon truth) | own cleanroom reader of the documented public format; also validates everything else |
| M5 (built) | Horizons observer tables + vectors → `tests/horizons_corpus.inc` | a verification fixture, refreshed deliberately — not part of release ingestion; see VALIDATION.md |
| later | SBDB non-grav params `A1`/`A2`/`A3` | needed for long-arc comet integration |
| later | SBDB `albedo`, `rot_per`, GM, taxonomy | physical-param extension of the record |
| later | delta catalogs (SBDB change feeds are not offered — diff via `full-prec` re-pull of changed designations) | overlay strategy makes this cheap |

# Changelog

Notable changes to Ephemeris Prometheia. Accuracy and performance figures in
this file are measured, not estimated; where a number is an estimate it says
so. Each entry points at the document carrying the method.

**Versioning.** `0.x` releases may change the C ABI. The C interface described
in [docs/C_API.md](docs/C_API.md) is a cross-repo contract — the Astrolog
project's ephemeris plugin builds against it — so any ABI change is
communicated to known consumers *before* it lands, never shipped unannounced.
That policy is what `0.x` means here: the interface is deliberate and
documented, not that it is frozen.

## Unreleased

### Client-server cross-testing

- **`tools/check/crosstest.py`** runs the reference client against two v4
  servers, compares their answers as angular separations, and uses JPL
  Horizons as referee wherever the corpus has the point. It writes a dated
  leg table ([docs/CROSS-TEST.md](docs/CROSS-TEST.md)). The first run
  (`docs/crosstest/2026-09-18.tsv`) found three defects in the Astrolog
  server, each bisected to one correction term. It also found one in its own
  adjudication, fixed before the table was written.
- The cross-test gained a topocentric anchor, a barycentric anchor and
  refusal checks, and sends only masks both servers list. Deliberate
  differences are recorded as expected-difference with their reason.
- `prometheiad` logs one line per connection, HELLO, request, ERROR and
  close, each with a connection id and the request id, at `--log-level
  quiet|info|debug` (default `info`). It never logs instants, sites, names
  or tokens (SERVER.md, "Logging").
- Cross-test legs `points` (orbit points by direction and distance, and
  the node-on-its-frame's-ecliptic rule) and `sidereal` (the three planes
  for two zodiacs).
- All three protocol v4 sidereal planes: the ecliptic of date, the
  ecliptic of the anchor epoch, and the solar system's invariable plane
  (derived from DE440's angular momentum). Available as
  `CalcOptions::sidereal_plane` in the engine (not yet in the C ABI) and
  through `prometheiad`; `prometheia-wire-client --sid-plane`. Segments
  serve the ecliptic of date only.
- `prometheia-wire-client --node` asked for the ascending node whatever
  point was named, and read the point letter as the method. It is now
  `NAIF.P[.M]`: point `a|d|p|A`, method `m`, `o` or 0–4, and a bad point is
  refused.
- Traceability: `prometheia-wire-client` picks a fresh request id per run
  (`# request N`, the daemon's `req=N`), and the cross-test TSV records the
  ids behind every row. The fetch tools log each GET with a UTC timestamp,
  URL and outcome. `ephem` names the ephemeris file and its version in its
  header and JSON.
- Protocol v4 per-kind correction masks (WELCOME tag 0x0014, ephv4
  `eed6429`, set `472b21a`): vendored and verdicted 99/99, with the
  handshake-dependent judgements 7/7. `prometheia-wire-client` prints
  `# corrkind` lines, and the fixture reader gained `--judge` for request
  fixtures whose refusal depends on a WELCOME. `prometheiad`'s WELCOME
  does not change.
- The cross-test judges the Swiss refit by position (km), not a fixed
  angle. It also referees deflection from a planet centre against the
  textbook formula, where Horizons has no observer.
- `frames::ut1_from_sidereal_time` recovers UT1 from a published local
  apparent sidereal time. `prometheia-ut1` exposes it on the command line
  for the harness.
- `prometheia-wire-client` gained `--deltat`, `--hyp`, `--elements`,
  `--center`, `--corrections` and a capabilities line. `tools/check/wirelib.py`
  is the one reader of its output.
- SERVER.md writes down the per-object error contract.

### Hypothetical bodies

- **Bodies from polynomial orbital elements**, `Engine::calc_elements`, the
  protocol's kind 4: pure two-body motion about the Sun or the Earth, with
  elements in any of five equinoxes, and the corrections applied as for a
  body. The mean anomaly and mean motion follow the protocol owner's
  normative rule, which was corrected before release: M's own nonzero
  coefficients decide its meaning, and n comes from the Gaussian constant,
  so an answer does not depend on the loaded ephemeris.
  [docs/HYPOTHETICALS.md](docs/HYPOTHETICALS.md).
- **Named hypothetical bodies**, `Engine::calc_hypothetical`, the protocol's
  kind 3, defined in JSON Lines element files: one strictly checked body per
  line, in Prometheia's own format. A shipped set is compiled in, and
  `add_hypotheticals()` adds an operator's file, whose definitions win.
- Orbit points and bodies from elements share one correction path; the
  orbit-point results are unchanged.
- **`prometheiad` serves both kinds**, as DATA and as fitted segments. The
  new `--hypotheticals FILE` option adds element files. WELCOME advertises
  kind 4 always, kind 3 with its tokens (A.3 0x0011) whenever any are
  defined, and all five element equinoxes (0x0012). The dataset id digests
  the shipped set and every element file. The fitted-cell key carries the
  elements, so two element sets never share a cell; a test that removes
  them from the key goes red.
- **C ABI version 5**, purely additive: `prometheia_calc_elements` and
  `prometheia_calc_hypothetical` (each with a `_ut` form),
  `prometheia_engine_add_hypotheticals`, token enumeration and
  `prometheia_hypothetical_get`. The signatures were reviewed by the Astrolog
  side before landing, per the ABI policy above. The structs are frozen for
  the life of an ABI version; only the TT forms are interoperable (the UT1
  forms bring in the engine's Delta T).
- The body an orbit is about is called its **origin** in Prometheia's own
  names (C, C++ and element files), because `PROMETHEIA_ELEMENTS_CENTRE_*`
  beside `PROMETHEIA_CENTER_*` was a misreading waiting to happen. Only the
  wire keeps the protocol's "centre".
- **`ephem`**: `hyp:TOKEN` and `hyp:all` as bodies, `--hypotheticals FILE`,
  and `$PROMETHEIA_HYPOTHETICALS`.
- The claim that orbit points are geometric, stale since corrections began
  applying to them, turned up a fourth time (SERVER.md's list of v4
  semantics) and a fifth (prometheia.h). Both are corrected, found this time
  by searching for the phrase rather than by a reader.
- **Error text never quotes the request** (protocol §3.8). Adding a message
  that would have echoed an unknown token exposed a whole class: unknown star
  names, designations, NAIF ids and zodiac tokens all came back verbatim in
  per-object or ERROR text. Per-object text is now a fixed sentence per
  error code, classified from the full message, and a test pins it for every
  object kind.
- **Correction masks follow the protocol.** WELCOME now lists every exact
  mask honoured per observer (all eight; at the Sun's centre only those
  without deflection), and anything else is ERROR 11. Before, it listed one
  mask per observer while answering every combination, so a conforming
  client would never have asked for a geometric or astrometric position.
- **`corrapplied.py` no longer passes vacuously.** It asks only the masks a
  server's WELCOME honours, reports what it cannot ask as inapplicable, and
  fails when nothing, or under half, was checked, or when a check never ran.
  Against the Astrolog server it had printed OK after checking zero cases.
- **Fixes from a review of this session's own work:** a client's name can no
  longer steer the per-object error code (NotFound is classified by its code
  before any message text is read); an element file's `"equinox": 0` is
  refused at load; `ephem` keeps a 64-character token whole; and the wire
  client's `--helio` asks for the mask the Sun's centre honours, and its
  `--jd` defaults to J2000.0 as its help text always said, not JD 0.
- The kind-4 rule is released on the protocol side as "the kind-4 elements
  rule" (Astrolog `176e333`).
- **Shipped element set:** the eight Hamburg points, and Le Verrier's
  predicted Neptune from his 1846 paper. The Hamburg elements are those
  Astrolog distributes, a recorded exception ([docs/DESIGN.md](docs/DESIGN.md)),
  and match `swetest` to 0.00076″. Le Verrier's are held to the position he
  printed himself.
- 0.1.0's notes called kinds 3 and 4 a settled end state. That reflected
  one client's needs and has been reversed; see
  [docs/SERVER.md](docs/SERVER.md).

## 0.1.0 — 2026-09-18

First release. A cleanroom successor to the Swiss Ephemeris, written without
reference to its source, under GPL-2.0-or-later. The work it covers landed
over 2026-09-16 and 2026-09-17, in 97 commits.

### Ephemeris and mechanics

- **JPL DE readers** for the binary format, self-describing across DE200
  through DE440+, either byte order, including the nutation, libration and
  TT−TDB columns. **NAIF SPK readers** (`.bsp`, type 2 and 3 segments, with
  segment chaining). [docs/DE.md](docs/DE.md), [docs/SPK.md](docs/SPK.md).
- **Mechanics core**: a Kepler engine (elliptic and hyperbolic, closed-form
  propagation used as the test oracle), adaptive DP5(4), and an IAS15-class
  Radau-15 collocation integrator whose constants are derived from the Jacobi
  recurrence rather than transcribed. Cubic-Hermite perturber trajectory
  tables, heliocentric N-body forces, a windowed memo cache.
- **EPM1 catalog container**: indexed, zstd-chunked, CRC-checked, no time axis.
  87 bytes per body for the real 1.57M-body catalog — 136.7 MB against the
  48 GB the equivalent `.se1` files occupy. [docs/FORMAT.md](docs/FORMAT.md).
- **Ingest pipeline** for the JPL SBDB Query API: resumable, strictly
  sequential, rate-limited, with freshness machinery (identity sweeps,
  `orbit_id` delta overlays, hot-subset sweeps).
  [docs/INGESTION.md](docs/INGESTION.md).

### Time and frames

- Exact proleptic-Gregorian calendar ↔ JD. UTC ↔ TAI ↔ TT with the USNO
  leap-second table, including correct `23:59:60` labelling in both
  directions. TT ↔ TDB via the published truncated series (~10 µs class).
- ΔT (TT − UT1) is pluggable and defaults to observed USNO values (1657 to
  the latest published month), with the Stephenson–Morrison–Hohenkerk eclipse
  reconstruction before 1657 and a tidal trend after. Continuous everywhere.
  **A ΔT model takes no state from the ephemeris** and is a pure function of
  the instant — deliberately, and [docs/TIME.md](docs/TIME.md) records why.
- IAU 2006 precession with the complete IAU 2000A nutation series (1365 terms,
  parsed from the public-domain USNO Circular 179), ICRF → equatorial and
  ecliptic of date, ERA/GMST/GAST, a WGS84 topocentric helper.
  [docs/FRAMES.md](docs/FRAMES.md).

### Engine, stars, orbit points

- `prometheia::Engine::calc(body, time, options)` over DE, SPK and catalog
  bodies, with apparent-place corrections (light time, gravitational
  deflection, annual aberration) applied as the caller asks, for geocentric,
  topocentric, heliocentric, barycentric and planet-centred observers.
- Fixed stars from a naked-eye BSC + Hipparcos set with an alias table, SIMBAD
  radial velocities cross-checked against the BSC.
  [docs/STARS.md](docs/STARS.md).
- **Nodes and apsides**, mean and osculating, as first-class computed points.
  Corrections apply to an orbit point exactly as to a body. The conventions
  and the measured magnitudes — including why a distant orbit's node moves
  21″ from the observer's velocity and ~0″ from light time — are in
  [docs/ORBIT-POINTS.md](docs/ORBIT-POINTS.md).
- Sidereal zodiacs (ayanamshas), on-demand covariances and position
  uncertainties.

### Server

- `prometheiad`, a WebSocket daemon speaking **ephemeris protocol version 4**,
  co-designed with the Astrolog project and locked from both sides: NAIF body
  IDs, profiles, instant lists, batched LOOKUP, per-object metadata with a
  truthful `corrApplied`, and SEGDATA — Chebyshev segments fitted on a
  server-owned 32-day lattice, with the ayanamsa carried as its own scalar
  series. Block-wise compute (~2 ms slices) with CANCEL and priority, cell
  caches shared across requests, TLS, token budgets, rate limits, and
  `/healthz`, `/readyz`, `/metrics` on the same port.
  [docs/SERVER.md](docs/SERVER.md), [docs/SEGMENTS.md](docs/SEGMENTS.md).
- The protocol's conformance fixtures are vendored with checksums and gated
  in-tree: 91/91, set digest pinned.

### Interfaces

- A C ABI (29 entry points, opaque handles) and `ephem`, a C99 CLI over it.
  [docs/C_API.md](docs/C_API.md), [docs/EPHEM.md](docs/EPHEM.md).
- Tools: `prometheia-fetch` (Python), `prometheia-convert`,
  `prometheia-spk-trim`, `prometheia-info`, `prometheia-bench`,
  `prometheia-wire-client`.
- `pkg-config --cflags --libs prometheia` links both a C and a C++ consumer,
  from an install prefix or from the build tree; both are verified.
- Build requirements are C++20, CMake, zstd and a threads implementation.
  OpenSSL is optional and enables TLS in the daemon. `doctest`, `uWebSockets`
  and `uSockets` are vendored.

### Accuracy, measured

Method and full tables in [docs/VALIDATION.md](docs/VALIDATION.md) and
[docs/ENGINE.md](docs/ENGINE.md).

| quantity | measured |
|---|---|
| DE440 against JPL's `testpo.440` (13,201 points) | 1.4e-14 AU |
| `de440s.bsp` against the DE440 binary | 4 cm |
| Sun and planets, astrometric, vs JPL Horizons (geo / topo) | 6 µas / 11 µas |
| Moon, astrometric, geocentric (1900–2100) | ≤ 0.006″ |
| Apparent place of date, published frame offsets removed | ~1 mas |
| Precession/nutation, differential vs `swetest` | 0.0005″ |
| Small bodies, ±10 yr from element epoch, with SB441-N16 | 0.0053″ |
| Light-time range, Sun and planets | 1.1 m |

### Performance, measured

- **1.9 µs** per position without rates, **4.0 µs** with rates, and **2.95 µs**
  for the server's access pattern (every body at one instant before the next).
  These were 53 and 55 µs before the optimisation work in this release.
- A 90-day segment request for heliocentric Jupiter at a 0.1″ target fits in
  4 segments with a worst residual of 0.031″; a repeat is served wholly from
  the cell cache.

### Not included

- **Hypothetical bodies and polynomial elements** (protocol kinds 3 and 4):
  not served, not advertised. This is a settled end state with reasoning
  recorded in [docs/SERVER.md](docs/SERVER.md), not a gap awaiting work.
- **zstd wire payloads** and **`deadlineMs` as a strategy switch**: the field
  is parsed and advisory; no strategy switch is implemented, and the document
  says so.
- **Vondrák 2011 long-term precession**: unstarted by choice. IAU 2006
  degrades far from J2000, which matters only for DE441's ±13k-year span.
- **Fuzzing of the wire parser.** The protocol reader is exercised by valid
  fixtures and five hand-written malformed frames; it has never been fed
  systematically hostile input. Stated here because a network server's parser
  is the part a reader should be told about.
- **Soak, load and concurrency testing.** Single-client, short-session only.

### Data and licensing

GPL-2.0-or-later. Sources are open with no strings attached; attribution-only
is acceptable and no CC BY-SA material is used anywhere. Data that is not
committed is reproducible from a committed `tools/` script with checksums.
Catalogs are published as tagged release assets rather than repo-tree files,
so a downstream can pin a tag and reproduce any historical answer.

Vendored third-party code: `doctest`, `uWebSockets`/`uSockets`, and the
Astrolog project's `ephproto.h` protocol header with its registries, each with
checksums recorded in `third_party/README.md`.

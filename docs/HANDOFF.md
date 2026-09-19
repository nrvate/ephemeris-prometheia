# Handoff — where the work stands (2026-09-18, evening)

A point-in-time snapshot for anyone picking the repository up. Durable
working rules live in [CLAUDE.md](../CLAUDE.md); the decision history in
[DESIGN.md](DESIGN.md); the protocol state in [SERVER.md](SERVER.md); the
cross-test's full story in [CROSS-TEST.md](CROSS-TEST.md); what shipped in
[CHANGELOG.md](../CHANGELOG.md).

## Released

- **v0.4.0**, published 2026-09-18
  (https://github.com/nrvate/ephemeris-prometheia/releases/tag/v0.4.0).
  - The library and tools are 0.4.0 (C ABI 6); `prometheiad` is 0.5.0.
  - New: the zodiacs defined at the instant, binary-star orbits, sidereal
    positions four times faster, fuzzing and load tools.
- **v0.3.0** and **v0.2.0**, the same day.

## Next

1. ~~Binary-star orbits~~: done (`53d5f58`, STARS.md "Binary stars").
2. ~~Release v0.4.0~~: done.
3. **Astrolog's side of 2026-09-18**, settled (their commits, read from
   their log):
   - **§3.5a:** closed at `b5c67d2`. A zodiac with no anchor epoch gets
     plane 2 from the instant and has no plane 1 (`031f3c9`), with its
     anchor at its true position (`789f3c2`).
   - **Their app's own `-Ys` charts** use A.8's plane too, through the
     server's rotation (`e6c10d6`).
   - **The IAU 1958 pole:** they keep their library's pole. The 0.175″ is
     recorded on their side as an ICRS-transfer difference, not a defect
     (`61759f4`). It stays our 15 `galequ-iau1958` findings.
   - **Sheoran:** they treat `true-sheoran` as anchorless, because their
     library does, measured by probe (`f3eaeec`). We keep the published
     epoch-anchored reading and do not implement the mode, so nothing
     diverges.
   - **Still to come from them:** the binary-star offsets. Their elements
     are in at `7c05f7f`, and the prerequisites at `97d7ab6` and `6838e50`.
4. **Done on 2026-09-18, for the record:**
   - both §3.5a parts, and every fix they named, verified (records g–k);
   - the FK5 star check;
   - the zodiacs defined at the instant (engine, C API, server, `ephem`);
   - fuzzing and the load tool, with the codec's float finding fixed and
     re-pinned (`114f2a5`).

5. **The agent interfaces** (maintainer, 2026-09-18: "AI-forward", with MCP
   and streamable HTTP; the fast paths stay C++, C and the binary protocol).
   - `prometheia-json` serves the JSON tools over MCP (stdio and streamable
     HTTP) and plain JSON (docs/JSON_API.md).
   - The vocabulary ("Words") follows the v4 registries, as amended by the
     Astrolog session. Astrolog exposes no chart tools: it is to be a
     display endpoint that draws what an agent got from here.
   - Open: the interpolated ("natural") lunar apogee, A.14 method 2. It is
     named and refused as `unsupported`. Computing it is an engine feature
     not yet begun.

## The cross-test

The runbook is [CROSS-TEST.md](CROSS-TEST.md). The harness,
`tools/check/crosstest.py`, drives `prometheia-wire-client` against both v4
daemons. `tools/check/wirelib.py` is the one reader of the client's output.

- **Launch:**
  - ours: `build/prometheiad --ephemeris ephe/linux_p1550p2650.440 --port
    47190 --threads 1`;
  - theirs: `astrolog-ephd` on 127.0.0.1:47391 (their own harness binds
    47291). The live tree is `/nvmraid/shares/Astrolog`, branch `qt`, since
    ephv4 landed there; the Astrolog session builds and runs it;
  - then `python3 tools/check/crosstest.py --out docs/crosstest/<date>.tsv`,
    and stop both with `pkill -x` (never `pkill -f`).
- **Legs:**
  - `surfaces` (with refusals per observer, kind and mask);
  - `same` / `horizons`, `helio`, `hamburg`, `apparent`;
  - `topo`: ΔT from Horizons' sidereal time, via `build/prometheia-ut1`;
  - `bary`, `deflection` (textbook formula);
  - `points`, `sidereal`, `sidsweep` (every zodiac token on every plane,
    graded on whether a plane moves the answer), `stars`.
- **Latest record:** `docs/crosstest/2026-09-18o.tsv`, after the binary
  orbits moved to the date's tangent plane (`618861f`): verdicts identical to
  record n. The only findings are
  the 15 `galequ-iau1958` rows (their pole transfer, with their
  maintainer). The binary-star rows are expected to differ by our orbit's
  bend, and do (CROSS-TEST.md, "Binary orbits").
- **Anchors:**
  - JPL Horizons (geocentric, heliocentric, topocentric, barycentric Sun);
  - the textbook deflection formula;
  - the Swiss refit's bands are lengths (`REFIT_KM`), with a 2 mas floor.
- **Recorded as expected, each with its reason in the row:**
  - Swiss's heliocentric light time;
  - its topocentric site about the mean pole;
  - the giant planets' mean elements (ours fitted to DE440, Swiss's from
    VSOP87, per its published manual);
  - coverage refusals at the edge of their files.
- **Application level:** the Astrolog Qt suite casts real charts through a
  server and through local Swiss files:
  ```
  ASTROLOG_EPHSRV_URL=localhost:47190 ASTROLOG_QT_TESTS=ephem-server-live \
    env -u DISPLAY QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME= \
    ./astrolog-qt-test -Yi1 ephem            # from /nvmraid/shares/Astrolog
  ```
  It demands bit-identity, so read the sizes, not the verdicts:
  - Earth 0.0004–0.0012″;
  - the eight Hamburg points ≤ 0.0013″, sent by the app as kind 3;
  - the Mars-centred chart within its 1e-5° tolerance;
  - the solar-system-plane chart −31.5″, which is Part A above.

## Logging and traceability (done)

- **`prometheiad`** (SERVER.md, "Logging"):
  - one timestamped line per connection, HELLO, request (objects by kind),
    completion, ERROR and close, with `c=<loop>.<n>` and `req=<id>`;
  - operational lines at every level;
  - never instants, sites, names or tokens.
- **Joining the two ends:** the wire client picks a fresh request id per run
  and prints it. The cross-test TSV records `req_ours` / `req_theirs`.
- **The tools:**
  - the fetch tools log each GET (`tools/fetch/fetchlog.py`);
  - `ephem` names its ephemeris file and version.
- **Open, theirs:** `astrolog-ephd`'s side of the join.

## Cleanroom state

Two exposures today, both recorded in DESIGN.md "Exposures" with the
maintainer's ruling:
- **The kind-4 elements rule:** `src/elements.cpp` is written only by a
  clean session.
- **The invariable plane's zero point,** which this side invited by asking
  how Swiss does it. Part A now adopts Prometheia's own reading, which
  predates the message, so the firebreak has nothing to rewrite.

Standing rule with the Astrolog side: only spec sentences, fixtures, numbers
and Swiss's *published* documentation cross, never source. Ask in spec
terms.

## Hypothetical bodies (done)

[HYPOTHETICALS.md](HYPOTHETICALS.md):
- kind 4 from JSON Lines element files, and kind 3 by token (C ABI 5);
- the eight Hamburg points from `seorbel.txt` (a recorded exception) and Le
  Verrier's Neptune;
- they match `swetest` to 0.00076″, and through `prometheiad` they match
  `astrolog-ephd` to ≤ 0.0013″.

## The Astrolog collaboration

- The ephemeris protocol is theirs. We are the §3 counterpart with a
  vendored, pinned copy (`third_party/ephproto/v4`,
  header from `qt` `114f2a5` (the floats drop), conformance set `cdbd7438…`,
  105/105 on both readers, judgements 7/7).
- Changes arrive as named drops, reviewed from the text first:
  - kind 4;
  - per-kind correction masks;
  - now the §3.5a sentences.
- Their plugin builds against [C_API.md](C_API.md), a cross-repo contract:
  ABI changes are announced and reviewed before they land.

## Parked (maintainer go-ahead required before starting)

Declined: zstd payloads, on measurement (SERVER.md, "Not implemented").

- The `deadlineMs` strategy switch: parsed and advisory today, and
  documented as unimplemented in SERVER.md.
- Nightly or automated catalogue release builds: an idea only.
- The Moon's node seen *from* a planet's centre, where the two servers
  disagree by ~31°: a definition no client asks for yet.

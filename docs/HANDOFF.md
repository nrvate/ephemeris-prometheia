# Handoff — where the work stands (2026-09-18, evening)

A point-in-time snapshot for anyone picking the repository up. Durable
working rules live in [CLAUDE.md](../CLAUDE.md); the decision history in
[DESIGN.md](DESIGN.md); the protocol state in [SERVER.md](SERVER.md); the
cross-test's full story in [CROSS-TEST.md](CROSS-TEST.md); what shipped in
[CHANGELOG.md](../CHANGELOG.md).

## Released

- **v0.2.0**, published 2026-09-18 (tag on `33c697a`,
  https://github.com/nrvate/ephemeris-prometheia/releases/tag/v0.2.0). The
  library and tools are 0.2.0; `prometheiad` is 0.3.0.
- **Since then (Unreleased):** C ABI version 6,
  `prometheia_options.sidereal_plane` (`76f588d`). The Astrolog side
  reviewed it and the maintainer approved it. Their plugin is rebuilding
  against it so it can serve Astrolog's solar-system-plane charts.

## Next

1. **Decide the two-part protocol drop** ("the §3.5a sentences",
   `/nvm/work/ephv4-drop-planes/DROP.md`, ephv4 `9e9e5c4`). The Astrolog
   maintainer approved both parts; this side's maintainer has not yet.
   - **Part A, the invariable plane's zero point:** the zodiac's zero-point
     direction projected onto the plane. This is what Prometheia already
     does, so nothing changes here. The Astrolog side settled it by
     measurement on their own server: their origin moves with the zodiac,
     which a plane cannot. They change.
   - **Part B, what a frame changes about a node:** "A node lies on the
     mean ecliptic of date; the profile's frame gives the coordinates it is
     expressed in." Prometheia changes: today a node asked in the J2000 and
     ICRF frames lies on the J2000 ecliptic (ORBIT-POINTS.md). The change
     is to find it on the ecliptic of date and rotate it. The `points`
     leg's node-frame check then flips to test the new rule.
2. **Star positions against an outside source** (maintainer's next ask).
   Both servers agree on 29 stars to 0.008″, but both use Hipparcos-derived
   catalogues, so a shared error would pass. Compare a handful of stars'
   catalogue positions with a published source, through a committed fetch
   script, politely (CLAUDE.md, "Data").
3. **Re-verify, when the Astrolog build carries them:**
   - their `9bb48a5`: the true descending node at 1800-01-01 now errors
     (coverage) instead of falling back to the ascending node's distance;
   - their `9e9e5c4` `sefstars.txt`: Toliman is α Cen B, and Proxima
     Centauri is α Cen C, not α Cen. Our `stars` leg's α Cen check should
     then agree for Toliman.

## The cross-test

The runbook is [CROSS-TEST.md](CROSS-TEST.md). The harness,
`tools/check/crosstest.py`, drives `prometheia-wire-client` against both v4
daemons. `tools/check/wirelib.py` is the one reader of the client's output.

- **Launch:**
  - ours: `build/prometheiad --ephemeris ephe/linux_p1550p2650.440 --port
    47190 --threads 1`;
  - theirs, from `/nvm/work/ephv4`: `./astrolog-ephd --bind 127.0.0.1
    --port 47391 --threads 1 --ephe "/nvm/work/ephv4/ephem;/nvm/work/ephv4"`
    (47391: their own harness binds 47291);
  - then `python3 tools/check/crosstest.py --out docs/crosstest/<date>.tsv`,
    and stop both with `pkill -x` (never `pkill -f`).
- **Legs:**
  - `surfaces` (with refusals per observer, kind and mask);
  - `same` / `horizons`, `helio`, `hamburg`, `apparent`;
  - `topo`: ΔT from Horizons' sidereal time, via `build/prometheia-ut1`;
  - `bary`, `deflection` (textbook formula);
  - `points`, `sidereal`, `stars`.
- **Latest record:** `docs/crosstest/2026-09-18f.tsv`, 1474 rows. The
  `stars` leg came after it (CROSS-TEST.md, "Fixed stars").
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
    ./astrolog-qt-test -Yi1 ephem            # from /nvm/work/ephv4
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
  vendored, pinned copy (`third_party/ephproto/v4`, ephv4 `eed6429`
  header, conformance set `d904e358…` at `472b21a`, 99/99, judgements 7/7).
- Changes arrive as named drops, reviewed from the text first:
  - kind 4;
  - per-kind correction masks;
  - now the §3.5a sentences.
- Their plugin builds against [C_API.md](C_API.md), a cross-repo contract:
  ABI changes are announced and reviewed before they land.

## Parked (maintainer go-ahead required before starting)

- zstd-compressed wire payloads: the envelope reserves the flag, but it
  needs a named protocol drop first.
- The `deadlineMs` strategy switch: parsed and advisory today, and
  documented as unimplemented in SERVER.md.
- Nightly or automated catalogue release builds: an idea only.
- The Moon's node seen *from* a planet's centre, where the two servers
  disagree by ~31°: a definition no client asks for yet.

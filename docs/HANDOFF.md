# Handoff — where the work stands (2026-09-18, evening)

A point-in-time snapshot for anyone picking the repository up. Durable
working rules live in [CLAUDE.md](../CLAUDE.md); the decision history in
[DESIGN.md](DESIGN.md); the protocol state in [SERVER.md](SERVER.md); the
cross-test's full story in [CROSS-TEST.md](CROSS-TEST.md); what shipped in
[CHANGELOG.md](../CHANGELOG.md).

## Released

- **v0.3.0**, published 2026-09-18
  (https://github.com/nrvate/ephemeris-prometheia/releases/tag/v0.3.0).
  - The library and tools are 0.3.0, with C ABI 6
    (`prometheia_options.sidereal_plane`). The Astrolog plugin consumes it
    (ephv4 `1be4518`).
  - `prometheiad` is 0.4.0: a node asked for in a fixed frame is now the node
    of date (§3.5a Part B).
  - Two new outside checks: the FK5 stars, and the `sidsweep` leg.
- **v0.2.0**, the same day (tag on `33c697a`).

## Next

1. **The two-part protocol drop** ("the §3.5a sentences",
   `/nvm/work/ephv4-drop-planes/DROP.md`, ephv4 `9e9e5c4`). The Astrolog
   maintainer approved both parts. **Part B is approved here and done**:
   nodes lie on the ecliptic of date in every frame. The `points-frame`
   rows then showed `astrolog-ephd`'s J2000-frame node is not the node of
   date rotated either (J2000 latitude 0.9″ at 1900 where ours reaches the
   ~47″ tilt). They confirmed it (their `eacaadb`, registry §4.2);
   their fix is not in yet. **Part A is approved here too** (2026-09-18)
   and relayed; nothing changes here, and the `sidereal` leg's plane-2
   rows can be graded once their fix lands.
   - **Part A, the invariable plane's zero point (approved):** the zodiac's zero-point
     direction projected onto the plane. This is what Prometheia already
     does, so nothing changes here. The Astrolog side settled it by
     measurement on their own server: their origin moves with the zodiac,
     which a plane cannot. They change.
   - **Part B, what a frame changes about a node:** "A node lies on the
     mean ecliptic of date; the profile's frame gives the coordinates it is
     expressed in." Done here: `orbit_ecliptic` in `src/engine.cpp`, with a
     test that the ICRF node rotated by the public frame matrices is the
     date-frame node. The `points` leg's J2000 check now compares the two
     servers' node of date.
2. **Star positions against an outside source: done** (STARS.md, "Checked
   against FK5"). `tools/check/stars_fk5.py` compares a server with the FK5,
   which is ground-based and pre-Hipparcos. Both servers pass:
   - the 24 ordinary stars within 0.56″ over 1900–2100;
   - the four astrometric binaries up to 2.3″ (Sirius), a straight-line-motion
     limit the servers share;
   - the two servers within 0.007″ of each other against FK5.
3. **Re-verified 2026-09-18 (record g):**
   - their `9bb48a5`: the 1800 descending node answers errCode 3;
   - their `9e9e5c4`: Toliman agrees (0.04″).

   - their `b89f510`: the J2000-frame node agrees (record h);
   - their `554288b`: α Cen A agrees to 0.007″ (record h).

   - their `4f9c2a1` + `284b321` (§4.1): both fixed planes agree to the
     floor (record j). The sweep's 288/288 for them was a harness bug: it
     counted refusals (NaN rows) as movement. Corrected in record k: 240
     agree, 48 refused.
   - **Open on their side:** Astrolog's local Swiss path still delegates
     planes 1 and 2, so the app's own charts keep the old plane-2 origin.
     This is a shared-core change, measured by their suite.

4. **Fuzzing (started 2026-09-18).** `tools/fuzz.sh [SECONDS]` (SERVER.md,
   "Fuzzing").
   - The session is clean over 2.36 million inputs.
   - The codec finding (DATA accepts non-finite values, and there is no
     canonical f32 NaN) is with the Astrolog side.
   - Load and soak are done too (`prometheia-load`, SERVER.md "Load and
     soak"): 64 connections for 5 minutes, flat memory, no leak, and the
     limits hold.
   - Next, if wanted: longer fuzz runs, and pointing the load tool at
     `astrolog-ephd`.

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
- **Latest record:** `docs/crosstest/2026-09-18k.tsv`, against Astrolog `qt`
  `114f2a5`: **no findings**. Their sweep is corrected to 240 agree and 48
  refused (CROSS-TEST.md, "The sweep corrected").
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
  vendored, pinned copy (`third_party/ephproto/v4`, ephv4 `eed6429`
  header, conformance set `d904e358…` at `472b21a`, 99/99, judgements 7/7).
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

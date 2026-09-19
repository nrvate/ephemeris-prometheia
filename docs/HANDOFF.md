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

1. **Binary-star orbits** (maintainer's go-ahead, 2026-09-18). Replace the
   straight line from the Hipparcos epoch for the stars it fails worst:
   α Cen A reaches 28.6″ from the FK5 at 1900, Sirius 2.3″, Procyon 1.5″
   (STARS.md, "Checked against FK5").
2. **Release v0.4.0** (approved). The library and tools will be 0.4.0, since
   the C API already says "library 0.4", and `prometheiad` 0.5.0.
3. **Waiting on the Astrolog side**, with every decision already made by
   the maintainer on 2026-09-18:
   - ~~Anchor at its true position~~: landed (their `789f3c2`), record m.
   - **The §3.5a text drop:** a zodiac with no anchor epoch has the
     instant's zero point on plane 2 and no plane 1, with its anchor at its
     true position. The class is "no anchor epoch", not a list.
   - **Their IAU 1958 pole:** they adopt Liu et al.'s ICRS transfer (0.18″
     on `galequ-iau1958`).
   - **Sheoran's zodiac:** epoch-anchored per its published definition.
   - **Their app's local Swiss path:** still carries the old plane-2 origin
     (a shared-core change, measured by their suite).
4. **Done on 2026-09-18, for the record:**
   - both §3.5a parts, and every fix they named, verified (records g–k);
   - the FK5 star check;
   - the zodiacs defined at the instant (engine, C API, server, `ephem`);
   - fuzzing and the load tool, with the codec's float finding fixed and
     re-pinned (`114f2a5`).

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
- **Latest record:** `docs/crosstest/2026-09-18m.tsv`. The only findings are
  the 15 `galequ-iau1958` rows (their pole transfer, before their
  maintainer); the zodiacs defined at the instant otherwise agree (CROSS-TEST.md,
  "The true anchor").
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

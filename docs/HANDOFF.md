# Handoff — where the work stands (2026-09-19, evening)

A point-in-time snapshot for anyone picking the repository up. Durable
working rules live in [CLAUDE.md](../CLAUDE.md); the decision history in
[DESIGN.md](DESIGN.md); the protocol state in [SERVER.md](SERVER.md); the
cross-test's full story in [CROSS-TEST.md](CROSS-TEST.md); what shipped in
[CHANGELOG.md](../CHANGELOG.md).

## Unreleased, on `initial` (2026-09-19)

Everything below is committed and pushed; no tag has been cut. The full
list, with its numbers, is CHANGELOG.md "Unreleased".

- **A planet's own orbit points are answered from its centre** — a defect
  of ours that record s found (ORBIT-POINTS.md, "From any observer").
- **The performance expedition**, `b200486`..`f41333e` (ENGINE.md,
  "Performance", and the new `prometheia-engine-bench`). Same outputs, save
  the natural apsides (≤ 0.03 mas) and nutation's second derivative
  (2e-22). Small bodies 2,712 → 7.1 µs a call; the Moon's natural apogee
  306 → 26 µs; `prometheiad` 31,900 → 49,300 requests a second, p99 4.63 →
  2.05 ms; a three-body, 1,000-instant JSON call 19.2 → 1.8 s. **The
  dataset id's digest changed once** as a result, so a pinned fixture
  elsewhere will need repinning.
- **`prometheia-json` reads a clock time before 1972 as UT1** (maintainer,
  2026-09-19), answering `"ut1"` in place of `"utc"` (JSON_API.md, "Time").
  It had refused every date before 1972, so most birth charts could not be
  asked for by clock time.
- **`prometheia-json` hardened**, by a new fuzzer, `fuzz_json` (SERVER.md,
  "Fuzzing"): a 500,000-level body no longer overflows the stack (64 now),
  a non-scalar `id` is answered null, `height_m` is bounded, and the HTTP
  log writes only names the server knows.
- **α Cen's proper motions** against Akeson et al. 2021's barycentre
  (STARS.md). Closed: the catalogue's B−A ratio is B's HIP2 error, and our
  model never reads B's proper motion. Nothing we answer moves.

## Released

- **v0.6.0**, published 2026-09-18
  (https://github.com/nrvate/ephemeris-prometheia/releases/tag/v0.6.0).
  - The library and tools are 0.6.0 (C ABI 6); `prometheiad` is 0.7.0.
  - New: the Moon's natural apogee and perigee; the `rates` leg and the two
    defects it found (topocentric point jitter, the rate step); range graded
    against Horizons; α Cen's masses from Akeson et al. 2021.
- **v0.5.0**, published 2026-09-18
  (https://github.com/nrvate/ephemeris-prometheia/releases/tag/v0.5.0).
  - The library and tools are 0.5.0 (C ABI 6, unchanged); `prometheiad` is
    0.6.0.
  - New: `prometheia-json` (MCP over stdio and streamable HTTP, and plain
    JSON), with words that follow the v4 registries. Binary-star orbits are
    in the sky's plane at the date, checked in position angle and direction.
- **v0.4.0**, published 2026-09-18
  (https://github.com/nrvate/ephemeris-prometheia/releases/tag/v0.4.0).
  - The library and tools are 0.4.0 (C ABI 6); `prometheiad` is 0.5.0.
  - New: the zodiacs defined at the instant, binary-star orbits, sidereal
    positions four times faster, fuzzing and load tools.
- **v0.3.0** and **v0.2.0**, the same day.

## Next

1. ~~Binary-star orbits~~: done (`53d5f58`, STARS.md "Binary stars").
2. ~~Releases v0.4.0, v0.5.0 and v0.6.0~~: done.
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
   - The natural (interpolated) lunar apogee and perigee are served
     (ORBIT-POINTS.md, "The natural apsides").

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
    graded on whether a plane moves the answer), `stars`;
  - `rates`: each server's rates against a five-point difference of its own
    positions (CROSS-TEST.md, "Rates, a new leg").
- **Latest record:** `docs/crosstest/2026-09-19t.tsv`, record s
  adjudicated (CROSS-TEST.md, "Record s adjudicated"): their Moon points
  from Mars fixed; the points' missing light time graded as declared in
  their corrApplied; the mean-perihelion rates with their maintainer.
- **Record s:** `docs/crosstest/2026-09-18s.tsv`, against Astrolog's
  `429c764` on a spare port (47392). Orbit points are now asked from the
  Sun, the barycentre and Mars's centre too (CROSS-TEST.md, "Orbit points
  from elsewhere"). Results:
  - One defect of ours, fixed: a planet's own points from its centre were
    refused.
  - New findings, theirs, sent: no light time on the Moon's points from
    the Sun or barycentre; Moon points from Mars placed 0.23–2.4 AU from
    the Earth; mean perihelia with the latitude in the latitude-rate
    column.
  - Unchanged: 91 `rates` rows (their library's speeds, now diagnosed on
    their side and recorded as §3.5a non-conformance under `ratesApprox`)
    and the 15 `galequ-iau1958` rows (their pole transfer).
  - Record r had closed the binaries (8 mas, direction included) and the
    natural apsides at passages (ours 0.002″, theirs 44″).
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

## Open items (2026-09-19)

**Ours.**

- **No tag for the work above.** `initial` carries a release's worth of
  changes with no version bump; the maintainer has not asked for one.
- **One gate failure on 2026-09-19 that did not reproduce.** It came right
  after a `pkill` of `prometheiad`; three further gates and thirty runs of
  `test_prometheiad` were clean, and the failing test's name was not kept.
  Recorded here because an unexplained red is worth remembering, not
  because there is a lead.
- **The cross-test runs by hand.** It needs two daemons, so it stays out of
  `tools/gate.sh` (CROSS-TEST.md, "What it leaves behind"), but nothing
  schedules it either, and a record only exists when someone runs one.
- **The rate-bound sweep was a one-off script**, not a committed check. The
  measured worst case (3.4e-6 °/day, 3.4e-10 AU/day) is why we send no
  A.3 `0x0013`; nothing re-measures it after a change.
- **`prometheia-load` has never been pointed at `astrolog-ephd`**
  (CROSS-TEST.md, "What this does not test").
- **Kinds 3 and 4 have no cross-test cell**, because the Astrolog client
  asks no server for them; kind 4 is covered another way.
- **`/llms.txt` in both repositories, or one combined?** (JSON_API.md,
  "Open questions") — a cross-repo decision, not ours alone.

**Theirs — claimed fixed on `qt`, not yet measured here.**

The Astrolog session reported all five on 2026-09-19. Each hash exists on
their branch with the subject given; none has been graded by a run of ours
yet. Record u is what turns this list into measurements — until it exists,
this paragraph is their testimony, not our evidence.

- The **binary-star offsets**, their `13d3e5e`, graded on their side at
  0.40 mas on Sirius and 0.29 mas on α Cen A against our fixtures.
- The **16 `rates` rows** (a mean perihelion's latitude-rate column holding
  the latitude), their `d1f1287`, by five-point difference at h = 1/1024 d.
- The **64 `sidsweep` rows and the plane-2 offset**, their `1ecb8b9`: their
  stars took Swiss's plane-2 origin while their planets took ours.
- Their **advertised rate bound**, now `5e-3` °/day and `1e-4` AU/day in
  place of the 5 °/day that had been raised to cover the row above.
- Their half of the **request-id log join** was never outstanding: it has
  been there since their `943b74e`, and both sides' notes had it wrong.

**Still genuinely unadjudicated.**

- **The 1800 Moon**, one row, whose anchor lies outside their coverage.
- **Six rows at 1650**, where they refuse the Earth, so a Moon point has
  nothing to be held to.

Closed earlier and not to be reopened: deflection from a planet centre
(record d, refereed against USNO Circular 179; their registry §2.3 carries
the 0.544″ and they no longer advertise the term), the Venus 2020 and
Mercury 2100 edge rows (inside the band once it became a length), and
unlisted correction masks (both §3.5a parts approved).

## Parked (maintainer go-ahead required before starting)

Declined: zstd payloads, on measurement (SERVER.md, "Not implemented").

- The `deadlineMs` strategy switch: parsed and advisory today, and
  documented as unimplemented in SERVER.md.
- Nightly or automated catalogue release builds: an idea only.

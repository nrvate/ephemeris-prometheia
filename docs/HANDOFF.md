# Handoff — where the work stands (2026-09-20)

A point-in-time snapshot for anyone picking the repository up. Durable
working rules live in [CLAUDE.md](../CLAUDE.md); the decision history in
[DESIGN.md](DESIGN.md); the protocol state in [SERVER.md](SERVER.md); the
cross-test's full story in [CROSS-TEST.md](CROSS-TEST.md); what shipped in
[CHANGELOG.md](../CHANGELOG.md).

## Shipped in v0.7.0 (2026-09-19)

The full list, with its numbers, is CHANGELOG.md "0.7.0".

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

- **v0.7.0**, published 2026-09-19
  (https://github.com/nrvate/ephemeris-prometheia/releases/tag/v0.7.0).
  - The library and tools are 0.7.0 (C ABI 6, unchanged); `prometheiad` is
    0.8.0.
  - New: the performance expedition (small bodies 2,712 → 7.1 µs a call,
    `prometheiad` 31,900 → 49,300 requests a second); a clock time before
    1972 read as UT1; `prometheia-json` hardened by `fuzz_json`; a planet's
    own orbit points answered from its centre.
  - Moves a pinned value twice: the natural apsides by ≤ 0.03 mas, and the
    dataset id's digest once.
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
2. ~~Releases v0.4.0 through v0.7.0~~: done.
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
   - **The binary-star offsets** landed at their `13d3e5e` (elements
     `7c05f7f`, prerequisites `97d7ab6` and `6838e50`) and are confirmed
     here in record u: `stars` 174 of 174 agree at a 0.02″ band.
4. **Done on 2026-09-18, for the record:**
   - both §3.5a parts, and every fix they named, verified (records g–k);
   - the FK5 star check;
   - the zodiacs defined at the instant (engine, C API, server, `ephem`);
   - fuzzing and the load tool, with the codec's float finding fixed and
     re-pinned (`114f2a5`).

5. ~~**The topocentric deflection leg**~~ — done, `62b8f80`, record
   `docs/crosstest/2026-09-20-deflection-topo.tsv`, written up in
   CROSS-TEST.md "The topocentric deflection leg". 23 rows, all agree.
   `leg_deflection_geo` became `_leg_deflection_at(… bit, where)` and the
   geocentric leg is that function with bit 0 and no site; the topocentric
   one runs from Zurich and Quito with `--topo` on every request in a row.
   - The deflection rows came out exactly as expected — ours 0.000000",
     theirs 0.000011", the geocentric figures. **The part that was worth
     building is the site-reach check.** A row graded against a server's
     own mask-1 answer cannot tell a topocentric answer from a geocentric
     one, so the leg also measures each server's shift from its own
     geocentric answer (11.6" at Zurich, 15.4" at Quito) and between the
     two sites (19.4"), against a 0.5" floor on the *largest* shift.
     Fault-injected both ways: 1% on the GM reds four rows a site and
     leaves the controls green; dropping `--topo` leaves all twenty
     deflection rows green and reds the three site rows.
   - Anything else built on this pattern should copy the second half, not
     the first. The self-referential grading that makes the leg portable
     across two servers is also what makes it blind to an argument that
     never arrives.

6. **The agent interfaces** (maintainer, 2026-09-18: "AI-forward", with MCP
   and streamable HTTP; the fast paths stay C++, C and the binary protocol).
   - `prometheia-json` serves the JSON tools over MCP (stdio and streamable
     HTTP) and plain JSON (docs/JSON_API.md).
   - The vocabulary ("Words") follows the v4 registries, as amended by the
     Astrolog session. Astrolog exposes no chart tools: it is to be a
     display endpoint that draws what an agent got from here.
   - The natural (interpolated) lunar apogee and perigee are served
     (ORBIT-POINTS.md, "The natural apsides").
   - **Driven end to end as an agent would, 2026-09-20** — a birth chart by
     clock time and place, then the names and the words around it, against
     JSON_API.md rather than against its own tests. Six gaps, all of one
     kind: *the server knew something and did not say it.*
     - `54482f2`: a deployment with no catalog was indistinguishable from a
       misspelling (`capabilities.asteroids`, and the per-object error);
       `lookup`'s `prefix` reached only the star index, so `node` never
       found `true node`.
     - The round after it: **a naive clock time was read as UTC** and the
       reply called it `"utc"` — the one wrong number this surface could
       hand back in silence — now refused wherever a time is read;
       **provenance never named the observer**, so a geocentric answer and
       a topocentric one 8.8" apart were byte-identical once they left the
       request behind; **an unknown top-level key was ignored**, so
       `houses` or `aspects` got positions back and no hint. Each of the
       three was fault-injected: removing the behaviour reds its case and
       nothing else.
     - The round after that, same class again, found by asking which
       arguments move a number without appearing in the answer: the
       **sidereal plane** (1.02° of latitude on Mars between `date` and
       `invariable`, and provenance identical across all three planes) and
       the **precession model** (6 mas at 1600), both now in provenance —
       precession only where it entered, since a tropical answer on ICRF
       or J2000 axes is the same number under either model. And the
       unknown-key rule, which `positions` alone had, now covers all four
       tools: a mistyped `prefix` on `lookup` had quietly matched exactly
       and answered nothing, which reads as "no such name".
     - The one behaviour change a lenient client could notice is the
       refusals: a caller that was sending a naive time, or an argument a
       tool does not read, now gets `invalid-arguments`.
     - The rule these all came from, worth keeping: **an argument that
       moved the answer is named in the answer**, and an argument that
       moved nothing is refused rather than dropped. It is the arrival
       problem (item 5) in the surface instead of the harness.

## The cross-test

The runbook is [CROSS-TEST.md](CROSS-TEST.md). The harness,
`tools/check/crosstest.py`, drives `prometheia-wire-client` against both v4
daemons. `tools/check/wirelib.py` is the one reader of the client's output.

- **Launch:**
  - ours: `build/prometheiad --ephemeris ephe/linux_p1550p2650.440 --port
    47190 --threads 1`;
  - theirs: `astrolog-ephd` on 127.0.0.1:47391 (their own harness binds
    47291). The live tree is `/nvmraid/shares/Astrolog`, branch `qt`, since
    ephv4 landed there; the Astrolog session builds and runs it. **47391 is
    their long-running daemon and is never to be restarted from this side;
    47392 is the spare they put up for cross-tests**, and the recent records
    were taken against it;
  - then `python3 tools/check/crosstest.py --out docs/crosstest/<date>.tsv`,
    and stop both with `pkill -x` (never `pkill -f`) — ours only.
  - `tools/check/ratesweep.py --server HOST:PORT --out <record>.tsv` sweeps
    one server on its own, for the rate bound (below).
- **Legs:**
  - `surfaces` (with refusals per observer, kind and mask);
  - `same` / `horizons`, `helio`, `hamburg`, `apparent`;
  - `topo`: ΔT from Horizons' sidereal time, via `build/prometheia-ut1`;
  - `bary`, `deflection` (textbook formula, from Jupiter's centre),
    `deflection-geo` (the same formula from the Earth, at each body's
    searched-for closest approach to the Sun), `deflection-topo` (the same
    from two sites, plus three rows that grade whether the site reached the
    computation at all);
  - `arrival`: whether each observer reached the computation at all, and
    whether observers that should differ do (CROSS-TEST.md, "The arrival
    leg") — the one leg that measures arrival rather than consistency;
  - `points`, `sidereal`, `sidsweep` (every zodiac token on every plane,
    graded on whether a plane moves the answer), `stars`;
  - `rates`: each server's rates against a five-point difference of its own
    positions (CROSS-TEST.md, "Rates, a new leg").
- **Latest record:** `docs/crosstest/2026-09-20u.tsv` (CROSS-TEST.md,
  "Record u"), against their `96aef21` on the spare port. No findings
  against this side. Their binary-star offsets, mean-apsis rates and
  plane-2 star origin all confirmed by measurement, and their advertised
  rate bound holds (worst 8.22e-4 °/day off their wire against 5e-3
  advertised). It also found two defects in this harness, both of which
  misread a correct server.
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

## Open items (2026-09-20)

**Ours.**

- ~~The topocentric deflection leg is not written~~: written and measured,
  `62b8f80` ("Next", item 5; CROSS-TEST.md, "The topocentric deflection
  leg"). All 23 rows agree, and the site demonstrably reaches the
  computation on both servers.
- ~~No leg yet grades an observer the way the topocentric one grades its
  site~~: the `arrival` leg does, `ef8c32c` (CROSS-TEST.md, "The arrival
  leg"). 8 rows, all agree on both servers, mask 0. Seven ask whether two
  observers differ — each observer against geocentric, plus barycentric
  against heliocentric and Jupiter's centre against Mars's — and one asks
  the opposite, since a heliocentric observer and an observer at the Sun's
  centre are the same place (both servers: 0.0000").
- **One gate failure on 2026-09-19 that did not reproduce.** It came right
  after a `pkill` of `prometheiad`; three further gates and thirty runs of
  `test_prometheiad` were clean, and the failing test's name was not kept.
  Recorded here because an unexplained red is worth remembering, not
  because there is a lead.
- ~~The cross-test runs by hand.~~ Closed 2026-09-20 by
  `tools/check/crossrun.py --record` (CROSS-TEST.md, "One command"): one
  command starts our daemon, runs every leg, then `corrapplied.py` and
  `ratesweep.py` against each server, stops only what it started, and diffs
  its table against the previous record so a run that reproduces the 99
  standing findings says it found nothing. It still needs *their* daemon,
  which it never manages, so it stays out of `tools/gate.sh`, and nothing
  schedules it — but the sequence is no longer something to remember.
  - First run: `docs/crosstest/2026-09-20v.tsv`, 3,758 rows, identical to
    record `u` verdict for verdict except the three legs built since (41
    rows, all agree) — the reproduction `u` never had.
  - It also found a defect in our own instrument: `corrapplied.py` read
    A.3 0x0004 alone and accused their server of claiming corrections it
    had declared in 0x0014, the per-kind record. Fixed, both servers pass,
    fault-injected. **Third time in two days that the measurement was
    wrong and the program was fine** — the expected ratio once the cheap
    checks are green, not a run of bad luck.
- ~~The rate-bound sweep was a one-off script~~: it is now
  `tools/check/ratesweep.py`, committed, reading whichever bound is in
  force off the wire and exiting non-zero when a server misses it. Ours
  measures worst 3.6e-6 °/day and, in the corrected absolute unit,
  1.7216e-10 AU/day over every solar-system row, inside A.3's default,
  which is why we still send no `0x0013`. It needs a daemon, so it is not
  in `tools/gate.sh`.
- ~~**`prometheia-load` has never been pointed at `astrolog-ephd`**~~ —
  done 2026-09-20 with their consent, on terms agreed beforehand
  (CROSS-TEST.md, "Pointed at `astrolog-ephd`"): their spare on 47392, 16
  connections, 30 s, their caps untouched. **2,463 canary answers graded,
  none differed** — their answers do not change under contention. Their
  compute budget matched ours to one request in 20,000 (19,999 in the
  first ten seconds, then 1,000 and 999 a second), which is two
  independent readings of A.3 landing on the same arithmetic and the only
  evidence either side has about the limiter. Their RSS rose 29.4 → 89.4
  MB and had not plateaued when the run ended; **that is the run's length,
  not a finding**, and was not reported as one. **Settled the same day** by
  their two-cap measurement — it tracks `--cache-mb` and plateaus — and it
  turned up a real finding against *both* projects: neither asserted
  anything about memory, so a cache that stopped evicting would have passed
  every check either side has. Ours is now
  `prometheia-load --memory-bound MB` (SERVER.md, "The memory bound"), which
  asserts the bound **and** that the cache is really filling, because a
  bound alone passes a server that caches nothing. Sabotage-proven five
  ways. Growth is about 1.4x the cap, flat at 20.8 MB over 818,169 requests
  at `--cache-mb 8`.
  - **Nothing runs it on a schedule**, same as `crossrun.py`: it needs a
    daemon, so it stays out of `tools/gate.sh`, which must stay seconds.
  - **What needed doing first:** the tool graded
    nothing. It counted answers that arrived and never read their numbers,
    so it could not have seen a server answer differently under load than
    it does idle — the same blindness as a leg graded against a server's
    own other answer, in a second place. It now re-asks fixed instants
    under load and grades every value, the shape, and each object's NAIF
    id against the baseline it took before the load (SERVER.md, "Load and
    soak", "The canaries"), fault-injected four ways. Against ours: 52,646
    canary answers graded in a 20 s run, none differed, and the check
    costs nothing measurable (41,770 requests a second with it, 40,015
    without — a canary is a cache hit).
  - It stays a consistency check. The baseline is the server's own idle
    answer, so it says nothing about whether the numbers are right; that is
    every other leg's job.
- **Kinds 3 and 4 have no cross-test cell**, because the Astrolog client
  asks no server for them; kind 4 is covered another way.
- ~~`/llms.txt` in both repositories, or one combined?~~ Settled
  2026-09-20 (JSON_API.md, "`/llms.txt`"): one per surface, and Astrolog
  exposes no agent tools to have one. It obliges them of nothing — if they
  ever serve agent tools they serve their own file beside them — so it did
  not need to go to the maintainer.

**Theirs — all measured and closed in record u (2026-09-20).**

- The **binary-star offsets** (`13d3e5e`): `stars` 174 of 174 agree.
- The **16 mean-apsis `rates` rows** (`d1f1287`): Moon mean node 16/16 and
  Mars mean perihelion 16/16 agree. A regression pin, not an independent
  verdict — they fitted the fix against numbers this side reported.
- The **64 `sidsweep` rows and the plane-2 offset** (`1ecb8b9`): 530 agree,
  no findings.
- The **topocentric lunar points** (`b631333`): Swiss's named points are
  geocentric definitions it does not serve topocentrically and does not say
  so. Found here by `tools/check/ratesweep.py` (8 rows over their bound,
  worst 7.93e-2 °/day); fixed the same day, re-measured at 2.5e-6, and
  their points now sit 0.089″–0.32″ from ours where the apogee had been a
  whole diurnal parallax away.
- Their half of the **request-id log join** was never outstanding; it has
  been there since `943b74e`, and both sides' notes had it wrong.

Still theirs, all long-standing: 84 `rates` rows of body speeds, 15
`sidinstant` rows for the IAU 1958 pole.

**Open with them, awaiting their decision.**

- **Their advertised rate bound.** Still `5e-3` °/day and `1e-4` AU/day,
  which their own fix has made loose. Our sweep's worst against their
  fixed build is **1.3939e-3** °/day (the topocentric Moon at Quito in
  2100), wider than their own 8.216e-4 because of the site and the epoch,
  not the object. We recommended `3e-3` and `3e-5` — two to three times the
  widest measurement either side holds, because this number has twice been
  wrong from being set *at* a measurement. `1e-5` AU/day in particular
  would fail on the day: the measurement is 1.0014e-5.
- ~~**§3.5a's distance tolerance cannot be met for a fixed star**~~:
  answered by them at their `022b0a4`, and not by any shape either side had
  proposed. **§3.5a now defines the comparison on f64 positions** — one
  sentence, every object, no relative tolerance and no exemption for stars.
  Their held-open outlier (Polaris from Quito at 1900) turned out to be an
  **f32 artifact**: at f32 the ulp of 2.7e7 AU is 3.26 AU, the distance
  column cannot move across the window, and the "discrepancy" is the
  server's own rate with the sign off. Reproduced here (CROSS-TEST.md, "The
  distance tolerance…"): our engine freezes the same column, so it is the
  wire type, not an engine. **Our sweep already read f64** — nothing in
  `tools/check/` passes `--f32` — so our 218 star rows stand, and we
  advertise no `0x0013` for the artifact to have described. What remains
  true is the original point: at f64 the floor is still ~9e-6 AU/day for
  Polaris, four orders above A.3's 1e-9 default.
- **Topocentric with the interpolated method** is now refused on their side
  (no `swe_nod_aps` destination exists), while their chart column keeps
  Swiss's wrong answer. Recorded in their registry §2.9; our sweep sees the
  refusal and records it as unanswered.

**Still genuinely unadjudicated.**

- **The 1800 Moon**, one row, whose anchor lies outside their coverage.
  It is the only unadjudicated row in record u; the cluster that record t
  left unattributed is gone, because `corrApplied` now attributes a
  light-time difference without needing the Earth beside it.

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

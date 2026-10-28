# Handoff — where the work stands (2026-10-28)

A point-in-time snapshot for anyone picking the repository up. Durable
working rules live in [CLAUDE.md](../CLAUDE.md); the decision history in
[DESIGN.md](DESIGN.md); the protocol state in [SERVER.md](SERVER.md); the
cross-test's full story in [CROSS-TEST.md](CROSS-TEST.md); what shipped in
[CHANGELOG.md](../CHANGELOG.md).

## Resume here (state at 2026-10-28)

**astromcp's port plan, Part A, has landed through A-14.** The plan is
`docs/prometheia_port_plan.md` on astromcp's `prometheia` branch. Its
coordinator is the astromcp session. Astrolog's session hears about every
change to the C ABI, the wire or `.epm` before it lands.

**Landed on 2026-10-28, in order:**
- A-12: positional reads (`io::RandomAccessFile`) for the DE, SPK and
  catalog readers (`a11531e`).
- A-13: read cursors. The readers keep no mutable state, and each thread
  brings its own `DeCursor`, `SpkCursor` or `ChunkCursor` (`e2b8074`).
- A-8: heliacal visibility after Schaefer (HELIACAL.md; `1508993`,
  `d816db7`, `94819e4`, `2fdab7d`).
  - Schaefer 1993's eq. 15c is misprinted, so the twilight term comes
    from his 1998 revision in S&T.
  - The default detection margin is 0 (maintainer).
  - Each event reports how firm its date is: `spread_before_days` and
    `spread_after_days`, both additive in C.
  - Against swetest every difference has one sign: we see objects
    sooner. Stars are 1–6 days early, Mars −11/+21 (HELIACAL.md).
- A-14: one frozen `Corpus` shared by one engine per thread
  (`Engine::open_context`; ENGINE.md, "Threads"; `17e9770`).
  - A context costs 8 KB, now that the nutation cache is lazy.
  - The single-thread path is +6% (median 3.83 → 4.05 µs).
  - `tools/tsan.sh` runs the thread suite under TSan.
- The macOS fix: the registry no longer uses floating-point `to_chars`
  where the platform lacks it (`7f22dc6`).
- The Windows fix: every open (the readers, the writers, the hypotheticals
  file and the format sniff) takes UTF-8 paths through `io::native_path` and
  `io::open_file` (EMBEDDING.md; `5b21e42`).

**Astrolog has confirmed:**
- A-12 on MSVC:
  - no warnings in `io.cpp` or `registry.cpp`;
  - its suite passes 6477 with 0 failed;
  - all 71 oracle lines are identical to Linux.
- A-13 with A-8, pinned at `2fdab7d`: 1246 with 0 failed, and the charts
  are byte-identical.
- A-14 with the macOS fix, pinned at `7f22dc6`:
  - macOS: Xcode 26.6, deployment target 12.0, no diagnostics in
    `registry.cpp`, and the suite passes 6475 with 0 failed;
  - locally: 1246 with 0 failed, and the charts at 1905, 2026 and 2099
    are byte-identical.

**Next:**
- **A-15 is paused** until the maintainer says go. Its scope:
  - shared perturber and apsis blocks;
  - the C dataset API: `prometheia_dataset_open`, `_add_*`, `_freeze`,
    `_close` and `prometheia_context_open(dataset)`. It is additive, and
    its exact signatures go to Astrolog before it lands;
  - `prometheiad` moves to one dataset with one context per event loop;
  - the stress test also runs through C;
  - the bench must show no loss on the single-thread path.
  - Size: M–L in the plan's own estimate.
- Then A-16 (segments through C, for the scan kernel) and A-17 coverage.

**Open:**
- Heliacal: only sighting records can settle the offset from swetest.
  Schaefer 1987 (JHA, 556 sightings) has not been read (HELIACAL.md).
- `tools/fetch/stars_fetch.py --only simbad-ngc` still waits on SIMBAD.
  The sandbox cannot reach it, and the edit that declares the source stays
  uncommitted until the file is pinned.

**Limits** (maintainer): at most 3 sub-agents and 2 CPU-bound jobs at
once. The machine is shared with the astromcp and Astrolog sessions.

## State at 2026-10-27

**astromcp is porting from Swiss to Prometheia.** Its plan's Part A
(A-0..A-17) is our work. The peers are the astromcp session, which
coordinates, and the Astrolog session, which gets every C ABI and wire
change *before* it lands (maintainer's rule). Astrolog absorbs each landing
by moving its pin; so far all 1246 of its group tests pass, and its charts
are byte-identical.

**Landed on 2026-10-27, in order:**
- A-0: `prometheia.swe` removed (`7cf6e12`).
- A-11: perturber tables on one fixed grid (`41c95e1`).
- A-1: houses, sidereal time, obliquity and rotations in C. R, C and T
  cusps out of zodiacal order are refused (`365fd3c`).
- A-17: the native Python binding (`1cf8bd7`).
- A-2: the horizon and refraction (`19c3fa2`).
- A-3: rise, set and transit (`85a8b97`).
- Chiron and Nessus against Horizons (`56d9dbd`).
- EPM1 1.2: orbit class and old designations (`a5abc8e`, `3f79d03`).
- A-5: the Raman zodiac (`3a29e24`).
- The full SBDB pull, `sbdb-20261027.epm`, released as `catalog-20261027`
  (`3ed2b9c`, `06ade08`).
- The Sun's orbit points (`015edb6`); on v4 under §3.5a as amended
  (Astrolog `4607f86`), with mean models named in source strings
  (`00d6ec6`).
- A-9: the object registry (`5e17363`).
- A-4: phenomena and magnitudes (`006956a`).

- A-6 and A-7: lunar and solar eclipses (ECLIPSES.md, SOLAR_ECLIPSES.md),
  built by agents in worktrees and merged here. Both APIs read `kinds` the
  same way (unsigned, 0 for any) and take TT starts. Their `prometheia_site`
  is one struct. `prometheia_engine_delta_t` gives UT1 for TT results.
- The Moon's magnitude stops at 120° of phase, the limit of the
  observations behind it (PHENOMENA.md).

**Next:**
- A-8, heliacal events after Schaefer: implemented (HELIACAL.md), with the
  paper's equations and swetest's output as the oracles; Ptolemy's arcs and
  Schaefer 1987 are not read.
- Then A-12..A-16, and A-17 coverage.

**Waiting on the maintainer:**
- `tools/fetch/stars_fetch.py --only simbad-ngc`, for NGC 869/884
  (Capulus). The sandbox cannot reach SIMBAD, and the edit declaring the
  source is uncommitted until the file is pinned.

**Limits** (maintainer): at most 3 sub-agents and 2 CPU-bound jobs at
once. The machine is shared with the astromcp and Astrolog sessions.

## State at 2026-10-25

**Astrolog is adopting Prometheia** (their maintainer's direction,
2026-10-25). Prometheia becomes their default ephemeris source, Swiss stays
selectable, and they build against a required Prometheia checkout, likely a
git submodule. Linux comes first, then Windows and macOS. They are removing
`astrolog-ephd`. Their `tools/ephsrv-prometheia.sh` keeps running their v4
client against our `prometheiad`. The paths we read stay where they are:
`ephsrv/ephproto.h`, `conformance/`, `registries.json` and
`houses-rows.tsv`. `crosstest.py`'s default Swiss reference
(`astrolog-ephd`) will stop being rebuilt on their side.

Their session is `astrolog-fc`. They sent asks A–F; status:
- **A, embedding (blocked them): done.** See [EMBEDDING.md](EMBEDDING.md):
  `embed/sources.txt` contract 1, `embed/prometheia.mk`, and
  `tools/check/embedtest.py` in `scheduled.sh`.
- **B, zstd on Windows and macOS:** the maintainer decided, 2026-10-25:
  zstd stays an external library everywhere, taken from the platform's
  package manager (vcpkg, Homebrew), and is not vendored. Contract 1
  stands.
- **C, MSVC and Apple clang:** never built. The audit's blockers are
  removed, and the fallbacks are tested on Linux (EMBEDDING.md,
  "Platforms"). Astrolog's GitHub runners will make the first real build
  and send us the output.
- **D, the smallest bundle:** `prometheia-catalog-subset` cuts a named
  catalog from the full one (INGESTION.md, "A catalog of named bodies");
  Astrolog's exact list is still to come. Measured:
  - a 69-body named catalog is 7.0 KB;
  - DE440s costs 10.9 MB per century;
  - the perturber kernel costs 3.8 MB per century for all 16 bodies.
- **E, before 1550:** maintainer's decision, 2026-10-25: **cut DE441
  spans**, with no analytic fallback. Done: `prometheia-de-trim`, and
  `de_fetch.py --only de441` makes two pinned cuts. They are
  `linux_p1000p1550.441` (51.1 MB) and `linux_m3000p1550.441` (423.0 MB),
  each bit-identical to DE441 behind DE440 (DE.md, "Cutting a span").
- **F:** nothing needed now.

Later the same day, from Astrolog's use (all fixed, tested and pushed):
- **A catalog body near a file's edge** was refused well inside the file,
  and the failure then stuck for every catalog body, for the engine's
  life (ENGINE.md, "Memo"; `f94fd56`).
- **`sbdb_fetch.py --covariance`** died at the first `A/` object. Now one
  body costs only itself (INGESTION.md, "Orbit covariance"; `6f18eb0`).
- **The catalog is current to 2026-10-25.** `--refresh-since` folded into
  the base gives `sbdb-20260916-refresh-20261025.epm` (1,567,167 records,
  sha256 beside it). It replaced `sbdb-full-20260916.epm`, which is
  removed and can be rebuilt from `sbdb-raw-full/`. The MCP config
  (user scope) serves the new file (`5a0101e`). Astrolog's
  `ephsrv-prometheia.sh` and `qttest.cpp` still named the old file when
  told.
- **Maintainer decisions:**
  - the next catalog update is a full pull;
  - looking a body up by its pre-numbering provisional designation
    (`prov_des`) is deferred (INGESTION.md, "Future ingestion paths");
  - no bundling of open-source dependencies.
- **Waiting on Astrolog:** their first MSVC/macOS compiler output, and
  their draft of the lunar-tidal WELCOME TLV (one value per coverage span,
  absent meaning unknown; we send it absent, TIME.md).

**Disk:** `/nvmraid` is tight: 3.5 GB free at one point on 2026-10-25,
20 GB after the maintainer cleared space. Large data goes to
`/nvm/work/ephe`, linked into `ephe/`: DE441 and its two cuts live there. `de_fetch.py --dir ephe --verify` passes with all three.

### State at 2026-09-29, evening

`initial` = `origin/initial`, gate green, `tools/scheduled.sh` green;
**v0.7.3 published** (library 0.7.3, C ABI 6; `prometheiad` 0.11.0). Nothing
is open with Astrolog and our queue is empty. Start our daemon when a
cross-test needs it, and stop it by the pid you started:

    ./build/prometheiad --ephemeris ephe/linux_p1550p2650.440 \
        --port 47190 --log-level info

Untracked `.claude-config/` and `.envrc` are never staged. Astrolog's spare
is **:47392** (read its identity from `/proc` before trusting a row);
**:47391 is their production daemon and is never touched.** The channel to
the Astrolog session (named `Astrolog`) is open by the maintainer's word:
say what matters at milestones, don't ping-pong, and never cite a commit
hash in the same tool batch as the commit.

**Focus: solar-system objects.** Fixed stars are parked
([STARS_BACKLOG.md](STARS_BACKLOG.md)); don't raise them.

**Data on this machine.** DE440 in `ephe/`. DE441 and its two cuts
(DE.md, "Cutting a span") are in `/nvm/work/ephe/`, linked into `ephe/`. SB441-N16 (full and the 1550–2650 cut) is in `ephe/`.
All are fetched and verified by `tools/fetch/de_fetch.py`
(`--only de440|de441|sb441`, `--verify`). The TDB series is generated from
the pinned ERFA `dtdb.c` in `erfa-raw/` by `tools/gen/gen_tdb_series.py`.
The MCP server config (user scope) serves DE440 then DE441.

**Settled on 2026-09-29, in release order** (CHANGELOG 0.7.1–0.7.3):
- **0.7.1:**
  - star distance rates in closed form;
  - `PROMETHEIA_ERROR_COVERAGE`;
  - ΔT and the epoch seen arriving by every leg;
  - `legtest.py`;
  - the §3.5a rate-bound reading (we meet the A.3 default and send no
    `0x0013`, deliberately).
- **0.7.2:**
  - DE441 behind DE440 (`add_ephemeris`), DE440 answering 1550–2650 to the
    bit;
  - Vondrák 2011 precession everywhere, with the mean obliquity the pole
    angle (§3.5a, Astrolog `d0286de`);
  - JSON rows carry `delta_t_s`, `delta_t_basis`, `ut1_minus_utc_s`;
  - future UTC is TAI − 37 s (maintainer);
  - `capabilities.dates`;
  - A.3 `0x000A` coverage, agreed and shipped both sides.
- **0.7.3:**
  - TDB−TT is the full Fairhead & Bretagnon series (787 terms public, 136 on
    one-day nodes in the engine, 1.55 µs, old speed);
  - a catalog body more than 100 years from its element epoch needs an
    asteroid perturber kernel (maintainer), provisioned by
    `de_fetch.py --only sb441`;
  - small bodies stated against Horizons (≤ 0.9″ over 1600–2500 with the
    kernel).
- **The cross-test** (records z, aa; CROSS-TEST.md):
  - Astrolog's ts.17/ts.18 closed their S8 (obliquity) and S9 (nutation);
  - `DATE_FRAME_BAND` is 0.1 mas;
  - new legs: `hypotheticals` (every shared kind 3 token; `neptune-leverrier`
    re-proved each run as Hoyt's vs Le Verrier's print) and sidereal orbit
    points (after their gate found Swiss's sidereal-node bug, their G30);
  - their cross-engine gate (`tools/ephsrv-prometheia.sh`, their side) passes
    69/69 against our 0.7.3, and their suite 171/171.
- **Every validation residual is explained** (DE.md, ENGINE.md):
  - the Moon's ~1 mas against swetest is swetest reading TT as TDB;
  - the Moon's 56–76 µas against Horizons at −3000/5000/9000 is Horizons
    holding the emission instant as a double of ET seconds (emulated to
    0.22 µas);
  - swetest's ecliptic of date uses the ε_A series, where we use the pole
    angle.

**In progress: a pure-Python, pyswisseph-shaped library**, planned in
[PYTHON.md](PYTHON.md) (maintainer decisions 2026-09-29). Phases 1–3 are done: houses in
the engine, the JSON/MCP `houses` tool ([HOUSES.md](HOUSES.md)), and the
package over stdio and HTTP (`python/`, `prometheia.swe`). Phase 4 (the binary
transport, v4 to `prometheiad`) is done too, so all five phases are.
What's left is time conversion over v4, which Astrolog and we agreed
follows houses.
The v4 houses drop (Astrolog `6235abd`) is vendored: `ephproto.h` 85d21bc8,
registries 9845fef7, 124/124 fixtures through both readers, their 2,244
reference rows agree to 0.000002″. `prometheiad` serves kind 6 (all eleven
systems; SERVER.md "House points").

**Candidates for next** (none started, none promised): the parked items
below. Cross-test legs at DE441 epochs are off the list: Astrolog serves
Swiss's bundled files, which refuse beyond ~2400, and does not plan to
serve further (their answer, 2026-09-29, evening).

**The day's lesson, if only one survives:** four times in a week a claim
here or in Astrolog was bounded by an axis nobody varied — a floor exact
when written, a count that was the width of a list, a bound measured on
five bodies and stated about every object, and a *conclusion* about which
epoch a defect lived at. Their framing, which is the better one: **a
count invites the question "how many?" and a conclusion invites
nothing.**

The grep started as "any sentence with *all*, *every* or *only*" and that
was wrong: **"the region is at the 1900.0 epoch"** contains none of the
three and was the worst of the four. The quantifier is a symptom. The
question to ask a claim is not *does this generalise?* but **which axes
were varied to produce this, and which were held?** — and a claim that
names no axis at all is the one to distrust first. That reformulation
(Astrolog, 2026-09-20) fits all four retroactively, which is how it is
known to be the right one: the floor, the list-width count and the
five-body bound each named no axis either, and the quantifier in each was
incidental.

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

- **catalog-20261027**, published 2026-10-27
  (https://github.com/nrvate/ephemeris-prometheia/releases/tag/catalog-20261027).
  A data-only release tagged at `3ed2b9c`. Its asset is `sbdb-20261027.epm`
  (EPM1 1.2, 1,580,028 records, sha256 `1dd348aa…59919`), fetched by
  `tools/fetch/catalog_fetch.py`. GitHub marked it "Latest" in place of
  v0.7.3; the maintainer gave the badge back to v0.7.3.
- **v0.7.3**, published 2026-09-29
  (https://github.com/nrvate/ephemeris-prometheia/releases/tag/v0.7.3).
  - The library and tools are 0.7.3 (C ABI 6, unchanged); `prometheiad` is
    0.11.0.
  - New:
    - the full Fairhead & Bretagnon TDB−TT;
    - catalog bodies need the perturber kernel past a century, and
      `de_fetch.py --only sb441` provisions it;
    - small-body accuracy stated against Horizons.
  - Moves:
    - positions, by ≤ 5 µas within 1600–2200;
    - the dataset id (`…#c029261c`);
    - the kernel cut's hash (e7b67e32…, same records).
- **v0.7.2**, published 2026-09-29
  (https://github.com/nrvate/ephemeris-prometheia/releases/tag/v0.7.2).
  - The library and tools are 0.7.2 (C ABI 6, `add_ephemeris` appended);
    `prometheiad` is 0.10.0.
  - New:
    - DE441 behind DE440;
    - the Vondrák 2011 default;
    - JSON ΔT and UT1−UTC disclosure;
    - `capabilities.dates`;
    - A.3 coverage.
  - Moves: date-frame answers by ≤ 0.01″, and the dataset id.
- **v0.7.1**, published 2026-09-29
  (https://github.com/nrvate/ephemeris-prometheia/releases/tag/v0.7.1).
  - The library and tools are 0.7.1 (C ABI 6, one status appended);
    `prometheiad` is 0.9.0.
  - New: a fixed star's distance rate in closed form;
    `PROMETHEIA_ERROR_COVERAGE`; protocol v4 re-pinned at the site drop and
    the rate-bound reading (Astrolog `7cbf0d8`, `5f11726`); `ratesweep.py`
    grading per max(1 AU, r); the drift check scheduled.
  - Moves a pinned value twice: star distance rates, and the dataset id.
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
   - **Measured for every leg, 2026-09-20** (`tools/check/blindspots.py`,
     CROSS-TEST.md "What each leg can see arriving"). Each leg is run with
     our daemon on both endpoints, so only its non-comparative checks can
     red, then re-run once per argument with that argument stripped. Green
     means the leg cannot tell the argument arrived. Eighteen legs, all
     measured. **`--deltat` was blind in all eighteen** (seen by all eighteen
     since 2026-09-29, CROSS-TEST.md "Delta T arrives"), and this document
     says every numeric leg sends delta T explicitly so that neither
     server's model enters — with nothing checking that it did. `--jd` is
     blind in thirteen. The record is committed, so a leg that *stops*
     seeing an argument reds against it.
   - **And within the hour the field mattered.** `deltaTSec` moves a
     topocentric star's reported distance rate on their server by 21x while
     its positions stay bit-identical (CROSS-TEST.md, "Finding, theirs:
     `deltaTSec` moves…"). It also explains the one standing row over their
     4e-3 bound: our sweep sends the field, their probe did not. Ours shows
     the same shape two orders milder and that is ours to explain.

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
  header from `qt` `7cbf0d8` (the site drop, vendored 2026-09-29),
  conformance set `07438c7f…`, 109/109 on both readers, judgements 7/7).
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
- ~~**One gate failure on 2026-09-19 that did not reproduce.**~~ Found
  2026-09-20, and it was the test. `server_log_traces_a_request_without_
  its_contents` asserts the log never carries what was asked, and one of
  the secrets it scans for is the site latitude **47.37**. It scanned the
  whole log line, timestamp included — and a line written 47.370 s into
  any minute reads `...T15:49:47.370Z`. Ten milliseconds a minute per
  line, four lines, two builds: about one gate run in a thousand. That is
  why sixty clean runs found nothing; the estimate was in the right place
  and the search was not.

  It surfaced only because `tools/gate.sh` now keeps the failing output
  (`build/gate-failure.log`, 742d36c) and `tools/scheduled.sh` runs the
  gate into a dated log. The first failure after that change was this one,
  with its name and its numbers.

  The scan now drops the `prometheiad <ISO8601> ` prefix from each line
  and reads only the rest. A stripper that returned nothing would make
  every secret absent, so the test also checks the stripper against the
  exact line that took the gate red, and requires the stripped text still
  to contain the message it expects. Fault-injected both ways: a secret
  that really is in the body still fails, and a stripper that strips
  everything fails on two assertions rather than passing.

  **Fourth time in three days that the measurement was wrong and the
  program was fine.**
- ~~The cross-test runs by hand.~~ Closed 2026-09-20 by
  `tools/check/crossrun.py --record` (CROSS-TEST.md, "One command"): one
  command starts our daemon, runs every leg, then `corrapplied.py` and
  `ratesweep.py` against each server, stops only what it started, and diffs
  its table against the previous record so a run that reproduces the 99
  standing findings says it found nothing. It still needs *their* daemon,
  which it never manages, so it stays out of `tools/gate.sh`.
  `tools/scheduled.sh --with-cross` runs it, opt-in on purpose: a thing
  that reaches outside this tree should be asked for and not arrive on a
  timer. The sequence is no longer something to remember.
  - **Its own assertions, 2026-09-20** (`tools/check/crossruntest.py`): five
    named, four of which were silent -- a record never written, a record
    with no rows, a comparison that raised, and an interrupted run -- plus a
    narrowed run that reported every leg it skipped as a change in their
    server. Exit 2 now means "could not run" and nothing else. Driven with
    scripted steps and two sockets, so no daemon starts and theirs is never
    touched.
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
  which is why we still send no `0x0013`. It needs a daemon, so the sweep
  itself is not in `tools/gate.sh`; `tools/scheduled.sh` runs it through
  `crossrun.py --with-cross`.
  - **The registries checker has one too** (`tools/check/registriestest.py`,
    2026-09-20), and it is the last gate-resident checker to get one. Seven
    cases against a *copy* of the vendored v4 pair — the pair itself is the
    byte-level authority and is never touched — each mutating it one way:
    a registry removed, one added that no table pins, a symbol renamed, a
    value changed, an entry added, and a reserved message type the header
    starts spelling. That last is a new check: the checker skips values
    "reserved by design", so a header that begins implementing one was the
    single way its coverage could shrink with every count unchanged.
    - Two things fell out of writing it. A drafted `nothing-checked`
      assertion turned out **unfirable** — a `registries.json` empty enough
      to make the count zero fires `pin-lost-a-registry` twenty-two times
      first — and was dropped rather than shipped as a check that cannot
      fail; the selftest's refusal of an unexercised assertion is what
      caught it. And a missing header symbol used to produce a *second*,
      misleading message telling the reader to re-vendor the registry, when
      what moved was the header. One fault, one instruction.
    - Every mutation asserts that it changed the file. The first run of the
      reserved-value case anchored on a string the header does not contain,
      and said so instead of passing.
  - **Its own assertions are in the gate** (`tools/check/ratestest.py`,
    2026-09-20). What nothing graded was not the arithmetic: `asked` and
    `answered` were counted, printed, and compared to nothing, so a sweep
    in which *every* object came back unanswered found no exceedance,
    printed "within its advertisement everywhere this sweep reached" and
    exited 0. In the tool that produced a finding against another
    project's server. It now declares six assertions, five of them about
    the sweep having happened at all — an empty grid, no WELCOME anywhere,
    a bound that changes mid-sweep, nothing answered, too little answered
    — and the exit status is exactly "did any fire". Seven scripted
    servers, one per assertion, under two seconds, no daemon.
  - It also latched the bound from the *first* reply whether or not that
    reply carried a WELCOME, so one dropped connection at the start would
    have judged a server advertising 5e-3 °/day against A.3's 1e-5 default
    — four hundred false accusations from a single failed request.
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
  - It needs a daemon, so it stays out of `tools/gate.sh`, which must stay
    seconds. ~~Nothing runs it on a schedule.~~ `tools/scheduled.sh` does
    (2026-09-20), as it does `crossrun.py` behind `--with-cross`.
  - **`tools/check/loadselftest.py`** (2026-09-20) falsifies all nine of
    the load tool's assertions in ten cases and requires the *right* one to
    fire — a case passes only when the named assertion reds and no other
    does, since "something went red" passes a gate whose bound was deleted.
    Falsified against itself four ways. **It shipped as the overclaim it
    exists to catch**: the first version exercised seven of the nine it
    declared, so `canary-none-graded` and `failures` could each have been
    deleted from `prometheia-load` unnoticed. Found by reading the case
    table against the assertion table — a count, not a run. It now refuses
    to start if an assertion has no case. It also found a real caveat on its
    first run:
    `--memory-bound` measures growth *during* a run, so a server already at
    its cache plateau passes any bound, and it must be pointed at a fresh
    daemon (SERVER.md).
  - **And one level up, 2026-09-20** (`tools/check/runners.py`, SERVER.md
    "Is every check run by something?"): all of this asks whether a check
    can go red, and nothing asked whether anything runs it. The Astrolog
    side found their own soak selftest wired into no runner that day. Ours
    now says, in the gate, that every tool under `tools/check/` is either
    invoked by a runner or carries a reason why nothing invokes it — and a
    mention in a comment is not an invocation, which matters because the
    gate names five tools it does not run. Two tools were run by nothing:
    `stars_fk5.py` and `ephproto4_fixtures.py` (scheduled since 2026-09-29), both by hand, both with the
    reason printed on every run.
  - **The hole this does not close.** Every fault injection described in
    these documents was done by hand once and never re-run. Eight tools now
    have selftests (`prometheia-load`, `corrapplied.py`, `crosstest.py`'s
    adjudicators, `ratesweep.py`, `ephproto4_registries.py`, `stars_fk5.py`,
    `ephproto4_fixtures.py`, `crossrun.py`), which is every tool under
    `tools/check/` that accuses; `crosstest.py`'s legs have
    `tools/check/legtest.py` since 2026-09-29 (CROSS-TEST.md, "The legs'
    own selftest").
  - **`tools/check/adjudicatetest.py`** (2026-09-20) is the second, and
    **the first falsification in this repo that runs on every commit**:
    `crosstest.py`'s three `adjudicate_*` functions are pure functions of
    the results table, so 29 cases drive them with no daemon and no
    ephemeris in under a tenth of a second, and it is in `tools/gate.sh`
    (which still finishes in ~23 s). It traces the three functions and
    fails if any executable line has no case, so a branch added without
    one fails the gate. Falsified three ways: two inverted branches each
    failed exactly one named case, and deleting a case made the coverage
    check name the three lines it orphaned.
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
  - **The cross-test's own row counts are now derived rather than
    reported** (`5ed3f48`, 2026-09-20). Not a selftest, the same class of
    defect one level down: `crosstest.py` read its Horizons corpus from
    gitignored `horizons-raw/` and used whatever rows were present, so an
    interrupted fetch would have read out as "N of N agree" with a smaller
    N — the number every record in CROSS-TEST.md is written up from.
    `corpus_rows()` derives the expected count from the committed
    request's own `TLIST` and refuses a short file. `_star_records()` was
    the same over a *committed* file: a regex that on any drift in
    `src/star_catalog.inc`'s layout matches fewer lines and reports the
    remainder as the catalogue; it now requires a match for all 9,290
    entries inside `kStarRecords[]`. Falsified four ways; legs `same`,
    `stars` and `bary` re-run against both daemons afterwards, unchanged.
- ~~**Kinds 3 and 4 have no cross-test cell.**~~ Half wrong, and measured
  on 2026-09-20. **Kind 3 has had a live cell since the first run**: the
  `hamburg` leg asks both servers for the eight Hamburg points by name and
  both answer, 24 rows agreeing to 6e-4″ with distances matching to 1e-12
  AU. The harness drives both servers with *our* reference client, so what
  the Astrolog client does or does not send never constrained it -- the
  sentence had carried their client's limitation across to a leg it does
  not touch.
  **Kind 4 cannot have one, and for a sharper reason than "their client
  does not send it": their server does not serve it.** `astrolog-ephd`
  advertises `caps kinds 47` (bits 0,1,2,3,5), with bit 4 clear; ours
  advertises 63. A kind-4 request would draw ERROR 11, correctly. It stays
  covered as a fixture -- the protocol owner's own element sets and
  answers, vendored at `third_party/ephproto/v4/elements/` with its digest
  pinned in `tests/test_ephproto4.cpp` -- which is the right shape for a
  question only one implementation can answer.
- ~~`/llms.txt` in both repositories, or one combined?~~ Settled
  2026-09-20 (JSON_API.md, "`/llms.txt`"): one per surface, and Astrolog
  exposes no agent tools to have one. It obliges them of nothing — if they
  ever serve agent tools they serve their own file beside them — so it did
  not need to go to the maintainer.
- **We advertise no rate bound at all, and would miss the default.**
  Found 2026-09-20 by the widened sweep (CROSS-TEST.md, "The ΔT axis
  becomes part of the sweep"). `prometheiad` sends no A.3 `0x0013`, so
  the registry default 1e-5 deg/day / **1e-9 AU/day** is what a client is
  entitled to assume, and all 60 Polaris rows in that run exceed the AU
  figure — worst 2.4722e-05, four decades over. The longitude side is
  fine (worst 5.2e-10). It is the f64-ulp star-rate artefact already
  diagnosed, seen from the advertisement rather than from the arithmetic,
  and it is the same question Astrolog's 4e-3 raised, pointed here. Two
  ways out and they are not equivalent: fix the star rate analytically
  (changes a wire value), or advertise a figure we can meet. Both are
  maintainer calls; the second is also a `docs/C_API.md`-adjacent promise.
- **No instrument compares two engines on rates.** Named 2026-09-20 and
  not filled (CROSS-TEST.md, "The instrument gap this fell through"). The
  `stars` leg compares the two servers on **positions**; `ratesweep.py`
  and the `rates` leg compare one engine's reported rate with its own
  difference — **one engine, with itself**. A quantity both engines report
  self-consistently and differently from each other passes both, and Vega
  is one: its distance rates differ by 7 km/s while its distance agrees to
  0.011%. Found by reading four numbers, not by a tool. Filling the gap
  needs an outside reference for a star's distance rate and there is none
  — Horizons, the anchor the other legs lean on, does not serve fixed
  stars — so this stays recorded rather than closed.

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

- **Their advertised rate bound.** `5e-3` °/day and **`4e-3` AU/day**.
  - **This document said `1e-4` on the AU axis and was wrong by a factor of
    forty**, for as long as it stood. Corrected 2026-09-20 by reading it off
    the wire — `ratesweep.py --server 127.0.0.1:47392` prints
    `bound 0.005 deg/day, 0.004 AU/day absolute (advertised)` — after the
    Astrolog session said so. A capability another project advertises is
    read, never remembered; this is the second time a number here was
    carried from memory and the second time it was wrong.
  - **The angle axis is covered.** Our worst against their fixed build is
    **1.3939e-3** °/day (the topocentric Moon at Quito in 2100), theirs
    1.7255e-3 on the same object, site and epoch. `5e-3` carries about 2.9×.
  - **The `3e-5` AU/day recommendation is withdrawn.** Its basis,
    "the measurement is 1.0014e-5", appears nowhere else in these documents
    and is not our worst: on our own server the widest star distance rate is
    **2.4722e-5** AU/day (Polaris, topocentric Quito, JD 2415020.5). A
    recommendation two to three times a measurement is worth nothing when
    the measurement was not the widest one, and this is the third time this
    number has been wrong from being set *at* something.
  - **The AU axis is still exceeded at one cell, and this is not news.**
    Polaris, topocentric Quito, JD 2415020.5 → 7.4824e-3 AU/day, 1.87× the
    `4e-3`. Re-measured 2026-09-20 and reproducible bit for bit, the only
    row over in an eighteen-row subgrid — but that subgrid samples **one
    ΔT per configuration**, and sweeping ΔT at that instant puts **eight
    of eleven** values over (CROSS-TEST.md, "The standing row is not one
    row"). The transition is between 25 and 30 s, which is below every ΔT
    a twentieth-century date carries. **CROSS-TEST.md already carries it**
    ("The chase does not converge") with the adjudication: no honest
    absolute number exists while a tolerance in AU/day is applied to a
    column whose ulp is 3.7e-9 AU (6e-9 until corrected 2026-09-29), a bound wide enough to cover it with
    headroom would be ~2e-2 AU/day and describe nothing, and the fix is the
    §3.5a sentence — which landed. It is a standing, adjudicated row, not a
    finding, and it was nearly re-reported to them as one from this page's
    stale copy of the numbers.
  - **Our grid gained what theirs found.** Their worst distance rate was
    Aldebaran, topocentric, on J2000 axes; we swept four stars and no
    topocentric J2000 configuration, so the cell was not in our grid at all.
    Both added (`ratesweep.py`). Aldebaran measures 4.7075e-6 on ours and
    7.8385e-4 on theirs.
  - **No number is recommended this time.** The measurements are above; what
    to advertise is theirs to set, and CROSS-TEST.md's conclusion stands:
    this side suggested `1e-4` once at a moment when its own table already
    showed 7.48e-3 on their wire, false by 75× against data it held. The
    lesson is not "pick a wider number" but "read the table before
    recommending one".
  - **Asked again 2026-09-20**, with both sides' numbers and one question:
    do they intend to change what `0x0013` advertises, and to what. Our
    sweep grades against whatever is on the wire, so if it stays
    `5e-3`/`1e-4` we grade against that and say so in the record rather
    than carrying the gap as a finding. The ambiguity is the only problem;
    either answer settles it. **Awaiting their reply.**
  - The same message relayed, reciprocally, the defect shape found five
    times here that day -- *the denominator came from the same input that
    had lost the rows* -- including the one instance that would have
    reached them: `crossrun.py`'s leg-by-leg diff reported legs a narrowed
    run had not run as `N -> 0`, a change in their server where the only
    thing that changed was which legs were asked for. No such row was ever
    sent; the near miss is the point, and it is the same class as the
    `corrapplied.py` 0x0004 mis-attribution reported earlier.
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
  true is the original point: at f64 the floor is still ~5.7e-6 AU/day (9e-6
  until corrected 2026-09-29) for
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

Declined: zstd payloads, on measurement (SERVER.md, "Not implemented"); acting on
`deadlineMs` (maintainer, 2026-09-29: the field is read and ignored).
Settled 2026-09-29, recorded in STARS_BACKLOG.md and CROSS-TEST.md:
- Vega's radial velocity (kept at −13.5 km/s);
- the analytic star rate (done);
- what we advertise for rates (the §3.5a reading; no `0x0013`).

- Nightly or automated catalogue release builds: an idea only.

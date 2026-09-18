# Handoff — where the work stands (2026-09-18)

A point-in-time snapshot for anyone picking the repository up. Durable
working rules live in [CLAUDE.md](../CLAUDE.md); the decision history in
[DESIGN.md](DESIGN.md); the protocol state in [SERVER.md](SERVER.md).

## Now: client-server cross-testing (2026-09-18)

The maintainer's direction: "a client server testing paradigm is
important". The runbook is [CROSS-TEST.md](CROSS-TEST.md); the harness is
`tools/check/crosstest.py`, which runs the reference client against both v4
daemons with JPL Horizons as referee. `tools/check/wirelib.py` is the one
reader of the client's output. The first full run is
`docs/crosstest/2026-09-18.tsv` (734 rows).

- **Launching both daemons.** Ours: `build/prometheiad --ephemeris
  ephe/linux_p1550p2650.440 --port 47190 --threads 1`. Theirs, from
  `/nvm/work/ephv4`: `./astrolog-ephd --bind 127.0.0.1 --port 47291
  --threads 1 --ephe "/nvm/work/ephv4/ephem;/nvm/work/ephv4"`. Then
  `python3 tools/check/crosstest.py --out docs/crosstest/<date>.tsv`, and
  stop both with `pkill -x`.
- **Rerun after the Astrolog fixes** (CROSS-TEST.md, "Rerun"): topocentric
  Earth rotation now honours the request's ΔT (Moon 12″ → 0.13″), and
  error 3 replaces 4 except on the nine Jupiter-centred rows at 1800.
- **Still open with the Astrolog side:** heliocentric light time (they say
  it is Swiss's model; the anchor puts them 0.38″ from Horizons, us at µas);
  their server accepts heliocentric mask 7, which it no longer advertises
  and ours refuses with ERROR 11, an interop split; deflection from the
  barycentre and Jupiter (7 mas on Mars, unadjudicated); Venus at 2 mas.
  Rerun once e9d406e is built. Legs 3–5 are theirs to drive.
- **Open on this side:** the topocentric anchor, which needs Horizons' UT1
  recovered from its sidereal time (tests/test_horizons.cpp does this).
- **Also done this stretch:**
  - the per-object error contract, written into SERVER.md;
  - correction masks advertised exactly, with ERROR 11 for the rest;
  - `corrapplied.py` can no longer pass on zero cases;
  - a review of this session's own code, with all five fixes landed;
  - a review of the Astrolog plugin, whose seven real findings they fixed
    (the eighth, sidereal stars, was a false alarm on this side).

## Done: hypothetical bodies (2026-09-18)

The maintainer, a Uranian astrologer, needed the Hamburg School's
transneptunian points; in Astrolog they are first-class planets by default.
Conventions are in [HYPOTHETICALS.md](HYPOTHETICALS.md).

- **Shipped:** the engine (`calc_elements` for the protocol's kind 4,
  `calc_hypothetical` for kind 3, from JSON Lines element files),
  `prometheiad` serving both, C ABI version 5 (reviewed by the Astrolog
  side before it landed), `ephem` (`hyp:TOKEN`, `hyp:all`) and the wire
  client (`--hyp`, `--elements`).
- **Spec:** "the kind-4 elements rule", released by the Astrolog side as
  five sentences (four at `176e333` plus a fifth, an amendment the
  maintainer approved). Its numeric fixture is vendored and gated
  (`ephproto4_kind4_elements_fixture`). The one known difference is Swiss's
  truncated k, 1.4 × 10⁻¹² relative, and it is bounded in the test.
- **Shipped elements:** the eight Hamburg points, taken from the Swiss
  distribution's `seorbel.txt` as a recorded exception
  ([DESIGN.md](DESIGN.md), "Exposures"). They match `swetest` to 0.00076″,
  and match what every supported Astrolog install reads. Also shipped: Le
  Verrier's Neptune. Adams's and Lowell's planets and Transpluto are not
  planned. The rest of the A.15 list works only from an operator's file.
- **Cleanroom:** a Swiss-source exposure and its remedy are recorded in
  DESIGN.md. `src/elements.cpp` and its tests are written only by a session
  that never saw the exposure.
- **With the Astrolog side:** see "Now", above.

## State at the previous snapshot (2026-09-17, night)

- HEAD = `origin/initial` = `fa4e0b3`, tree clean, gate green (21 suites
  in Release and ASan+UBSan).
- **Protocol version 4 §3 is fully implemented**, end to end:
  - `afb0d1e` — the locked conformance fixtures gate in C++ (91/91,
    set digest pinned) plus the by-name registries check in the gate.
  - `1c932d0` — the session rewrite onto v4: NAIF bodies, profiles,
    instant lists, batched LOOKUP, ΔT from the request, datasetId over
    the data files' contents, `corrApplied` as the structural table,
    deflection honoured at the barycentric observer; the v3 wire map
    and header deleted.
  - `28661cb` — SEGDATA on the 32-day lattice (tropical coefficients,
    the ayanamsa as its own scalar series), row-block compute (~2 ms
    slices), CANCEL that stops work, priority, and the cell caches
    wired into the loop. Docs follow-up `405cb94`.
  - `9bdb753` corrected a records error (dates written a day ahead).
  - `fa4e0b3` — from the Astrolog phase-7 review: ENGINE.md's stale
    orbit-point sentence rewritten, and `prometheia.pc` fixed so the
    build tree links (absolute paths + public zstd/C++-runtime deps;
    the library is static-only).
- The migration list in SERVER.md is **complete**; nothing build-shaped
  is queued. Measured anchors: DE440 heliocentric Jupiter, 90-day ask
  at 0.1″ → 4 segments, worst residual 0.031″; the Lahiri ayanamsa
  series 0.051″; a repeated request served wholly from the cell cache.

## The Astrolog collaboration

The ephemeris protocol is theirs; we are the §3 counterpart and pinned
a vendored copy (checksums in `third_party/README.md`). §3 is locked
from both sides; no protocol rounds are owed. On their side: phase 4
(selection re-plumb) was in flight when this was written; their
**phase 6 remote adapter will be the first live consumer** of our
DATA/META — the armed check is a truthful `corrApplied` against real
traffic (their client and our `prometheia-wire-client` can point at
either daemon), and it is now a tool rather than a plan:
`tools/check/corrapplied.py --host H --port N` runs it against any v4
server and is green on ours (SERVER.md, "Checking that it is true");
their **phase 7 plugin builds against
[C_API.md](C_API.md)**, a cross-repo contract.

2026-09-17, night: their phase-7 review measured our engine moving
planetary-node answers by ~10.5″ and asked whether it was intended. It
was — the mover was `cf889eb` (14:43 that day: corrections apply to
orbit points as sent, geometric() unchanged to the bit), settled jointly
for v4; [ORBIT-POINTS.md](ORBIT-POINTS.md) carries the rule and the
magnitudes. Verified by probes on both sides of the change; the answer
went back through the maintainer-relayed mail channel, with the advice
to re-pin those legs citing `cf889eb` (mask-0 comparisons are the
portable ones for planetary points).

## Parked (maintainer go-ahead required before starting)

- Vondrák 2011 long-term precession (IAU 2006 degrades far from J2000;
  matters for DE441's ±13k-year span).
- zstd-compressed wire payloads.
- The `deadlineMs` strategy switch — parsed and advisory today, honestly
  documented as unimplemented in SERVER.md.
- Nightly/automated release builds of catalogs (idea only).

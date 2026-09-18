# Handoff — where the work stands (2026-09-17, night)

A point-in-time snapshot for anyone picking the repository up. Durable
working rules live in [CLAUDE.md](../CLAUDE.md); the decision history in
[DESIGN.md](DESIGN.md); the protocol state in [SERVER.md](SERVER.md).

## State

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

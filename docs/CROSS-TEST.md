# Cross-testing two servers

A plan for the first live session between `prometheiad` and `astrolog-ephd`,
written before the session rather than during it. Astrolog's phase 6 brings up
a remote adapter and a v4 daemon on their side; at that point four pairings
exist where today there is one, and the point of writing this now is that the
useful decisions are the ones made before anyone is mid-debug.

This is a **test plan, not a gate**. Nothing here runs in `tools/gate.sh` — it
needs two daemons and two codebases. The permanent artefacts it should leave
behind are a recorded leg table and, where a disagreement turns out to be real,
a test in whichever repo was wrong.

## The rule that matters most

**Neither side changes code to make the other's number match until an
independent anchor has adjudicated.**

Two implementations converging on each other is not validation; it is the exact
mechanism by which both end up wrong in the same direction, and it is
unrecoverable afterwards because the disagreement that would have revealed it
is gone. This is not hypothetical between these two projects. We have already
produced, on the same day, two confident and wrong attributions that agreement
would have cemented:

- Our first light-time measurement on Jupiter's node read 20.83″ and looked
  like a clean confirmation of the peer's reading. It was an artefact of
  retarding the observer along with the source, which is aberration in
  disguise. The tell was two columns agreeing to four digits.
- Their first attribution of a 14-assertion failure was a sticky `tid_acc` in
  a Swiss fork. Specific, plausible, and wrong — the fork was not linked into
  the binary at all.

Both were caught by an independent check. Neither would have been caught by
the other server agreeing.

So: a leg is **green** when both servers agree *and* the answer sits inside the
anchor's tolerance. Two servers agreeing with each other and not with the
anchor is a **finding**, not a pass, and it is the most interesting outcome
this exercise can produce.

## The matrix

|  | `prometheiad` | `astrolog-ephd` |
|---|---|---|
| **Astrolog's client** | the first external consumer of our DATA/META — the event this is for | their own gate; runs already |
| **`prometheia-wire-client`** | what we test today | an independent reader against their daemon |

The two diagonal cells carry the new information. Their client against our
server exercises everything that has never been exercised: our wire, chunking,
error taxonomy, CANCEL, priority and caches, driven by a consumer with its own
assumptions rather than by the tests we wrote for ourselves. Our client against
their daemon gives them a second opinion from a reader that is not their own
gate — the same independence that found 85/85 on their v4 fixtures.

Both servers should also be run through `tools/check/corrapplied.py`, which
compares a server only with itself and therefore presumes nothing about either
engine.

## The anchors, and which one adjudicates what

Measured, and documented in [VALIDATION.md](VALIDATION.md) and
[ENGINE.md](ENGINE.md).

| anchor | what it settles | our measured agreement |
|---|---|---|
| JPL `testpo.440` (13,201 points) | DE interpolation itself | 1.4e-14 AU |
| JPL Horizons, **astrometric ICRF** | light time, observer geometry, frame | 6 µas geo, 11 µas topo; Moon ≤ 0.006″ 1900–2100 |
| JPL Horizons, small bodies + SB441-N16 | perturbed integration | 0.0053″ at ±10 yr |
| `swetest`, differential | precession/nutation of date | 0.0005″ |

**Reduce every disagreement to astrometric ICRF before taking it to Horizons.**
This is the non-obvious part and it comes out of our own corpus. Horizons
refers *apparent* RA/Dec to the EOP-corrected IAU 1976/80 equinox, which is a
documented −53 mas from the IAU 2006/2000A equinox we use, and its ecliptic of
date uses an obliquity 42 mas larger. Comparing apparent places to Horizons
therefore means subtracting published constants first, and in a live session a
51.8 mas constant offset is very easy to mistake for a real disagreement.
Astrometric ICRF has no such correction and is the clean referee.

Two further traps in the anchor itself, both already paid for:

- **Sun-centred "apparent" from Horizons is referred to the Sun's equator.**
  Latitudes match a rotation to the solar pole exactly; longitudes differ by a
  constant 14.56°. Those rows are not comparable and must not be used as an
  anchor. Only the Sun-centred *astrometric* rows are.
- **Outside Horizons' EOP era** (before 1962, after 2026) it falls back to
  plain IAU 1976/80 and apparent positions diverge from ours by up to 0.52″.
  Anchor legs stay inside the EOP era; anything outside it is reported, never
  gated.

## Which disagreements are defects, and which are expected

The most valuable thing this document can do is say, in advance, which numbers
are allowed to differ. Otherwise a known model difference eats an evening.

**Tier 1 — must agree to roundoff.** Geometric positions (corrections mask 0)
of DE bodies, both servers reading the same DE440 file, in ICRF. Nothing
model-dependent remains: this is the same Chebyshev data evaluated twice. A
disagreement here is a defect in one of the two, and `testpo.440` says which.

**That premise is false for `astrolog-ephd` in its default configuration,**
and the first draft of this plan got it wrong. Per the Astrolog side: their
daemon links the Swiss library, which by default reads Swiss's own `.se1`
files — a compressed re-fit of the JPL integration with its own segmentation
and a stated fidelity of about 0.001″ for the planets — and their bundled set
is DE441, not DE440 (measured from the running binary, not read off a
filename). A default-configuration mask-0 leg therefore has a floor set by
that refit, not by roundoff, and `testpo.440` cannot adjudicate it: testpo
validates JPL's interpolation, and the refit is a generation downstream.

So a tier 1 leg **must put `astrolog-ephd` in its JPL mode, reading the same
DE440 file this server reads**, and must say so in the leg row. It is not the
default, and a leg that forgets it silently becomes a tier 2 leg wearing a
tier 1 label.

**Tier 2 — must agree to ~1 mas.** Astrometric and apparent places of bodies;
osculating orbit points. Measured precedent between these two engines: Jupiter's
osculating ascending node, ours 20.8367″ against theirs 20.837001″; lunar orbit
points to 0.0002″; the Moon's own apparent place 11.4249″ against 11.424861″.
A disagreement above a few mas here is worth chasing; Horizons adjudicates.

**Default-configuration mask-0 legs belong here too**, recorded with the reason
"Swiss `.se1` refit of DE441, not the DE itself" so nobody chases it twice. The
band for those is set by the refit's documented fidelity, and should be written
as that — our own measurement would only rediscover a number that already has
a source.

**Tier 3 — expected to differ; agreement is not the test.** These are model
choices, not errors, and a gate on them would be wrong:

- **Mean elements.** Our mean nodes differ from theirs by 0.006–0.025″ because
  our mean-element fits are different fits. Report the number; do not chase it.
- **ΔT.** Different models, legitimately. Pin the ΔT explicitly in the request
  (v4 §3.5 lets the client send a value or a table) so that a ΔT difference
  cannot masquerade as a position difference. **Every numeric leg should send
  its own ΔT.** On the Astrolog side this is more than hygiene: their library
  derives ΔT from the DE number of the file currently loaded, so their ΔT
  depends on which dataset is open and on *when* it is asked relative to the
  first file open — the bug they fixed in phase 4c, worth 0.037 s at 1900.
  Sending ΔT removes that whole axis. Ours takes no ephemeris state
  ([TIME.md](TIME.md)).
- **Ayanamsa variants**, where the definitions differ.
- **Small-body solutions**, unless both sides are demonstrably on the same
  SBDB solution and the same perturber set.

**Courtesy tier — recorded, never gated.** Quantities that are not portable by
construction, per [ORBIT-POINTS.md](ORBIT-POINTS.md) and v4 §3.5a:

- **Orbit points with a proper subset of the corrections.** An astrometric
  lunar node is 19.105″ from the apparent one here, because barycentric
  retardation is left with nothing to cancel it, and a few mas in an engine
  that retards geocentrically. Both are self-consistent, both report
  `corrApplied = {lightTime}` truthfully, and they differ by 19″. Corrections
  on an orbit point are interoperable applied **in full or not at all**.
- **The Moon's orbit points generally** — the two-routes case. They already
  re-pinned these as the courtesy tier in `1fe84a8`.

For planetary orbit points, the **mask-0 leg is the portable comparison**: light
time there is thousandths of an arcsecond, so a geometric leg cannot drift the
way an apparent one can when either engine changes its correction conventions.
That is what moved 10.5″ under them in the first place.

## The bisection ladder

When a number disagrees, add one thing at a time. Each rung isolates exactly
one stage, and the rung where it breaks is the answer.

1. **Mask 0, ICRF, J2000.** Raw ephemeris interpolation and nothing else.
   Breaks here → the DE reader or the file. `testpo.440` settles it — but only
   if `astrolog-ephd` is in JPL mode on the same file. In its default
   configuration, a break at this rung is first a question about the `.se1`
   refit, not a defect.
2. **Mask 1 (light time), ICRF.** Adds the light-time solution.
   Breaks here → the retardation loop, its convergence, or which body is
   retarded. For an orbit point, remember that retarding the *observer* is
   numerically indistinguishable from aberration.
3. **Full corrections, ICRF.** Adds deflection and aberration.
   Breaks here → the correction models. Horizons astrometric adjudicates the
   light-time part; the observer-velocity term shows up as ~20″ at J2000.
4. **Apparent of date.** Adds precession, nutation, equinox and obliquity.
   Breaks here → frames. `swetest` differential adjudicates at 0.0005″, and
   Horizons only after the published −53 mas equinox and 42 mas obliquity
   offsets are removed.
5. **Observer changes** (topocentric, heliocentric, planet-centred) last, one
   at a time. The correction narrowing is **by observer, not by kind**:
   measured on Saturn at J2000, the observer-velocity term is 0.0000″
   barycentric, 0.0107″ at the Sun, 10.4372″ geocentric, 8.7706″ centred on
   Jupiter.

## Legs the numeric matrix cannot see

Two legs proposed by the Astrolog side. Both test something every other leg
assumes rather than checks.

**The silent-fallback leg.** Swiss does not fail when a file is missing or an
instant is out of its coverage: for the main planets it silently answers from
its analytic Moshier model instead. They measured it — with every ephemeris
directory removed, the Sun still computes, 0.04″ off, with no error and no
flag. That is a server answering from a different model than the one its
`datasetId` names, which is the class `corrapplied.py` exists for, one level up.
So: request an instant outside the pinned ephemeris's coverage, and an object
the pinned set does not cover, and require **either** a per-object error (3,
out of range; 4, data missing) **or** a truthful source string — never a
silent substitution. Run it against both servers. It needs to be a named leg,
because every other leg in the matrix would pass while being quietly answered
by a different model.

**Chunk, grid and list equivalence.** The same question asked as one grid, as
several chunks and as an explicit instant list must give identical bytes. The
v4 row formula is normative — row *r* is `(jd1+jd2) + (double)(r·stepNs)/86400e9`,
computed from *r*, never accumulated — and a server that accumulates drifts at
large *r*. That presents as a position disagreement that walks with row index,
which is easy to misread as physics. A 10,000-row grid compared on the
*implied times* isolates it in one leg.

## Measurement traps to encode in the harness

- **Drive the client with a one-source chain.** Astrolog's client now has a
  fallback chain: when the selected source cannot answer an object, the next
  one does, and the chart still comes out with a notice. A leg pointing their
  client at `prometheiad` with a chain of `prometheia,swiss` would, on any
  failure, show our engine agreeing with Swiss to the bit — because it *is*
  Swiss. That is the two-implementations-converging failure arriving through a
  side door, and it is unrecoverable for the same reason. Pin the chain to the
  single source under test and assert the fallback notice is absent on every
  numeric leg. The Astrolog side is making this a named precondition of their
  harness.

Every one of these has cost this project or the Astrolog project real time.

- **Compare angular separations, never longitude differences.** A degree of
  longitude is a degree of sky only on the equator. Their 0.063″ "disagreement"
  on the Moon was the cosine factor at 5.17° latitude and nothing else.
- **Use `atan2(|a×b|, a·b)`, never `acos(a·b)`.** The cosine of a
  milliarcsecond rounds to exactly 1.0 in f64, so an `acos` separation returns
  a clean zero for every small angle — a false negative sitting precisely at
  the deflection scale, which reads as a server correctly applying nothing.
  This bug was in `corrapplied.py` before it was ever pointed at anyone.
- **Do not gate on `corrApplied`.** v4 §3.5a forbids it and the field cannot
  bear it: our lunar orbit points agree to 0.0002″ while reporting *different*
  `corrApplied`, and our mean nodes differ by up to 0.025″ with *identical*
  `corrApplied`. It explains a difference; it never predicts one.
- **Suspiciously exact agreement is evidence about the measurement.** Two
  independent quantities matching to four digits, or a column of exact zeros
  where the physics says small-but-nonzero, means look at the instrument.
- **A check that has never gone red is not known to work.** Fault-inject before
  trusting a green: `corrapplied.py` had two bugs that only a deliberately
  broken server revealed, one of which made a whole check vacuous.

## What each leg records

One row per leg, machine-readable, so the session is reproducible afterwards
and so a later regression has something to diff against:

    epoch (TT, two-part) · object · observer · frame · plane · corrections mask
    · ΔT sent · prometheiad answer · astrolog-ephd answer · separation (arcsec)
    · anchor value and source · tier · verdict

A verdict is `agree`, `expected-difference` (with the tier-3 reason), or
`finding`. "Both servers agree, anchor disagrees" is a `finding`.

## What this does not test

Worth stating so nobody reads a green matrix as more than it is.

- **Robustness.** Neither the numeric legs nor the protocol legs feed malformed
  input. Astrolog runs a `ROBUST` gate; this repository has five hand-written
  malformed frames in `tests/test_server.cpp` and no fuzz corpus, so our wire
  parser has never seen a hostile byte. That gap is ours and predates this
  plan.
- **Load and duration.** Single-client, short-session. No soak, no concurrent
  clients, no memory-growth measurement.
- **Correctness of anything only one side implements.** Kinds 3 and 4 are
  unadvertised on both sides by agreement (see [SERVER.md](SERVER.md)), so
  there is nothing to cross-test there.

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

**Done, 2026-09-18.** Against `astrolog-ephd`, 27 of 29 cases were checked,
with all three checks running on all 27; against `prometheiad`, 27 of 29.
Both pass. The skips are legitimate on both servers: the Sun seen from the
Sun's centre, and a node seen from its own body. Against `astrolog-ephd`, the
truthfulness of deflection or aberration asked for alone is *inapplicable*,
because Swiss cannot drop light time on its own. That server honours exactly
masks 7, 0, 3, 5 and 1, and the tool reports the gap rather than passing it.
The first run had printed OK after checking zero cases. The tool now fails
on that, and fixing it exposed that `prometheiad` was under-advertising its
masks (SERVER.md, "Correction masks").

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
tier 1 label. *As of 2026-09-18 `astrolog-ephd` has no JPL mode at all* (it
always reads `.se1`), so no tier 1 leg can run against it yet. The runbook
says what runs instead.

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

**The elements tier — self-adjudicating.** A kind-4 answer is a pure function
of the elements the client sent: two-body motion by a rule both sides agreed,
with the mean motion taken from the Gaussian constant and not from either
server's ephemeris ([HYPOTHETICALS.md](HYPOTHETICALS.md)). No model choice is
left to hide behind, so two implementations must agree to the precision of
their Kepler solvers, and any disagreement is a defect in one of them. That
makes it the one leg in this plan that needs no external anchor. It is also
the one where a disagreement is *informative* rather than ambiguous.

It runs as a fixture rather than a live diagonal, since the Astrolog client
does not send kind 4. The protocol owner generates element sets and answers
from its implementation; this server is checked against them, numbers only.
The first cases are the ones that are degrees apart under a wrong rule:

- two sets differing only in the term count, a century from the epoch;
- two terms with M's second coefficient zero and the node drifting — the case
  a count-keyed rule freezes;
- a genuine M(T) set, where the polynomial is the whole mean anomaly;
- an Earth-centred orbit, which pins the mass-ratio divisor.

Compare at TT (the `_ut` forms bring in each engine's Delta T and belong to a
different tier), geometric first, then with each correction.

**First run, 2026-09-18.** The four cases above were run once by hand, with
inputs from this side and answers from both. All four match. The worst
disagreement is 0.3 mas, on an Earth orbit of a = 0.01 AU after 173
revolutions. The case that separates the corrected rule from the frozen
one lands on the corrected rule on both sides. Every row that disagrees at
all is one where the mean motion enters, and each disagreement grows with
the mean anomaly accumulated. That traces to a single cause: Swiss
implementations carry k as a truncated decimal in degrees per day,
1.4 × 10⁻¹² relative off the protocol's value. This is a **known, quantified,
one-cause difference**, worth about 2 × 10⁻⁷″ on the Hamburg points (under
half an orbit a century). It is recorded rather than "fixed", because fixing
it on the Astrolog side would put that one Swiss consumer out of step with
every other. A cross-test that sees it should stop, not chase it.

**Now a gate.** The committed fixture arrived the same day, with its
generator and digest. It is vendored at
`third_party/ephproto/v4/elements/FIXTURE.tsv`, and
`ephproto4_kind4_elements_fixture` holds the engine to all eight rows. The
bound is derived from the known difference's cause, not tuned: 1.446 × 10⁻¹²
times the mean anomaly the row accumulates, with 20% headroom, over a
roundoff floor. Nudging the Earth mass ratio by 1.5 × 10⁻⁷ fails it by four
orders of magnitude.

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

## Runbook (draft, 2026-09-18)

The principles above say what counts as a pass. This says what is actually
run, by whom, in what order. It is a draft for both sides to amend: the
Astrolog side owns everything that drives its client or its daemon.

### Setup, recorded in the leg table's header

- **Versions.** Both servers' `datasetId`, the Prometheia commit and the
  Astrolog `ephv4` commit.
- **Files.** `prometheiad` reads `ephe/linux_p1550p2650.440` (DE440).
  `astrolog-ephd` always reads Swiss `.se1` files (the refit of DE441); it
  has **no JPL mode**. Launch it as `astrolog-ephd --bind 127.0.0.1 --port N
  --threads 1 --ephe "<tree>/ephem;<tree>"`. `--ephe` is its whole search
  path, and the tree root holds `sefstars.txt` and `seorbel.txt`: without it,
  stars and the hypothetical bodies do not compute.
- **Time.** Every numeric leg is a TT grid or instant list and sends its own
  ΔT, so no ΔT model enters.
- **Their client** runs a single-source chain, and every numeric leg asserts
  that the fallback notice is absent.

### Who drives which cell

| cell | driver | status |
|---|---|---|
| our client → both daemons | `tools/check/crosstest.py`: one request, sent to each daemon, diffed as angular separation, checked against the anchor where one exists, written to the leg table | **run**: legs 2, 6, 7, 9 (below) |
| their client → `prometheiad` | their harness, the live parity group pointed at `prometheiad` | Astrolog side |
| their client → their daemon | their gate | exists |
| both daemons, self-consistency | `tools/check/corrapplied.py` | **done**: 27/29 each |

### Legs, in order (each is cheap to stop at)

1. **Error contract first** (the cheapest leg: one object per case, and a
   disagreement about what an error *means* would otherwise surface
   misattributed inside a numeric leg). Both directions, each daemon against
   its written contract. **Run once, 2026-09-18**, with the Astrolog client
   against `prometheiad`. Every case matched except "Beta Sco", where the
   *documentation* was wrong: SERVER.md called it ambiguous, while STARS.md
   and the server resolve it to the brightest component.
2. **Surfaces.** Each side parses the other's WELCOME. The output is a table
   of advertised capabilities side by side (kinds, masks per observer,
   zodiacs, segments, limits). It is a record, not a verdict: it says which
   of the legs below can run against which daemon.
3. **Silent fallback.** An out-of-coverage instant and an uncovered object
   must come back as error 3 or 4, or with a truthful source string.
4. **Delivery equivalence.** The same question as one grid, in chunks, and
   as an instant list gives identical bytes. A 10,000-row grid is compared
   on its implied times.
5. **CANCEL and priority**, their client against `prometheiad`: ERROR 10
   before the answer is whole, nothing cached, interactive before prefetch.
6. **Both servers, the same question** (split from the anchor leg: agreement
   between the two and agreement with the sky are different claims, with
   different failure modes). Mask 0, ICRF, geocentric, with the ten corpus
   bodies at its eight epochs. With no JPL mode in `astrolog-ephd`, this is
   tier 2: DE440 against Swiss's `.se1` refit of DE441, within the refit's
   documented fidelity. A true tier-1 leg waits on a JPL mode, which is a
   feature for the maintainer to decide on.
7. **Each server against Horizons: astrometric ICRF at the corpus's own points.** Mask 1,
   equatorial ICRF, at exactly the corpus's instants, bodies and observers
   (geocentric, heliocentric, three topocentric sites). Each server is
   compared with Horizons as well as with the other. This server's own
   agreement with Horizons is 6 µas geocentric ([VALIDATION.md](VALIDATION.md)).
8. **Tier 2: apparent of date** for every observer kind, with the
   bisection ladder on any break.
9. **Hamburg points by name** (kind 3). `astrolog-ephd` serves kind 3: its
   token table is compiled in, and the elements come from `seorbel.txt` on
   `--ephe`.
   Expect about 10⁻⁷″: the same elements, differing only by k. Kronos
   carries the compiled-in-table trap (HYPOTHETICALS.md), so the
   `astrolog-ephd` leg records which table it read.
10. **Segments.** The Astrolog client has no SEGDATA decoder yet, so this
    leg waits on one. Until then, `prometheia-wire-client --segments` covers
    `prometheiad`'s side.

### First run, 2026-09-18

`docs/crosstest/2026-09-18.tsv`: 734 rows, legs 2, 6, 7, 8 and 9 plus the
heliocentric anchor. `prometheiad` on DE440 and `astrolog-ephd` on its Swiss
files, both on loopback, driven by `prometheia-wire-client`. Verdicts: 394
agree, 6 expected-difference, 205 findings, 40 findings attributed to the
Astrolog server by the anchor, 1 unadjudicated, 66 unanswered.

- **Each server against Horizons, geocentric** (astrometric ICRF, ten bodies
  at the corpus's epochs). `prometheiad`: 2–4 µas for the Sun and planets,
  5.8 mas for the Moon, reproducing VALIDATION.md through the wire.
  `astrolog-ephd`: 0.3–0.8 mas for the planets and 1.2 mas for the Moon, its
  refit's fidelity. So the geocentric gap between the servers is the refit.
- **Both servers, the same question** (mask 0): within 2 mas except the
  Moon, whose DE440-against-DE441 gap (21 mas at 1800) the anchor marks as
  expected.
- **Hamburg points by name**: within 0.62 mas at 1900, 2000 and 2100, and
  the distances to 10⁻¹² AU.
- **Apparent place from other observers** is where the findings are, each
  bisected to one term by varying the mask or the ΔT sent:
  - **Heliocentric light time**, attributed by the anchor. From the Sun's
    centre, astrometric, `prometheiad` is 1–6 µas from Horizons and
    `astrolog-ephd` 0.38″ (Mercury), 0.28″ (Venus) and 0.11″ (Mars).
    Geometric positions agree to 0.3 mas, and light time alone moves Venus
    24.208″ on one server and 24.452″ on the other: the retardation itself
    differs by 1%.
  - **Earth rotation ignored the ΔT a request sends.** Rows are asked at
    TT, so ΔT enters only through the observer's Earth rotation (UT1 =
    TT − ΔT). Changing the ΔT sent by 100 s moved `prometheiad`'s
    topocentric Moon by 16.9″ and `astrolog-ephd`'s by 0.000″: its
    topocentric rotation used Swiss's own ΔT. §3.5 makes the request's ΔT
    govern, and this was the whole of the 12″ topocentric Moon gap.
  - **Deflection advertised but not applied** for barycentric and
    planet-centred observers. Masks 0, 1 and 5 agree, and the gap appears
    only with the deflection bit: 3.2″ on Venus from the barycentre at
    J2000, 7 mas on Mars from Jupiter. `astrolog-ephd`'s WELCOME lists
    mask 7 for those observers, and its `corrApplied` claims deflection.
    `corrapplied.py` could not see it, because checking deflection alone
    needs a mask that server cannot express. Only a second engine could.
- **Smaller:** error 4 where 3 is meant, at the first instant of the Swiss
  files (1800-01-01, with light time); and Venus 2.03 mas from Horizons at
  its 2020 inferior conjunction, just over the 2-mas band. That is plausibly
  the refit seen from 0.29 AU, a question for the Astrolog side.
- **A harness bug this run caught in itself**: the first full run
  adjudicated every apparent-place gap against the *geocentric* anchor, and
  called 204 rows expected. That hid the ΔT and deflection findings. An
  anchor now judges only rows from its own observer.
- **Not yet run:** legs 3–5, which the Astrolog client can drive, and the
  topocentric anchor, which needs Horizons' UT1 recovered from its sidereal
  time first.

### Rerun, 2026-09-18 (Astrolog fixes cd92ffd and 8a7ba36)

Against an `astrolog-ephd` built at 10:01, which carries the ΔT and
error-code fixes but not e9d406e (WELCOME narrowing). Output kept in
`build/xtest/rerun.tsv`, not committed. Verdicts: 411 agree, 6
expected-difference, 188 findings, 40 findings attributed to the Astrolog
server, 1 unadjudicated, 66 unanswered.

- **Topocentric Earth rotation: fixed.** Worst topocentric Moon gap 12″ →
  0.129″, topocentric findings 25 → 8. The residue is not the sidereal
  time: see the next section.
- **Error codes: mostly fixed.** At 1800-01-01, outside `astrolog-ephd`'s
  coverage, 57 rows now answer 3. Nine still answer 4: every
  Jupiter-centred row at that instant, where the other observers answer 3.
- **Unchanged, as expected from this binary:** heliocentric light time (the
  40 attributed rows), and the deflection gap from the barycentre and from
  Jupiter.
- **The Astrolog side's position on those, not yet settled:**
  - heliocentric light time is Swiss's model, and their golden gate is
    bit-exact against Swiss. The anchor still puts `prometheiad` at µas and
    theirs at 0.38″ from Horizons; they asked for this side's definition.
  - WELCOME is narrowed, but their server still *accepts* masks it no
    longer advertises (heliocentric mask 7). `prometheiad` answers those
    with ERROR 11 as §3.5a requires, so a client that sends heliocentric
    mask 7 works against one server and fails against the other.
  - Jupiter-centred keeps its full advertisement. The 7 mas deflection gap
    on Mars remains unadjudicated: no anchor observes from Jupiter.

### Second rerun, 2026-09-18: three new anchors, and what they settled

Against the `astrolog-ephd` built at 10:16 (through e9d406e, the narrowed
WELCOME). Output in `build/xtest/rerun2.tsv`, not committed; a dated
record waits for a build of 4b1e375. Verdicts: 540 agree, 173
expected-difference, 27 findings, 16 findings attributed to the Astrolog
server, 49 unadjudicated. Nothing is left unanswered.

The harness gained:

- **The topocentric anchor** (`topo` leg). Horizons prints its local
  apparent sidereal time at the site; `build/prometheia-ut1` solves the
  ΔT that reproduces it under this library's GAST
  (`frames::ut1_from_sidereal_time`, shared with `tests/test_horizons.cpp`),
  and both servers are sent that ΔT. Fault-injected: a 1 s ΔT error turns
  12 of our rows into `finding (ours)`.
- **The barycentric anchor** (`bary` leg): the Sun from the barycentre,
  geometric, against one Horizons vectors request (`bary-sun` in
  `tools/fetch/horizons_fetch.py`), judged as a length.
- **Refusals** (in `surfaces`): every (observer, mask) a server does not
  list is sent, and anything but ERROR 11 is recorded.
- **Masks from WELCOME.** The apparent leg now sends the fullest mask
  both servers list for each observer, not a fixed one.

What they found:

- **`astrolog-ephd` places a topocentric observer about the mean pole.**
  With Horizons' own Earth rotation sent to both servers, `prometheiad`'s
  topocentric Moon is 8.3 mas from Horizons, and `astrolog-ephd`'s is
  0.165″. The geocentric Moon agrees to 1.6 mas at the same instants.
  Recovering the observer's offset from the two topocentric vectors gives
  100–290 m, horizontal (vertical under 2 m), in a direction that turns
  from epoch to epoch. Varying the ΔT sent does not remove it, so it is not
  the rotation angle. It is the nutation pole offset: rotating our site
  vector by (Δε, −Δψ sin ε) with a four-term nutation reproduces the
  measured offset to 1–3 m at all 15 rows, and the opposite sign misses by
  twice the offset. Their site vector is rotated by sidereal time but not
  taken through nutation. This is the whole of the topocentric Moon
  residue in the apparent leg too (0.02–0.13″).
- **The barycentric gap is the Sun's position, not a correction.** Masks 0
  and 1 give the same 0.23″ as mask 1 did. Against Horizons, ours is 3e-7
  km (DE440 and DE441 are the same fit over these epochs), and theirs
  0.04–1.05 km. That is inside the refit's fidelity (2 mas at 1 AU is 1.45
  km); it looks large from the barycentre only because the Sun is 0.005 AU
  away. Marked expected-difference by the anchor.
- **Deflection from the barycentre is no longer compared.**
  `astrolog-ephd` now lists only masks 0 and 1 there, and at the Sun's
  centre, so the harness sends mask 1.

Agreed with the Astrolog side, and recorded as expected-difference with
the reason in each row:

- **Heliocentric light time.** They keep Swiss's, because `astrolog-ephd`
  must serve the numbers the desktop library computes, and have reported
  it upstream with this side's definition. The harness marks a
  heliocentric row expected only where the geometric (mask 0) answers
  agree, so the difference is confined to light time.
- **Accepting unlisted masks.** Their server answers masks it does not
  list, on purpose: an orbit point from the Sun's centre honours bits that
  a body there cannot, and WELCOME's per-observer list cannot say so.
  Their client now narrows every request to a listed mask (fixed in their
  4b1e375), so an Astrolog cast works against `prometheiad`. The spec
  question stays open: §3.5a says ERROR 11, and a per-kind list would let
  both servers keep it.
- **Coverage.** A row one server refuses as outside its data's span
  (errCode 3) is a legitimate answer. The nine Jupiter-centred rows at
  1800 that answered 4 now answer 3.

Still open:

- **Deflection seen from Jupiter**, 7 mas on Mars, unadjudicated. Their
  account: Swiss deflects the target as seen from the Earth and then
  re-centres on Jupiter; ours deflects with the observer at Jupiter. No
  anchor observes from Jupiter.
- **Venus at its 2020 inferior conjunction**, 2.03 mas from Horizons, and
  Mercury from the barycentre in 2100, 2.01 mas from ours. Both are just
  over the 2 mas band, and plausibly the refit.

### The record, 2026-09-18 (`docs/crosstest/2026-09-18b.tsv`)

The Astrolog side confirmed the mean-pole site in the source. The site
routine can nutate, but every internal call site asks it not to, so the
site is built about the mean pole and used against a true-of-date
geocentric vector. The cause is upstream Swiss, not `astrolog-ephd`'s
wiring. They have reported it upstream, because it reaches every
topocentric chart the desktop application draws. The harness records it
as expected-difference only where the two servers' geocentric answers
agree at the same instant and the gap is no larger than the nutation
offset can make it: 0.2″ on the Moon, 1 mas elsewhere. Fault-injected:
with that band cut to 0.01″, 42 rows go back to findings.

Against the 10:16 build (its Jupiter coverage fix predates the 4b1e375
commit), 827 rows: 540 agree, 209 expected-difference, 5 findings, 2
findings attributed to the Astrolog server, 49 unadjudicated. What
remains:

- **Venus at its 2020 inferior conjunction** (six rows across legs,
  2.03–2.31 mas) and **Mercury from the barycentre in 2100** (2.01 mas):
  just over the 2 mas band, plausibly the refit, and a question for the
  Astrolog side.
- **Deflection seen from Jupiter** (48 rows) and the 1800 Moon, whose
  anchor row is outside `astrolog-ephd`'s coverage: unadjudicated.

The header now also records the daemon binary's build time, because a
commit can postdate the build that ran. `astrolog-ephd` moved to port
47391 for these runs: their harness binds 47291 itself, and on a shared
machine one harness ended up testing the other's daemon. `crosstest.py`
now refuses to run when both endpoints answer with the same server and
dataset.

### Third record, 2026-09-18 (`docs/crosstest/2026-09-18c.tsv`)

Two changes closed everything but one row.

- **Bands that follow the refit's actual error.** The Astrolog side
  measured Swiss's `.se1` against the DE file inside one process. The
  compression error is fixed in *position*: the same third of a kilometre
  on Mars at 0.37 AU and at 1.85 AU. So Swiss's documented "1 mas" is a
  typical figure at typical distances, not a bound. Our own sweep against
  Horizons gives worst values of 0.44–0.74 km for the Sun through Mars,
  1.9 km for Jupiter, 4.5–8.3 km beyond. `REFIT_KM` in `crosstest.py`
  holds those values plus about a third. The band is the larger of that
  length and the old 2 mas angle. The length widens it only at close
  range: from the barycentre the refit's errors are larger (about 21 km
  on Neptune), so the geocentric length must not tighten it. The Venus and
  Mercury edge rows are inside the band now: the compression, seen close up.
- **A deflection referee where Horizons cannot observe.** The `deflection`
  leg takes each server's mask-3 answer from Jupiter's centre. It compares
  that answer with the textbook solar deflection (USNO Circular 179, the
  NOVAS form, written in Python from the formula) applied to the same
  server's mask-1 answer. `prometheiad` agrees to 3 × 10⁻¹⁰″;
  `astrolog-ephd` is off by up to 0.54″, with Mars in 2075 bent 800 times
  more than the geometry allows. It also bends the Sun's own light (22 mas
  from Jupiter in 2075), which the Sun cannot do to itself. The Astrolog
  side's account was that Swiss deflects as if the observer were the
  Earth. Modelled that way (Earth-seen deflection as a displacement,
  re-centred on Jupiter), it does not reproduce their numbers either, so
  the cause is theirs to find. Fault-injected: a 1% change in the
  textbook's GM turns the largest of our rows red (0.65 mas against a
  0.2 mas band).

Verdicts (899 rows): 555 agree, 210 expected-difference, 111 findings
attributed to the Astrolog server (all deflection from a planet centre:
63 in the deflection leg, 48 apparent-place rows it adjudicates), 1
unadjudicated (the 1800 Moon, whose anchor is outside their coverage).

### Fourth record, 2026-09-18 (`docs/crosstest/2026-09-18d.tsv`)

The Astrolog side traced the planet-centred deflection in the source. Swiss
re-bases aberration on the centring body, but its deflection routine takes
no observer and uses the Earth's Sun geometry on a vector relative to
Jupiter. The result corresponds to no observer anywhere, which is why the
"seen from the Earth, re-centred" model could not reproduce it. It is
upstream Swiss. `astrolog-ephd` (050a5a4) no longer claims the term:
WELCOME lists 0/1/5 from a planet centre, and corrApplied says the same.

The deflection leg now judges each server that lists masks 1 and 3 from a
body centre, and records a server that does not as not advertising the
term. Our deflection is still refereed on every run, and a run in which
neither server lists the masks fails rather than skipping.

Verdicts (901 rows): 675 agree, 203 expected-difference (each row gives
its reason), no findings, 1 unadjudicated (the 1800 Moon, whose anchor is
outside `astrolog-ephd`'s coverage).

### After the per-kind drop, 2026-09-18 (`docs/crosstest/2026-09-18e.tsv`)

The refusal leg now covers every (observer, kind, mask) for bodies and
orbit points: a listed mask (0x0004 union 0x0014) must be served, and an
unlisted one must draw ERROR 11. `astrolog-ephd` now enforces what it
lists, and adds masks 3, 5 and 7 for orbit points from the Sun's centre
and the barycentre. All 160 checks agree. Fault-injected two ways:
treating every mask as listed gives 48 findings, and ignoring 0x0014
misjudges exactly the six orbit-point pairs it exists for. The
"permissive by decision" expected-difference is gone, because the
divergence it recorded is retired. The whole record has 1034 rows: 831
agree, 180 expected-difference, no findings, and 1 unadjudicated (the
1800 Moon).

### Orbit points and sidereal planes become standing legs, 2026-09-18

Today's orbit-point and sidereal findings came from ad-hoc probes, so both
checks are now legs of `crosstest.py`.

- **`points`:** 24 orbit points (the Moon's mean and osculating nodes and
  apsides, and those of Mercury, Mars, Jupiter and Saturn) at the corpus
  epochs, by direction and by distance, mask 0, true ecliptic of date.
  Bands per class, from the first measurement:
  - the Moon 1″ / 5 km (measured ≤ 0.49″, 1 km);
  - osculating planetary points 3″ / 1500 km (≤ 1.8″, 780 km; the refit's
    state errors reach the elements amplified);
  - the inner planets' and Mars's mean points 2″ / 500 km.
  Jupiter's and Saturn's mean points differ by up to 3300″ because mean
  elements are model-defined: ours are fitted to DE440, and Swiss's come
  from VSOP87. Its published manual says so, and says the two "are
  considerable" apart. Recorded as expected-difference with that citation. A second check
  holds the rule that a node lies on its frame's ecliptic: the Moon's mean
  node in the J2000 frame must have zero J2000 latitude.
- **`sidereal`:** the three A.8 planes for Fagan-Bradley and Lahiri, five
  bodies. Planes 0 and 1 are judged by what the sidereal rotation adds to
  the same body's tropical gap at that instant: 0.003″, the documented
  ayanamsa agreement. Plane 2 is unadjudicated while its latitudes agree
  (≤ 0.05″) and its longitude offset is one constant across bodies
  (spread ≤ 0.02″). That is a difference of origin, open in the protocol.

Fault-injected: tightening the Moon's point band, the plane-2 spread and
the sidereal extra gives 211 findings.

First results, all on the Astrolog side and sent:
- Their Moon node in the J2000 frame sits 0.006–10″ off the J2000
  ecliptic: they compute nodes on the ecliptic of date and convert.
- At 1800-01-01, the first instant of their files, their osculating
  points answer errCode 4, except the true descending node, which answers
  with the ascending node's distance (Δ 49,839 km). It looks like a
  fallback at the file edge.

### Fixed stars, 2026-09-18

The `stars` leg asks 29 named stars (the royal stars, the brightest
navigational stars, Algol, Polaris and the Pleiades' Alcyone) at 1900, 2000
and 2100, apparent, tropical and Lahiri. Both servers read
Hipparcos-derived catalogues with proper motion, and they agree to
0.008″. The band is 0.02″; a 0.0005″ band gives 134 findings.

α Centauri is handled separately, because the catalogues differ in what
its names mean:
- Our Rigil Kentaurus is α Cen A and our Toliman is α Cen B, as the IAU
  names them. They are 16.45″ apart at J2000.
- `astrolog-ephd` answers both names with one α Cen entry, which lies
  between A and B. Rigil Kentaurus is recorded as unadjudicated (a
  catalogue convention: the system against component A). Toliman is a
  finding on their side, since a client asking for B gets something else.

### The §3.5a sentences, and α Centauri, 2026-09-18 (evening)

- **The invariable plane's origin** (the `sidereal` leg's plane-2 rows):
  the Astrolog side measured its own plane 2 against its own plane 0 and
  found its zero point moves with the zodiac. A plane cannot know which
  zodiac was asked for. The proposed §3.5a sentence takes Prometheia's
  reading (the zero-point direction projected onto the plane), and the
  Astrolog side will change. When they do, those rows should agree, and the
  leg's plane-2 branch then judges like planes 0 and 1.
- **The node-frame rule** (the `points-frame` rows): the amendment puts a
  node on the mean ecliptic of date in every frame, and both maintainers
  approved it. Prometheia changed, and the check now compares the two
  servers' J2000-frame node of date. It found that `astrolog-ephd`'s
  J2000-frame node is not the node of date rotated. Its J2000 latitude is
  0.9″ at 1900, where the tilt between the ecliptics of 1900 and 2000 puts
  ours at 47″, and the servers differ by 6–51″ away from 2000. Ours is held
  to the rule by an engine test built from the public frame matrices.
- **α Centauri:** the Astrolog side's `sefstars.txt` now puts Toliman at
  α Cen B and Proxima Centauri at α Cen C (ephv4 `9e9e5c4`). Their file is
  deliberately no longer byte-identical to Swiss's. The `stars` leg's
  Toliman row should then agree; not yet re-run.

### The sidereal sweep, and two re-verifications, 2026-09-18 (`docs/crosstest/2026-09-18g.tsv`)

Against `astrolog-ephd` from Astrolog `qt` at `91e427e`: 2,226 rows, with
agree 1,809, expected-difference 313, finding 7, finding (theirs) 64,
unadjudicated 2 and unanswered 9.
- **`sidsweep`, a new leg.** Every A.11 zodiac token is asked on every A.8
  plane at 1950-07-01 and 2075-01-01, which are off every token's anchor
  epoch. It is graded per server, on what a plane request does:
  - a token WELCOME does not list must draw ERROR 11;
  - a listed token's planes 1 and 2 must each move the answer from plane 0,
    and a row bit-identical to plane 0 is a finding.

  That grading was suggested by the Astrolog side: a plane accepted and
  ignored is invisible to any comparison with a server that was never asked.
  - **`prometheiad`:** lists 3 of the 48 tokens (Fagan/Bradley, Lahiri and
    user). All three move on both planes, and the 45 unlisted tokens draw
    ERROR 11. All 288 rows agree.
  - **`astrolog-ephd`:** lists all 48. Sixteen star- and frame-anchored
    tokens answer plane 0 for planes 1 and 2 at both epochs, which is 64
    findings. They match the Astrolog side's own prediction and their
    registry §4.1 (fix decided, pending). The rows say so; they stay
    findings.
  - A caution for a later widening: at a token's own anchor epoch, plane 1
    *is* plane 0, and an identical row is then correct.
- **Plane 2, graded under Part A.** Both maintainers approved the projected
  zero point on 2026-09-18. `astrolog-ephd`'s constant offset (−31.51″
  Fagan/Bradley, −30.42″ Lahiri) is now expected-difference, pending their
  fix. It becomes `agree` when the offset closes.
- **The 7 findings** are the J2000-frame Moon node (`points-frame`). The
  Astrolog side confirmed it; their registry §4.2 records it with the fix
  pending.
- **Re-verified:**
  - Their `9bb48a5`: the osculating descending node at 1800 now answers
    errCode 3 (outside coverage) rather than a fallback.
  - Their `9e9e5c4`: Toliman now agrees, 0.04″ apart.
- **α Cen A (Rigil Kentaurus) still differs by 6.23″,** and it is a
  definition rather than a finding.
  - Ours is the Hipparcos α Cen A.
  - Theirs sits about 6″ from A toward B, roughly the pair's centre of mass.
  - FK5 538 cannot choose between them. At 2000 it is 6.8″ from ours and
    2.5″ from theirs, and ours runs to 28.6″ by 1900: the 80-year AB orbit
    against a straight line (STARS.md, "Known limits").

### Two Astrolog fixes verified, 2026-09-18 (`docs/crosstest/2026-09-18h.tsv`)

Against Astrolog `qt` at `b89f510`: agree 1,817, expected-difference 313,
finding (theirs) 64, unadjudicated 1 (the 1800 Moon) and unanswered 9.
- **The J2000-frame node** (their `b89f510`, registry §4.2): all 8
  `points-frame` rows agree. The 7 findings of record g are gone.
- **α Cen A** (their `554288b`): Rigil Kentaurus is 0.007″ from ours, where
  it was 6.23″.
  - Their old entry sat 38% of the way from A to B. The Astrolog side
    measured that it was neither A, nor the photocentre, nor the centre of
    mass: a ground-based centroid of the unresolved pair.
  - The `stars-alcen` rows now judge both names against `ALCEN_BAND`
    (0.1″, an estimate). The two sides read the pair from different
    catalogues.
- **Still open, theirs:** the 64 `sidsweep` rows and the plane-2 offset,
  both pending their registry §4.1. They will refuse the zodiacs whose zero
  point is defined at the instant (errCode 2) on planes 1 and 2, and the
  sweep grades such a refusal `refused`.

### Their §4.1, and the anchor's nutation, 2026-09-18 (`docs/crosstest/2026-09-18i.tsv`)

Against Astrolog `qt` at `4f9c2a1`, where planes 1 and 2 became their own
arithmetic:
- **`sidsweep`:** the record says all 288 of their rows agree. **That count
  is wrong** (see the correction under record j).
- **The fixed planes now disagree by a zodiac-dependent constant, identical
  on planes 1 and 2:** Fagan/Bradley 3.31″, Lahiri 16.78″. That is 140
  findings. Plane 1 had agreed to 0.003″ while Swiss computed it.
  - The constants are the nutation in longitude at each anchor epoch:
    −3.303″ at 1950-01-01 and +16.778″ at 1956-03-21.
  - Their zero point placed the true ayanamsa of t0 on the mean ecliptic of
    t0. On a mean frame it is the mean value (true less Δψ(t0)), which is
    how our anchors are held.
  - About 0.015″ is left on plane 2. That is the ~20 mas between their
    mean-J2000 construction and our ICRF one, which they predicted.
- The plane-2 branch now grades any offset as a finding, since their
  implementation has landed.

### No findings, 2026-09-18 (`docs/crosstest/2026-09-18j.tsv`)

Against Astrolog `qt` at `284b321`, their one-flag fix for record i's
anchor: agree 1,951, expected-difference 243, unadjudicated 1 (the 1800
Moon), unanswered 9, **no findings.**
- Plane 1 is back to ≤ 0.0013″.
- Plane 2's origin offset closed to −0.005 … +0.003″ for both zodiacs.
- **Correction, found by the Astrolog side:** the sweep's 288/288 for
  `astrolog-ephd` in records i and j is overstated.
  - Their server refuses the zodiacs defined at the instant (`true-citra`,
    `galcent-*` and others) on both fixed planes, with errCode 2.
  - A refused object's rows are NaN. The leg's "did it move" test took a
    NaN separation, which is neither equal nor small, as movement.
  - The harness now treats NaN rows as no answer. An explicit refusal
    grades `refused`, which is protocol-correct, not a finding.
  - The corrected count is in record k.
  - Records i and j are left as they were written. Their TSVs still grade
    those refused rows `agree`; this note is the correction.
- Their own gate now reproduces Swiss's anchor-ecliptic answer with their
  in-house arithmetic (54 comparisons, 0.01″). Without the flag, all 54 fail
  by Δψ(t0).
- The record's header marks their binary STALE: it was rebuilt after the
  daemon started, most likely by their `make check`. The numbers show the
  fix in effect.

### The sweep corrected, 2026-09-18 (`docs/crosstest/2026-09-18k.tsv`)

Against Astrolog `qt` at `114f2a5`, marked `+dirty` by the harness because
their tree had uncommitted changes at build time. The sweep leg treats a
refusal's NaN rows as no answer:
agree 1,903, expected-difference 243, **refused 48**, unadjudicated 1,
unanswered 9, **no findings**.
- **`astrolog-ephd` refuses twelve tokens on both fixed planes** (errCode
  2): the eleven zodiacs defined at the instant, and `true-sheoran`. Their
  other 36 tokens move on both planes.
- Sheoran's zodiac is anchored at an epoch per its published definition
  (−60° at the winter solstice of 4174 BCE), so its refusal is their
  choice, not the proposed §3.5a clause. The protocol allows it.
- `prometheiad` lists three tokens here (it did not yet list the new ones):
  288/288.

### Zodiacs defined at the instant, 2026-09-18 (`docs/crosstest/2026-09-18l.tsv`)

`prometheiad` now lists the eleven zodiacs defined at the instant
([FRAMES.md](FRAMES.md)).

The Astrolog daemon predates their `9df1a4e` binary, and the header says
STALE. That commit only changes when they refuse on the fixed planes, which
this record does not depend on.

Totals: agree 1,911, expected-difference 243, refused 70, unadjudicated 1,
unanswered 9, and 135 findings (theirs), all in the new leg:
- **`sidinstant`, plane 0.** Their anchor is the apparent position, so every
  row differs by the anchor's aberration. The published definition takes
  the anchor's true position; the Astrolog side agrees and will adopt it as
  a third §3.5a sentence.
  - Spica −4.4 … −5.3″ and ζ Psc +3.0 … +3.8″, which are the stars'
    aberration they measured independently.
  - δ Cnc +18.1 … +18.6″ and λ Sco −20.6 … −20.8″.
  - The Galactic Centre −20.4″ for every mode anchored on it.
- **`galequ-iau1958` −0.18″:** a different ICRS realisation of the IAU 1958
  pole. Ours matches both Liu et al.'s transfer and the Hipparcos
  catalogue's (ERFA) to 12 mas, and the Astrolog side accepts ours.
- **`galequ-true` and `galequ-mula`** agree to 4 mas: the anchor is a pole,
  which has no aberration.
- **`sidsweep`, ours:** 14 tokens, 266 rows agree, and 22 refused. The
  refusals are the eleven on plane 1 at both epochs, as the clause says.

### The true anchor, 2026-09-18 (`docs/crosstest/2026-09-18m.tsv`)

Against the Astrolog daemon from their clean `b0b6a19`, which includes the
true anchor (`789f3c2`).
- The header reads `b0b6a19+dirty` and STALE. The harness names the tree and
  binary as they are now, and they had rebuilt from a working tree after
  launching. Their launch script printed the running daemon's commit
  (`b0b6a19`, clean).

Totals: agree 2,031, expected-difference 243, refused 70, unadjudicated 1,
unanswered 9, and 15 findings (theirs).
- **`sidinstant`:** the aberration pattern of record l is gone.
  - The star modes agree to 0.002–0.023″.
  - The Galactic-Centre modes agree to −0.064 … +0.022″, drifting with the
    epoch.
  - The modern-pole node modes agree to 4 mas.
  - What remains is the anchors' data: the stars from two catalogues, and
    Sgr A*'s place and motion from different sources. The leg allows it with
    `INSTANT_ANCHOR_BAND`, 0.1″ (an estimate).
- **The 15 findings** are `galequ-iau1958` at −0.18″, the ICRS transfer of
  the IAU 1958 pole. Adopting Liu et al.'s transfer is before their
  maintainer.
- **Wilhelm's zodiac** agrees with the rest since ours moved to the mean pole
  of date (FRAMES.md).

### Binary orbits, and the clause on both sides, 2026-09-18 (`docs/crosstest/2026-09-18n.tsv`)

Against the Astrolog daemon from their clean `9fbdc21`. The header reads
`+dirty` because their working tree had changed since; the harness names the
tree, not the running binary.

Totals: agree 2,041, expected-difference 257, refused 46, unadjudicated 1,
unanswered 9, and 15 findings (theirs).
- **The §3.5a clause is on both sides** (their `031f3c9`). Each server
  refuses plane 1 for the zodiacs defined at the instant and answers plane
  2 from the instant's zero point: 24 refusals theirs, 22 ours.
- **Binary stars** ([STARS.md](STARS.md), "Binary stars"). Sirius, Procyon,
  and α Cen A and B now carry their orbits here. Their server keeps the
  straight line.
  - The `stars` and `stars-alcen` rows for these four are
    expected-difference only when the servers differ by exactly our bend,
    computed independently by `binary_orbits.py`. They do, to 3 mas.
  - α Cen B − A is the orbit's 14.11″ here, and 16.47″ on theirs.
- **The 15 findings** are `galequ-iau1958`'s pole transfer, which is with
  their maintainer.

### Rates, a new leg, 2026-09-18 (`docs/crosstest/2026-09-18p.tsv`)

Every leg before this one grades positions. Each row carries six numbers,
but no leg ever looked at the three rates. The retrograde flag depends on
the sign of one, so this was a blind spot, not a detail.

- **`rates`, a different oracle.** Each server's reported rates are checked
  against a five-point central difference of its own positions, one request
  of five rows at t − 2h … t + 2h. Grading each server against itself sees
  a wrong sign, unit or frame in either one, with nothing to agree on.
  - **The grid:** 8 objects (Sun, Moon, Mercury, Mars, Jupiter, Pluto, the
    Moon's mean node, Sirius), 8 configurations (geocentric apparent in
    three frames, astrometric ICRF, heliocentric, topocentric Zurich,
    `lahiri`, `true-citra`), 2 epochs.
  - **Bands:** 1e-6 °/day, and 6e-6 °/day topocentric (an estimate, twice
    the measured 2.9e-6). Distance rates: 1e-9 AU/day per AU of distance.
- **Two traps in the method,** both measured.
  - **The step must be exact in binary:** h = 1/1024 day. At h = 0.001
    day, the 40 µs quantization of a JD double alone showed as 7e-7 °/day
    on the Moon.
  - **Three points are not enough topocentrically.** The Moon's diurnal
    parallax makes the three-point truncation about 4e-5 °/day. Five points
    bring it to about 3e-10.
- **It found two defects of ours, both fixed** (ENGINE.md, "Rates";
  ORBIT-POINTS.md):
  - The engine's own rate step, 0.001 day, is also a three-point difference.
    Its truncation was 2.6e-5 °/day on a topocentric Moon. At 1/4096 day it
    is 2.9e-6.
  - The Moon's points jittered by 1.2 mas topocentrically: their retarded
    epoch lacked the rounding recovery bodies have. Now 8 µas.
- **Ours now:** all 126 rows pass. Worst 1.6e-6 °/day (the topocentric mean
  node), 7e-7 geocentric, distance 6.5e-11 AU/day.
- **Theirs:** 95 of 126 rows miss (reported to the Astrolog session as
  numbers).
  - Topocentric Moon: up to 8.2e-4 °/day.
  - `true-citra`: every body 5e-5 to 1e-4 °/day in longitude, where
    `lahiri` is within 1e-6. The instant-defined zodiac's own motion looks
    to be missing from the rates.
  - Distance rates up to 1.8e-6 AU/day; longitude and latitude rates
    generally 1e-6 to 1e-5 °/day.
- **Heliocentric runs at mask 1.** `astrolog-ephd` refuses (ERROR 11) the
  default heliocentric light time and aberration for some object kinds, and
  one refusal fails the whole request.
- **The other legs are unchanged from record o.**

### Range, graded against Horizons, 2026-09-18 (`docs/crosstest/2026-09-18q.tsv`)

The same audit's second gap. The `horizons` legs compared directions only,
so a distance error was invisible to every leg except `points` and `bary`.
Horizons' `delta`, the light-time range, is what mask 1 answers, and it was
already in the corpus.

- **Ours:** 1.1 m geocentric at worst (the outer planets), 0.25 m
  heliocentric. Graded at 5 m, an estimate at 4× that. With the band set to
  1 mm, 68 rows turn into findings, so the check does fire.
- **Theirs:** 1–22 km (Mercury heliocentric 22 km, Neptune 15.6 km, the
  Sun 1 km, the Moon 3 m). Recorded in each row's note, not graded:
  nothing published gives a band for their compressed files.
- **Verdicts otherwise identical to record p.**

### What it leaves behind

The leg table, committed as a dated record under `docs/crosstest/`. Every
`finding` row becomes a test in whichever repository was wrong, citing the
row. Nothing here enters `tools/gate.sh`, because it needs two daemons.

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
  input. Since 2026-09-18 that is `tools/fuzz.sh`'s job (SERVER.md,
  "Fuzzing"): libFuzzer over the codec and over the session. Its first
  10-minute runs gave 2.36 million session inputs with no failure, and one
  codec finding: DATA's non-finite values. It runs by hand, not in the gate,
  and it is single-connection, so it says nothing about the next item.
- **Load and duration.** Not in these legs, which are single-client and
  short-session. Since 2026-09-18, `prometheia-load` covers `prometheiad`
  (SERVER.md, "Load and soak"): 64 connections for 5 minutes at about 35,600
  requests a second, with memory flat and nothing leaked, and the
  per-address caps holding. It has not been pointed at `astrolog-ephd`.
- **Kinds 3 and 4 against the Astrolog client.** This server now serves
  named hypotheticals and bodies from elements; the Astrolog client does not
  request them from any server, because its user's own element file defines
  those bodies and a local Kepler solution is exact. So there is no
  diagonal cell for them. Kind 4 is tested another way, below, and more
  strongly than anything else here.

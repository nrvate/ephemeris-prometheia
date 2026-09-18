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
  input. Astrolog runs a `ROBUST` gate; this repository has five hand-written
  malformed frames in `tests/test_server.cpp` and no fuzz corpus, so our wire
  parser has never seen a hostile byte. That gap is ours and predates this
  plan.
- **Load and duration.** Single-client, short-session. No soak, no concurrent
  clients, no memory-growth measurement.
- **Kinds 3 and 4 against the Astrolog client.** This server now serves
  named hypotheticals and bodies from elements; the Astrolog client does not
  request them from any server, because its user's own element file defines
  those bodies and a local Kepler solution is exact. So there is no
  diagonal cell for them. Kind 4 is tested another way, below, and more
  strongly than anything else here.

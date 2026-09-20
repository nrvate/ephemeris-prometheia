#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Falsify crosstest.py's adjudicators against hand-built tables.

The cross-test's three adjudicators decide what a disagreement *means*: a
row where the two servers differ is not yet a finding, and these turn it
into `expected-difference`, `unadjudicated`, or leave it a `finding`. They
are the code that speaks to the other project. They are also the code that
got it wrong on 2026-09-20, when a verdict fell through a branch because
evidence was missing rather than contradicting, and six rows accused a
server that was right.

Nothing graded them. Every leg of the cross-test needs two daemons and an
ephemeris, so every falsification of one was done by hand, once, and written
up in docs/CROSS-TEST.md. These three are different: they are pure functions
of the results table. They can be driven from synthetic rows with no client,
no server and no data, which means this script is **in tools/gate.sh** and
re-runs on every commit -- the first falsification in this repo that does.

Each case builds a table, runs one adjudicator, and asserts the verdict of
**every** row, not just the interesting one: an adjudicator that upgrades the
row it was asked about while also touching its anchor is wrong in a way that
checking one row cannot see. Half the cases are negative -- the evidence is
absent, or contradicts, or belongs to another observer -- because those are
the branches that turn a right server into a finding.

Coverage is checked structurally rather than claimed. The script traces the
three functions while the cases run and fails if any executable line was
never reached, so a branch added to crosstest.py without a case here fails
the gate instead of being discovered by a peer. (`loadselftest.py` shipped
the day before declaring nine assertions and exercising seven; a docstring
promising full coverage is a promise nothing keeps.)

    tools/check/adjudicatetest.py
"""
import os
import sys
import trace

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import crosstest  # noqa: E402

ADJUDICATORS = {
    "helio-light-time": crosstest.adjudicate_helio_light_time,
    "coverage": crosstest.adjudicate_coverage,
    "same": crosstest.adjudicate_same,
}


def row(leg, verdict, obj=5, epoch=2451545.0, observer="geo", mask=0, note="", sep=0.0):
    return dict(leg=leg, epoch_tt=epoch, object=obj, observer=observer, mask=mask,
                verdict=verdict, note=note, sep_servers=sep)


def table_of(rows):
    t = crosstest.Table()
    for r in rows:
        t.add(**r)
    return t


# name, adjudicator, rows, expected verdict per row, a substring the first
# row's note must gain ("" for "the note must not change").
CASES = [
    # ---- adjudicate_helio_light_time ----------------------------------
    ("a helio apparent gap whose geometry agrees", "helio-light-time",
     [row("apparent", "finding", observer="helio", mask=1),
      row("same-helio", "agree")],
     ["expected-difference", "agree"], "light time differs"),
    ("the same, reached from the Horizons leg", "helio-light-time",
     [row("horizons-helio", "finding (theirs)"),
      row("same-helio", "agree")],
     ["expected-difference", "agree"], "light time differs"),
    ("the geometry disagrees too, so it stays a finding", "helio-light-time",
     [row("apparent", "finding", observer="helio", mask=1),
      row("same-helio", "finding")],
     ["finding", "finding"], ""),
    ("no geometric row to consult", "helio-light-time",
     [row("apparent", "finding", observer="helio", mask=1)],
     ["finding"], ""),
    ("a geocentric row is not this rule's business", "helio-light-time",
     [row("apparent", "finding", observer="geo", mask=1),
      row("same-helio", "agree")],
     ["finding", "agree"], ""),

    # ---- adjudicate_coverage ------------------------------------------
    ("one server is outside its data's span", "coverage",
     [row("same", "unanswered", note="errCode ours 0 theirs 3")],
     ["expected-difference"], "outside one server's coverage"),
    ("the refusal is not a coverage refusal", "coverage",
     [row("same", "unanswered", note="errCode ours 0 theirs 6")],
     ["unanswered"], ""),
    ("both servers refused for coverage: still unanswered", "coverage",
     [row("same", "unanswered", note="errCode ours 3 theirs 3")],
     ["unanswered"], ""),
    ("an answered row carrying an errCode note", "coverage",
     [row("same", "finding", note="errCode ours 0 theirs 3")],
     ["finding"], ""),
    ("an unanswered row whose note is something else", "coverage",
     [row("same", "unanswered", note="no reply within the timeout")],
     ["unanswered"], ""),

    # ---- adjudicate_same ----------------------------------------------
    ("the servers differ and the anchor holds them both", "same",
     [row("same", "finding"), row("horizons", "agree")],
     ["expected-difference", "agree"], "each server inside its own Horizons band"),
    ("the anchor is itself a finding", "same",
     [row("same", "finding"), row("horizons", "finding (theirs)")],
     ["finding", "finding (theirs)"], "anchor verdict: finding (theirs)"),
    ("the anchor leg was not answered here", "same",
     [row("same", "finding"), row("horizons", "unanswered")],
     ["unadjudicated", "unanswered"], "the anchor leg was not answered here"),
    ("there is no anchor row at all", "same",
     [row("same", "finding")],
     ["unadjudicated"], "the anchor leg was not answered here"),
    ("a heliocentric row takes the heliocentric anchor", "same",
     [row("same-helio", "finding"), row("horizons-helio", "agree")],
     ["expected-difference", "agree"], "each server inside its own Horizons band"),
    ("the geocentric anchor does not adjudicate a heliocentric row", "same",
     [row("same-helio", "finding"), row("horizons", "agree")],
     ["unadjudicated", "agree"], "the anchor leg was not answered here"),
    ("from Jupiter, the textbook referee agrees", "same",
     [row("apparent", "finding", observer="jupiter"), row("deflection", "agree")],
     ["unadjudicated", "agree"], "deflection against the textbook: agree"),
    ("from Jupiter, the textbook referee does not", "same",
     [row("apparent", "finding", observer="jupiter"), row("deflection", "finding (ours)")],
     ["finding (ours)", "finding (ours)"], "deflection against the textbook"),
    ("from Jupiter, with no deflection row", "same",
     [row("apparent", "finding", observer="jupiter")],
     ["unadjudicated"], "no deflection anchor at this row"),
    ("from Jupiter, the deflection row went unanswered", "same",
     [row("apparent", "finding", observer="jupiter"), row("deflection", "unanswered")],
     ["unadjudicated", "unanswered"], "no deflection anchor at this row"),
    ("the Sun from the barycentre, anchored in km", "same",
     [row("apparent", "finding", obj=10, observer="bary"), row("horizons-bary", "agree", obj=10)],
     ["expected-difference", "agree"], "the .se1 refit, seen close up"),
    ("the Sun from the barycentre, anchor disagreeing", "same",
     [row("apparent", "finding", obj=10, observer="bary"),
      row("horizons-bary", "finding", obj=10)],
     ["finding", "finding"], "no anchor from this observer"),
    ("a topocentric gap inside the mean-pole band", "same",
     [row("apparent", "finding", obj=301, observer="topo", sep=0.1),
      row("apparent", "agree", obj=301, observer="geo")],
     ["expected-difference", "agree"], crosstest.MEAN_POLE_NOTE[:24]),
    ("a topocentric gap too large for the mean pole", "same",
     [row("apparent", "finding", obj=301, observer="topo", sep=9.0),
      row("apparent", "agree", obj=301, observer="geo")],
     ["finding", "agree"], "no anchor from this observer"),
    # Row 1 is adjudicated too, and reaches no anchor of its own.
    ("a topocentric gap whose geocentric row is a finding", "same",
     [row("apparent", "finding", obj=301, observer="topo", sep=0.1),
      row("apparent", "finding", obj=301, observer="geo")],
     ["finding", "unadjudicated"], "no anchor from this observer"),
    # ...and the order matters, which is worth pinning rather than
    # discovering. The real table adds geocentric rows before topocentric
    # ones, so by the time a topo row consults its geo row that row has
    # already been adjudicated -- and the branch admits "expected-difference"
    # deliberately, so a geo row the anchor excused carries the topo row
    # with it. Reverse the two rows here and the topo row stays a finding.
    ("a geocentric row excused before the topocentric row reads it", "same",
     [row("apparent", "finding", obj=301, observer="geo"),
      row("horizons", "agree", obj=301),
      row("apparent", "finding", obj=301, observer="topo", sep=0.1)],
     ["expected-difference", "agree", "expected-difference"],
     "each server inside its own Horizons band"),
    ("a topocentric planet is held to the tighter band", "same",
     [row("apparent", "finding", obj=5, observer="topo", sep=0.1),
      row("apparent", "agree", obj=5, observer="geo")],
     ["finding", "agree"], "no anchor from this observer"),
    ("an agreeing row is never adjudicated", "same",
     [row("same", "agree"), row("horizons", "finding")],
     ["agree", "finding"], ""),
    ("a leg the adjudicator does not own", "same",
     [row("rates", "finding"), row("horizons", "agree")],
     ["finding", "agree"], ""),
]


def run_case(case):
    """Return a list of complaints; empty means the case passed."""
    name, which, rows, want_verdicts, want_note = case
    before = [r.get("note", "") for r in rows]
    t = table_of(rows)
    ADJUDICATORS[which](t)
    bad = []
    for i, (got, want) in enumerate(zip(t.rows, want_verdicts)):
        if got["verdict"] != want:
            bad.append(f"row {i}: verdict {got['verdict']!r}, wanted {want!r}")
    if want_note:
        if want_note not in t.rows[0]["note"]:
            bad.append(f"row 0's note does not say {want_note!r}: {t.rows[0]['note']!r}")
    elif t.rows[0]["note"] != before[0]:
        bad.append(f"row 0's note changed to {t.rows[0]['note']!r} and should not have")
    return bad


def run_all():
    failed = []
    for case in CASES:
        bad = run_case(case)
        print(f"{'ok' if not bad else 'FAILED':6}  {case[0]}")
        for line in bad:
            print(f"          {line}")
        if bad:
            failed.append(case[0])
    return failed


def executable_lines():
    want = set()
    for f in ADJUDICATORS.values():
        want |= {ln for _, _, ln in f.__code__.co_lines() if ln is not None}
    return want


def main():
    want = executable_lines()
    tracer = trace.Trace(count=1, trace=0)
    failed = tracer.runfunc(run_all)
    hit = {ln for (fn, ln), n in tracer.results().counts.items()
           if os.path.abspath(fn) == os.path.abspath(crosstest.__file__) and n}
    missing = sorted(want - hit)

    print()
    if missing:
        # Not a case failing: a branch of an adjudicator that no case here
        # reaches, so it could be deleted or inverted and every case above
        # would still pass.
        print("no case reaches crosstest.py lines: " + ", ".join(str(x) for x in missing))
        for ln in missing:
            print(f"    {ln}: " + crosstest_line(ln))
    if failed:
        print(f"{len(failed)} of {len(CASES)} cases FAILED: " + ", ".join(failed))
    if failed or missing:
        return 1
    print(f"all {len(CASES)} cases pass, and every line of the {len(ADJUDICATORS)} "
          f"adjudicators is reached")
    return 0


def crosstest_line(n):
    with open(crosstest.__file__) as f:
        for i, line in enumerate(f, 1):
            if i == n:
                return line.rstrip()
    return "?"


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Falsify every assertion corrapplied.py makes, and require the right one to fail.

corrapplied.py is the tool that accuses other people's servers, and until now
nothing graded it.  Its falsifications existed as prose -- docs/CROSS-TEST.md
says it "had two bugs that only a deliberately broken server revealed", and
nothing re-ran that broken server.  Both of its real defects were in the
*reading* rather than the arithmetic: a unit confusion, and reading A.3
0x0004 while ignoring 0x0014, which produced two FAILs against a server doing
exactly what it advertised.  This is the structural version.

Each case breaks one thing and names the assertion that must catch it.  A
case passes only when **that** assertion fires and no other one does: a
selftest asking only "did it go red" passes a gate whose check has been
deleted, because a neighbouring assertion reds on the same input.

**There is no list of assertions in this file.**  `corrapplied.py
--list-assertions` is the table and every run ends with `assertions evaluated
[...] fired [...]`, so a check added there and nowhere else stops this script
instead of passing unseen.  Three refusals come out of that: an assertion the
tool declares that no case fires; a case expecting one the tool does not
declare; and an assertion no case ever *reaches*, which is the strongest,
because a case can name an assertion and never arrive at the code evaluating
it.

The fault is injected at the client, not the server: corrapplied.py reaches a
server only through prometheia-wire-client and sees only what wirelib.py
parses from its stdout, so tools/check/fakewire.py answers from a scenario
file.  No daemon, no ephemeris, no socket, and a server that lies in exactly
one way at a time -- which is what makes "this assertion and no other" a
meaningful claim.

**What this cannot see**, stated because a check that does not say so invites
belief it has not earned: fakewire.py reproduces the client's output format
rather than speaking v4, so nothing here grades wirelib.py against a real
client, and a drift in the client's printing would have to be mirrored here
by hand.  The compensating property is the direction it rots: wirelib's
regexes stop matching, every case loses its answers, and the run fails on
`nothing-checked` rather than quietly passing.  The wire itself is covered
where it belongs, by the conformance fixtures and the cross-test.
"""

import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
REPORT = re.compile(r"^assertions evaluated \[([^\]]*)\] fired \[([^\]]*)\]", re.M)

LIGHT_TIME, DEFLECTION, ABERRATION = 1, 2, 4
ALL_OBSERVERS = 31  # the five observer bits of A.3 0x0004


def parse_report(text):
    """(evaluated, fired) as name sets, or (None, None) if the run printed none."""
    m = REPORT.search(text)
    if not m:
        return None, None
    return set(m.group(1).split()), set(m.group(2).split())


def all_assertions(tool):
    """The assertion table, from the tool rather than from a copy beside it."""
    out = subprocess.run([sys.executable, tool, "--list-assertions"],
                         capture_output=True, text=True, timeout=60)
    if out.returncode != 0:
        raise SystemExit(f"{tool} --list-assertions failed: {out.stderr.strip()}")
    names = out.stdout.split()
    if not names:
        raise SystemExit(f"{tool} --list-assertions printed nothing")
    return names


def run_case(tool, fake, scenario, extra, tmp):
    """corrapplied.py against a scripted server. Returns (exit code, output)."""
    path = os.path.join(tmp, "scenario.json")
    with open(path, "w") as f:
        json.dump(scenario, f)
    env = dict(os.environ, FAKEWIRE_SCENARIO=path)
    out = subprocess.run([sys.executable, tool, "--client", fake] + extra,
                         capture_output=True, text=True, timeout=600, env=env)
    return out.returncode, out.stdout + out.stderr


# Every mask corrapplied.py ever asks for, declared for every observer: the
# control, and the base every other scenario narrows.
EVERY_MASK = [[ALL_OBSERVERS, m] for m in (0, LIGHT_TIME, DEFLECTION, ABERRATION, 7)]


def cases():
    """name, scenario, extra argv, the assertions that must fire, exit code."""
    return [
        # A server that is right about itself: every assertion is evaluated
        # and none fires. Without this case the table below proves only that
        # broken servers go red, not that correct ones stay green.
        ("a server that tells the truth",
         {"corrmasks": EVERY_MASK, "mode": "honest"}, [], set(), 0),

        # corrApplied grows when the request asks for more. Still a subset of
        # what WELCOME declares and still correct at mask 0, so `declared`
        # and `truthful` cannot see it -- this is the echo bug this
        # repository shipped until the v4 rewrite, and it has exactly one
        # witness.
        ("corrApplied echoes the request",
         {"corrmasks": EVERY_MASK, "unavailable": ABERRATION, "mode": "varies"},
         [], {"independence"}, 1),

        # The server claims a correction its own WELCOME never advertises for
        # this observer and kind. Constant across masks, so `independence`
        # stays quiet; the claimed bit is never asked for alone, so
        # `truthful` never sees it.
        ("corrApplied claims what WELCOME does not",
         {"corrmasks": [[ALL_OBSERVERS, 0], [ALL_OBSERVERS, LIGHT_TIME]],
          "mode": "overclaim", "claim_extra": DEFLECTION},
         [], {"declared"}, 1),

        # The bit is clear, and asking for it alone moves the sky twenty
        # arcseconds. A false denial: structurally available, reported absent.
        ("a clear bit that moves the sky",
         {"corrmasks": EVERY_MASK, "mode": "false-denial", "deny": LIGHT_TIME},
         [], {"truthful"}, 1),

        # A.3 0x0004 empty. Nothing can be asked, and the tool must say so
        # rather than checking nothing and reporting a pass.
        ("WELCOME advertises no correction masks",
         {"corrmasks": [], "mode": "honest"}, [], {"welcome-no-corrmasks"}, 1),

        # Every request draws an error, so no case survives to be checked.
        # Silence is not a pass. `min-fraction` is deliberately not evaluated
        # here: it would fire on the same input for a second reason, and a
        # condition counted twice is what lets a deleted check stay invisible.
        ("the server answers nothing",
         {"corrmasks": EVERY_MASK, "mode": "honest", "answer": []},
         [], {"nothing-checked"}, 1),

        # Six of twenty-nine cases answered: enough to look like a run, far
        # too few to be one.
        ("the server answers only one observer",
         {"corrmasks": EVERY_MASK, "mode": "honest", "answer": ["geo"]},
         [], {"min-fraction"}, 1),

        # Masks 0 and 7 honoured and no single correction: `independence` and
        # `declared` both run, `truthful` needs a bit asked for alone and
        # never gets one. A check that never ran is not a check that passed.
        ("no correction can be asked for alone",
         {"corrmasks": [[ALL_OBSERVERS, 0], [ALL_OBSERVERS, 7]], "mode": "honest"},
         [], {"check-never-ran"}, 1),
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", default=os.path.join(ROOT, "tools/check/corrapplied.py"))
    ap.add_argument("--fake", default=os.path.join(ROOT, "tools/check/fakewire.py"))
    ap.add_argument("-v", "--verbose", action="store_true", help="echo each run's output")
    args = ap.parse_args()

    table = cases()

    # Two cases that red identically are one case, and the table does not say
    # so: delete either and everything still passes.
    same = {}
    for name, _, _, expect, want_exit in table:
        key = (frozenset(expect), want_exit)
        if key in same:
            raise SystemExit(f"'{name}' and '{same[key]}' assert the same thing "
                             f"({sorted(expect) or 'nothing'}, exit {want_exit}): "
                             "either is redundant, and deleting it would go unnoticed")
        same[key] = name

    declared = all_assertions(args.tool)
    covered = {a for c in table for a in c[3]}
    unexercised = [a for a in declared if a not in covered]
    if unexercised:
        raise SystemExit("corrapplied.py declares these and no case fires them: "
                         + ", ".join(unexercised))
    stray = sorted(covered - set(declared))
    if stray:
        raise SystemExit("cases expect assertions the tool does not declare: "
                         + ", ".join(stray))

    tmp = os.environ.get("CLAUDE_JOB_DIR")
    tmp = os.path.join(tmp, "tmp") if tmp else os.path.join(ROOT, "build")
    os.makedirs(tmp, exist_ok=True)

    failures, evaluated = [], set()
    for name, scenario, extra, expect, want_exit in table:
        code, out = run_case(args.tool, args.fake, scenario, extra, tmp)
        if args.verbose:
            print(out)
        seen, got = parse_report(out)
        note = []
        if got is None:
            note.append("the run printed no assertion report")
            got = set()
        else:
            evaluated |= seen
        if code != want_exit:
            note.append(f"exit {code}, wanted {want_exit}")
        if got != expect:
            missing = sorted(expect - got)
            fired = sorted(got - expect)
            if missing:
                note.append("did not fire: " + ", ".join(missing))
            if fired:
                note.append("fired but should not have: " + ", ".join(fired))
        print(f"{'ok' if not note else 'FAILED':6}  {name}")
        for line in note:
            print(f"          {line}")
        if note:
            failures.append(name)

    print()
    never = [a for a in declared if a not in evaluated]
    if never:
        print("no case reaches: " + ", ".join(never))
    if failures:
        print(f"{len(failures)} of {len(table)} cases FAILED: " + ", ".join(failures))
    if failures or never:
        return 1
    print(f"all {len(table)} cases: each of the {len(declared)} assertions corrapplied.py "
          f"declares is reached, and fires on its own fault and no other")
    return 0


if __name__ == "__main__":
    sys.exit(main())

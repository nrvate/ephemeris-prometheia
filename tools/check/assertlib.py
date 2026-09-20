#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""One register of named assertions, shared by the checkers that accuse.

Several tools here decide whether another project's server is wrong, and a
checker's own failure mode is not "it went red when it should have been
green" -- it is a *green that could not have been red*: a check that never
reached its comparison, or one whose exit status was some neighbouring
condition and not the check at all.  Both have shipped in this repository.

So every assertion is named, every run says which names it evaluated and
which fired, and the exit status is exactly "did any fire" and counts
nothing else.  Three properties come out of that, and they are the reason
this is a register rather than a pile of `if` statements:

  * A deleted check reports both "did not fire" and the wrong exit status,
    so it cannot hide behind another exit condition.
  * `evaluated` is not `fired`, and neither is inferable from the prose a
    run prints, so a selftest can require that an assertion was *reached*.
  * The name list is the tool's own (`--list-assertions`), so a selftest
    reads the table from the thing it grades rather than from a copy beside
    it.  A copy rots toward green; this project has shipped that twice.

Usage:

    ASSERTIONS = ("nothing-checked", "exceeds-bound")     # in the tool
    asserts = assertlib.Assertions(ASSERTIONS)
    if not asserts.judge("nothing-checked", checked == 0, msg, into=fails):
        asserts.judge("exceeds-bound", bool(over), msg, into=fails)
    asserts.report()
    return 1 if asserts.any_fired() else 0

`judge` returns the verdict so the caller can nest, which is how a condition
is kept from being counted twice: two assertions that fire on the same input
mean deleting either one changes nothing anybody sees.
"""

import os
import re
import subprocess
import sys

REPORT_RE = re.compile(r"^assertions evaluated \[([^\]]*)\] fired \[([^\]]*)\]", re.M)


class Assertions:
    """Which assertions this run evaluated, and which of them fired."""

    def __init__(self, names):
        self.names = tuple(names)
        if len(set(self.names)) != len(self.names):
            raise SystemExit(f"duplicate assertion names: {self.names}")
        self.evaluated = set()
        self.fired = set()

    def judge(self, name, bad, message=None, into=None):
        """Evaluate one assertion. `bad` true means it fired."""
        assert name in self.names, name
        self.evaluated.add(name)
        if bad:
            self.fired.add(name)
            if into is not None and message is not None:
                into.append(message)
        return bool(bad)

    def any_fired(self):
        return bool(self.fired)

    def report(self, out=None):
        print(f"assertions evaluated [{' '.join(sorted(self.evaluated))}] "
              f"fired [{' '.join(sorted(self.fired))}]", file=out or sys.stdout)

    def list_and_exit(self):
        """`--list-assertions`: the table, one name per line, exit 0."""
        for name in self.names:
            print(name)
        return 0


# ---------------------------------------------------------------------------
# The other half: grading a tool that keeps such a register.
#
# A selftest that asks only "did it go red" passes a gate whose check has been
# deleted, because a neighbouring assertion reds on the same input. So a case
# names the assertion that must fire, and passes only when THAT one fires and
# no other does. Three refusals fall out, and the third is the strongest:
#
#   * an assertion the tool declares that no case fires;
#   * a case naming an assertion the tool does not declare;
#   * an assertion no case ever *reaches* -- a case can name one and never
#     arrive at the code that evaluates it.
#
# The declared list always comes from `--list-assertions`, never from a copy
# in the selftest: a copy rots toward green.


def parse_report(text):
    """(evaluated, fired) as name sets, or (None, None) if the run printed none."""
    m = REPORT_RE.search(text)
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


def refuse_duplicate_cases(table):
    """Two cases that red identically are one case, and the table does not say
    so: delete either and everything still passes."""
    same = {}
    for name, expect, want_exit in table:
        key = (frozenset(expect), want_exit)
        if key in same:
            raise SystemExit(f"'{name}' and '{same[key]}' assert the same thing "
                             f"({sorted(expect) or 'nothing'}, exit {want_exit}): "
                             "either is redundant, and deleting it would go unnoticed")
        same[key] = name


def drive(tool, table, run_one, verbose=False):
    """Run every case and grade it. `table` is (name, expect, want_exit) rows and
    `run_one(name)` returns (exit code, output). Returns a process exit code."""
    refuse_duplicate_cases(table)
    declared = all_assertions(tool)
    covered = {a for _, expect, _ in table for a in expect}
    unexercised = [a for a in declared if a not in covered]
    if unexercised:
        raise SystemExit(f"{tool} declares these and no case fires them: "
                         + ", ".join(unexercised))
    stray = sorted(covered - set(declared))
    if stray:
        raise SystemExit("cases expect assertions the tool does not declare: "
                         + ", ".join(stray))

    failures, evaluated = [], set()
    for name, expect, want_exit in table:
        code, out = run_one(name)
        if verbose:
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
    print(f"all {len(table)} cases: each of the {len(declared)} assertions "
          f"{os.path.basename(tool)} declares is reached, and fires on its own fault "
          f"and no other")
    return 0

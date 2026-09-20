#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Is every tool under tools/check/ actually run by something?

The Astrolog side found on 2026-09-20 that `ephsrv-soak.sh --selftest` --
the structural selftest they had told us about, documented in its own
header -- was wired into no runner at all.  **A rule existing is not a rule
running.**  Everything else under `tools/check/` asks whether a check could
go red; nothing asked whether anything invokes it.

So this holds a table: every `.py` beside it is either run by a named
runner, or carries a reason why nothing runs it.  Four things rot, and each
is an assertion:

  * a tool is added and nobody classifies it;
  * the table claims a runner runs a tool and the runner stopped;
  * the table excuses a tool that a runner does in fact run;
  * a tool is excused with no reason given.

**A mention is not an invocation**, and this is not a detail.  `gate.sh`
names `corrtest.py`, `ratesweep.py`, `crosstest.py`, `ephproto4_fixtures.py`
and `fakefixtures.py` in its comments while running none of them, and
`scheduled.sh` names `stars_fk5.py` while running `starstest.py`.  A naive
grep therefore reports five tools as gate-run that the gate never invokes --
the false green this file exists to refuse.  Comments and docstrings are
stripped before anything is looked for.

**Being graded is not being run.**  `starstest.py` drives `stars_fk5.py`
against a mutated catalogue and a scripted client: that grades the tool, it
does not do the tool's job.  A tool whose only invoker is its own selftest
is `graded-only` here, not run.

Hermetic and instant: it reads files and starts nothing.  In tools/gate.sh.
"""

import argparse
import ast
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assertlib  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CHECK = os.path.join(ROOT, "tools", "check")

# The runners: what a scheduled machine or a pre-commit gate actually starts.
RUNNERS = {"gate": "tools/gate.sh", "scheduled": "tools/scheduled.sh"}

ASSERTIONS = (
    "nothing-scanned",     # no tool was found, so "all classified" is about nothing
    "unlisted",            # a tool this table does not classify
    "stale-entry",         # this table names a file that is gone
    "claim-false",         # "run by R" and R does not invoke it
    "stale-exception",     # excused, and a runner runs it after all
    "unexplained",         # excused with no reason
)

# name -> (status, reason). A status naming a runner is checked against that
# runner's code; every other status must carry a reason, because "nothing
# runs this" is a decision and not an absence.
TOOLS = {
    # Run by tools/gate.sh, every commit.
    "ephproto4_registries.py": ("gate", ""),
    "registriestest.py": ("gate", ""),
    "adjudicatetest.py": ("gate", ""),
    "ratestest.py": ("gate", ""),
    "fixturestest.py": ("gate", ""),
    "runners.py": ("gate", ""),
    "runnerstest.py": ("gate", ""),

    # Run by tools/scheduled.sh.
    "corrtest.py": ("scheduled", ""),
    "crossruntest.py": ("scheduled", ""),
    "starstest.py": ("scheduled", ""),
    "loadselftest.py": ("scheduled", ""),
    "blindspots.py": ("scheduled", ""),

    # scheduled.sh does invoke crossrun.py, behind --with-cross: the status
    # names the runner because the runner's code names it, and the flag is
    # the reason. The three it drives are reached only through it, and each
    # reads another project's server -- a thing that reaches outside this
    # tree is asked for rather than arriving on a timer.
    "crossrun.py": ("scheduled", "only behind --with-cross; it drives the three below"),
    "crosstest.py": ("opt-in", "crossrun.py runs it; it needs both daemons"),
    "corrapplied.py": ("opt-in", "crossrun.py runs it against each server"),
    "ratesweep.py": ("opt-in", "crossrun.py runs it against each server"),

    # Nothing runs these for their own purpose, and that is the honest state.
    "stars_fk5.py": ("graded-only",
                     "needs a live server AND stars-raw/ with pyerfa; starstest.py grades "
                     "its assertions against a mutated catalogue, which is not the same as "
                     "comparing a server with the FK5. Run by hand before a release"),
    "ephproto4_fixtures.py": ("graded-only",
                              "needs Astrolog's conformance directory, which is not in this "
                              "tree and is not on this machine; fixturestest.py grades its "
                              "assertions against a set fakefixtures.py builds"),

    # Imported, never invoked.
    "assertlib.py": ("library", "the assertion register and the selftest driver"),
    "wirelib.py": ("library", "the v4 client wrapper the wire checkers share"),
    "binary_orbits.py": ("library", "ORB6 orbits, for stars_fk5.py and crosstest.py"),

    # Scripted stand-ins: run, but only ever by the selftest that needs them.
    "fakewire.py": ("stand-in", "a scripted server for corrtest.py and ratestest.py"),
    "fakestars.py": ("stand-in", "a scripted client for starstest.py"),
    "fakefixtures.py": ("stand-in", "builds a fixture set for fixturestest.py"),
    "fakecross.py": ("stand-in", "a scripted step for crossruntest.py"),
    "dropflag.py": ("stand-in", "the argument-dropping client for blindspots.py"),
}


def code_of(path):
    """A file's text with comments and docstrings removed.

    Everything here turns on the difference between naming a tool and
    running it, and both documents and both runners are full of prose that
    names tools.
    """
    with open(path) as f:
        text = f.read()
    if path.endswith(".py"):
        try:
            tree = ast.parse(text)
        except SyntaxError:
            return text
        drop = set()
        for node in ast.walk(tree):
            if not isinstance(node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef,
                                     ast.ClassDef)):
                continue
            body = getattr(node, "body", [])
            if (body and isinstance(body[0], ast.Expr)
                    and isinstance(body[0].value, ast.Constant)
                    and isinstance(body[0].value.value, str)):
                first = body[0].value
                drop.update(range(first.lineno, (first.end_lineno or first.lineno) + 1))
        lines = [("" if i + 1 in drop else l) for i, l in enumerate(text.splitlines())]
        text = "\n".join(lines)
    return "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("#"))


def invokes(text, name):
    """Does this code start `name`? A bare mention in prose is not an answer."""
    return re.search(r"(^|[\"'/\s])" + re.escape(name) + r"($|[\"'\s])", text) is not None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list-assertions", action="store_true",
                    help="print the assertions this tool makes, one per line, and exit")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--root", default=ROOT,
                    help="the tree to audit. A seam for tools/check/runnerstest.py, which "
                         "builds a small one; the table below is this tree's")
    ap.add_argument("--table",
                    help="a JSON {name: [status, reason]} to use instead of the table in "
                         "this file. The same seam, and never how it runs for real")
    a = ap.parse_args()
    if a.list_assertions:
        return assertlib.Assertions(ASSERTIONS).list_and_exit()
    asserts = assertlib.Assertions(ASSERTIONS)
    fails = []

    root = a.root
    check = os.path.join(root, "tools", "check")
    tools = TOOLS
    if a.table:
        import json
        with open(a.table) as f:
            tools = {k: tuple(v) for k, v in json.load(f).items()}

    found = sorted(f for f in os.listdir(check) if f.endswith(".py"))
    if asserts.judge("nothing-scanned", not found,
                     f"no .py was found under {check}, so \"every tool is classified\" "
                     "would be a statement about nothing", fails):
        asserts.report()
        return 1

    runner_code = {}
    for key, rel in RUNNERS.items():
        path = os.path.join(root, rel)
        runner_code[key] = code_of(path) if os.path.exists(path) else ""

    asserts.judge("unlisted", bool(set(found) - set(tools)),
                  "these are not in this file's table, so nothing says whether anything "
                  "runs them: " + ", ".join(sorted(set(found) - set(tools))), fails)
    asserts.judge("stale-entry", bool(set(tools) - set(found)),
                  "this file's table names these and they are gone: "
                  + ", ".join(sorted(set(tools) - set(found))), fails)

    broken, excused_but_run, unexplained = [], [], []
    for name in sorted(set(found) & set(tools)):
        status, why = tools[name]
        if status in RUNNERS:
            if not invokes(runner_code[status], name):
                broken.append(f"{name} (said to be run by {RUNNERS[status]})")
        else:
            if not why:
                unexplained.append(name)
            for key, code in runner_code.items():
                if invokes(code, name):
                    excused_but_run.append(f"{name} ({RUNNERS[key]} runs it)")
        if a.verbose:
            print(f"  {name:26s} {status:12s} {why}")

    asserts.judge("claim-false", bool(broken),
                  "the runner named does not invoke these any more: " + ", ".join(broken),
                  fails)
    asserts.judge("stale-exception", bool(excused_but_run),
                  "these are excused here and a runner runs them after all, so the "
                  "exception has rotted: " + ", ".join(excused_but_run), fails)
    asserts.judge("unexplained", bool(unexplained),
                  "these are not run by a runner and give no reason; \"nothing runs this\" "
                  "is a decision and is written down: " + ", ".join(unexplained), fails)

    by_status = {}
    for name in sorted(set(found) & set(tools)):
        by_status.setdefault(tools[name][0], []).append(name)
    print(f"{len(found)} tools under {os.path.relpath(check, root)}: "
          + ", ".join(f"{len(v)} {k}" for k, v in sorted(by_status.items())))
    for name in by_status.get("graded-only", []):
        # Printed every run, because this is the state the check exists to
        # keep visible rather than to forbid.
        print(f"  run by nothing: {name} -- {tools[name][1]}")

    for line in fails:
        print("FAIL  " + line)
    asserts.report()
    return 1 if asserts.any_fired() else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Can crosstest.py's legs go red, and only the leg that should?

Every other checker under tools/check/ has a selftest; the legs of
crosstest.py carried theirs as prose -- "1% on the textbook GM turns four
rows red", "a 1 s delta T error turns 12 rows into findings" -- each done by
hand once and never re-run.  This re-runs them.

Each case runs four legs with our daemon on BOTH endpoints (--self-compare,
so every cross-server verdict is trivially `agree` and only a leg's graded
reference can go red) under one named sabotage of a reference the harness
computes itself: the textbook solar GM, the delta T recovered from Horizons'
sidereal time, Horizons' light-time range, Horizons' barycentric Sun.  A case
passes only when the red rows are exactly the target leg's -- red somewhere
else would be a sabotage reaching a leg it should not, and red nowhere is a
leg that cannot see its own reference.  The control runs with no sabotage
and must be all green; the refusal case holds the rule that a sabotaged
harness never grades another project's server.

What it cannot see: the legs graded only against the other server, whose
reach tools/check/blindspots.py measures by dropping arguments instead.

Usage:
    tools/check/legtest.py --port 47190      (our daemon, already running)
"""

import argparse
import csv
import os
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CROSSTEST = os.path.join(ROOT, "tools", "check", "crosstest.py")
LEGS = "horizons,topo,bary,deflection-geo"
GREEN = {"agree", "expected-difference", "refused"}

# name, sabotage, the legs that must be red (and no other), exit code
CASES = [
    ("control: no sabotage, every leg green", None, set(), 0),
    ("1% on the textbook solar GM", "deflection-gm", {"deflection-geo"}, 1),
    ("one second on Horizons' Earth rotation", "topo-deltat", {"horizons-topo"}, 1),
    ("ten metres on Horizons' light-time range", "horizons-range", {"horizons"}, 1),
    ("ten kilometres on Horizons' barycentric Sun", "bary-sun", {"horizons-bary"}, 1),
]


def run(port, sabotage, out, self_compare=True):
    env = dict(os.environ)
    env.pop("PROMETHEIA_XTEST_SABOTAGE", None)
    if sabotage:
        env["PROMETHEIA_XTEST_SABOTAGE"] = sabotage
    ep = f"127.0.0.1:{port}"
    argv = [sys.executable, CROSSTEST, "--ours", ep, "--theirs", ep, "--legs", LEGS,
            "--out", out] + (["--self-compare"] if self_compare else [])
    return subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, env=env, timeout=600)


def red_legs(path):
    with open(path) as f:
        rows = csv.DictReader((line for line in f if not line.startswith("#")), delimiter="\t")
        return {r["leg"] for r in rows if r["verdict"] not in GREEN}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=47190, help="our daemon (both endpoints)")
    args = ap.parse_args()
    out = os.path.join(ROOT, "build", "legtest.tsv")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    failed = 0
    for name, sabotage, want, want_exit in CASES:
        if os.path.exists(out):
            os.remove(out)
        done = run(args.port, sabotage, out)
        if not os.path.exists(out):
            print(f"FAIL    {name}: no table written (exit {done.returncode})\n"
                  f"        {(done.stderr or done.stdout).strip()[-300:]}")
            failed += 1
            continue
        got = red_legs(out)
        ok = got == want and done.returncode == want_exit
        print(f"{'ok' if ok else 'FAIL':7s} {name}: red {sorted(got) or 'nothing'}"
              + ("" if ok else f", wanted {sorted(want) or 'nothing'} and exit {want_exit} "
                                f"(got exit {done.returncode})"))
        failed += not ok
    # A sabotaged harness must refuse to grade anything but our own server.
    done = run(args.port, "bary-sun", out, self_compare=False)
    refused = done.returncode != 0 and "must never grade" in (done.stderr + done.stdout)
    print(f"{'ok' if refused else 'FAIL':7s} a sabotage without --self-compare is refused")
    failed += not refused
    print(f"\n{len(CASES) + 1 - failed} of {len(CASES) + 1} cases pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

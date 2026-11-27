#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A scripted stand-in for the steps crossrun.py runs, for crossruntest.py.

crossrun.py's job is the sequence: start ours, read theirs, run crosstest.py
and the two sweeps, write a record, say what changed.  What has to be graded
is that sequence and what it concludes -- not the cross-test itself, which
has its own checks and needs two daemons.

So this answers in place of `crosstest.py`, `corrapplied.py` and
`ratesweep.py`: crossruntest.py copies it into a directory under each of
those names and points `--check-dir` at it.  It reads one JSON scenario from
`$PROMETHEIA_FAKECROSS` and does exactly what it says -- write these rows to
`--out`, exit with this code -- so a case can produce a record with no rows,
no record at all, or a step that fails, with no daemon anywhere.

**What this cannot see:** anything about the cross-test.  The rows here are
shaped like a record and mean nothing.
"""
import json
import os
import sys
import time

COLUMNS = ("leg", "case", "verdict", "detail")


def main():
    who = os.path.basename(sys.argv[0]).replace(".py", "")
    scn = json.loads(os.environ.get("PROMETHEIA_FAKECROSS", "{}"))
    out = None
    argv = sys.argv[1:]
    for i, a in enumerate(argv):
        if a == "--out" and i + 1 < len(argv):
            out = argv[i + 1]
    if scn.get("sleep") and who == "crosstest":
        # Long enough for the case that interrupts crossrun.py mid-step.
        time.sleep(float(scn["sleep"]))
    rows = scn.get("rows", [])
    if out is not None and scn.get("write", True):
        with open(out, "w") as f:
            f.write("# a scripted record, written by fakecross.py\n")
            f.write("\t".join(COLUMNS) + "\n")
            for r in rows:
                f.write("\t".join(str(r.get(c, "")) for c in COLUMNS) + "\n")
    print(f"verdicts: {len(rows)} row(s) from {who}")
    return int(scn.get("exit", {}).get(who, 0))


if __name__ == "__main__":
    sys.exit(main())

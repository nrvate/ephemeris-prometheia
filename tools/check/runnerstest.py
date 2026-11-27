#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Falsify every assertion runners.py makes, against a built tree.

runners.py is the answer to "a rule existing is not a rule running", so the
question it invites immediately is who grades it.  Each case here builds a
small tree -- two runner scripts and a handful of tools -- plus a table that
classifies them, and mutates one of the two so they disagree.

The tree is built rather than borrowed.  Pointing this at the real
`tools/check/` would grade the repository's own table instead of the logic,
so every case would be the control and nothing could be made to fail on
purpose.

The case that matters most is the last: a runner that **names** a tool in a
comment without running it.  That is not hypothetical -- `tools/gate.sh`
names five tools it does not invoke -- and a checker that matched on
mentions would call all five run and this whole file would be decoration.

assertlib.drive requires the named assertion to fire and no other, reads the
declared list from `--list-assertions`, and refuses an assertion no case
exercises.

Usage:
  python3 tools/check/runnerstest.py [-v]
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assertlib  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

# The tree every case starts from: a gate that runs one tool, a scheduled
# script that runs another, a library nothing invokes, and a tool excused
# with a reason.
GATE = '#!/usr/bin/env bash\n# names excused.py and does not run it\nstep python3 tools/check/graded.py\n'
SCHED = '#!/usr/bin/env bash\nstep python3 "$repo/tools/check/nightly.py"\n'
TOOLS = ["graded.py", "nightly.py", "lib.py", "excused.py"]
TABLE = {
    "graded.py": ["gate", ""],
    "nightly.py": ["scheduled", ""],
    "lib.py": ["library", "imported, never invoked"],
    "excused.py": ["graded-only", "needs a thing this machine does not have"],
}


def build(work, gate=GATE, sched=SCHED, tools=None, table=None):
    check = os.path.join(work, "tools", "check")
    os.makedirs(check)
    with open(os.path.join(work, "tools", "gate.sh"), "w") as f:
        f.write(gate)
    with open(os.path.join(work, "tools", "scheduled.sh"), "w") as f:
        f.write(sched)
    for name in (TOOLS if tools is None else tools):
        with open(os.path.join(check, name), "w") as f:
            f.write("# a tool\n")
    path = os.path.join(work, "table.json")
    with open(path, "w") as f:
        json.dump(TABLE if table is None else table, f)
    return path


def without(key):
    t = {k: list(v) for k, v in TABLE.items() if k != key}
    return t


def cases():
    """name, how to build, the assertions that must fire, exit."""
    return [
        # The control: the tree and the table agreeing. Without it the table
        # shows only that a broken tree reds.
        ("a tree whose table is right", {}, set(), 0),

        # No tool at all: "every tool is classified" about nothing.
        ("no tool in the tree", {"tools": [], "table": {}}, {"nothing-scanned"}, 1),

        # A tool added and nobody classified it. This is the one that fires
        # when someone writes a new check and stops there.
        ("a tool the table does not name",
         {"tools": TOOLS + ["newcheck.py"]}, {"unlisted"}, 1),

        # The table outliving a file.
        ("the table names a file that is gone",
         {"tools": [t for t in TOOLS if t != "lib.py"]}, {"stale-entry"}, 1),

        # The claim that rots silently: the runner stopped invoking it and
        # the table still says it does.
        ("the gate stopped running what the table says it runs",
         {"gate": "#!/usr/bin/env bash\necho nothing\n"}, {"claim-false"}, 1),

        # The other direction: excused, and run after all.
        ("an excused tool a runner runs after all",
         {"sched": SCHED + 'step python3 "$repo/tools/check/excused.py"\n'},
         {"stale-exception"}, 1),

        # Excused with no reason. "Nothing runs this" is a decision.
        ("a tool excused with no reason",
         {"table": dict(without("excused.py"), **{"excused.py": ["graded-only", ""]})},
         {"unexplained"}, 1),
    ]


# Not an assertion: a runner that only *names* a tool must not count as
# running it, and that fires nothing -- it is the control's own behaviour.
# assertlib would refuse it as a duplicate of the control, which is true:
# the case table cannot tell them apart. What tells them apart is that the
# mention is there at all, so it is checked separately.
def mention_only():
    gate = ('#!/usr/bin/env bash\n'
            '# excused.py is named right here and never run\n'
            'step python3 tools/check/graded.py\n')
    sched = SCHED + '# nightly.py also mentions excused.py in prose\n'
    return {"gate": gate, "sched": sched}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", default=os.path.join(ROOT, "tools/check/runners.py"))
    ap.add_argument("-v", "--verbose", action="store_true", help="echo each run's output")
    args = ap.parse_args()

    tmp = os.environ.get("CLAUDE_JOB_DIR")
    tmp = os.path.join(tmp, "tmp") if tmp else os.path.join(ROOT, "build")
    work = os.path.join(tmp, "runnerstest")

    table = cases()
    by_name = {c[0]: c for c in table}

    def run_with(how):
        shutil.rmtree(work, ignore_errors=True)
        os.makedirs(work)
        path = build(work, **how)
        out = subprocess.run([sys.executable, args.tool, "--root", work, "--table", path],
                             capture_output=True, text=True, timeout=120)
        return out.returncode, out.stdout + out.stderr

    code = assertlib.drive(args.tool, [(c[0], c[2], c[3]) for c in table],
                           lambda name: run_with(by_name[name][1]), verbose=args.verbose)

    rc, text = run_with(mention_only())
    note = []
    if rc != 0:
        note.append(f"exit {rc}, wanted 0: a runner that names a tool in a comment does "
                    "not run it, and this checker must not say it does")
    if "excused.py" in text.split("run by nothing:")[0]:
        note.append("reported excused.py as run, on the strength of a mention")
    print(f"{'ok' if not note else 'FAILED':6}  a runner that only names a tool")
    for line in note:
        print(f"          {line}")
    if note:
        code = 1

    shutil.rmtree(work, ignore_errors=True)
    return code


if __name__ == "__main__":
    sys.exit(main())

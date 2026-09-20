#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Falsify every assertion crossrun.py makes, with scripted steps.

crossrun.py is the one command that runs the whole cross-test and leaves a
record, and it is the script that points at another project's server.  What
it concludes -- "this run found nothing new", or a list of what changed --
is read by people on both sides, so its quiet failures were the expensive
kind: a record asked for and never written printed its own path and exited
0; a record with no rows read out as "found nothing new" about nothing; a
comparison that raised printed a line to stderr and exited 0; and an
interrupted run that ran no step at all exited 0 with a summary of nothing.

Each case runs crossrun.py with `--check-dir` pointed at copies of
`fakecross.py` named after the steps, and with two sockets this test opens
itself standing in for the daemons -- so no daemon runs, ours is never
started, and theirs is never touched.  `--records-dir` gives it a records
directory of the case's own making, so a "previous record" is whatever the
case wants and docs/crosstest/ is never read or written.

assertlib.drive requires the named assertion to fire and no other, reads the
declared list from `--list-assertions`, and refuses an assertion no case
exercises.

**What this cannot see:** the cross-test itself, and the parts of crossrun.py
that only a real run reaches -- the build, starting and stopping our daemon,
and the pre-flight exits (their daemon down, binaries missing), which exit 2
and are checked here only in that they are not assertions.

Usage:
  python3 tools/check/crossruntest.py [-v]
"""

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assertlib  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
STEPS = ("crosstest.py", "corrapplied.py", "ratesweep.py")

ROWS = [{"leg": "bodies", "case": "399", "verdict": "agree", "detail": ""},
        {"leg": "bodies", "case": "301", "verdict": "agree", "detail": ""},
        {"leg": "stars", "case": "Vega", "verdict": "agree", "detail": ""}]
MOVED = [dict(ROWS[0], verdict="differs")] + ROWS[1:]
OTHER_LEG = [{"leg": "nodes", "case": "399.a", "verdict": "agree", "detail": ""}]
WIDER = ROWS + OTHER_LEG


def record(path, rows):
    with open(path, "w") as f:
        f.write("# a scripted record\n")
        f.write("leg\tcase\tverdict\tdetail\n")
        for r in rows:
            f.write("\t".join(r[c] for c in ("leg", "case", "verdict", "detail")) + "\n")


def cases():
    """name, scenario, previous record's rows (or None), extra argv, fire, exit."""
    return [
        # The control: three rows written, a previous record carrying the
        # same three, every step green. This is what a quiet run looks like,
        # and without it the table shows only that a broken run reds.
        ("a run that found nothing new", {"rows": ROWS}, ROWS, [], set(), 0),

        # A step failed. The one condition crossrun.py already had.
        ("a step exits non-zero",
         {"rows": ROWS, "exit": {"ratesweep": 1}}, ROWS, [], {"step-failed"}, 1),

        # The record was asked for and the step wrote nothing. The summary
        # printed the path of a file that is not there.
        ("the record is never written",
         {"rows": ROWS, "write": False}, ROWS, [], {"record-missing"}, 1),

        # A record with no rows: nothing was cross-tested, and comparing it
        # with the previous one would say "found nothing new".
        ("the record has no rows", {"rows": []}, ROWS, [], {"record-empty"}, 1),

        # A previous record exists and cannot be compared: it carries only
        # legs this run did not run, so there is no overlap to diff. Counting
        # the absent legs as changes would report a change in their server
        # where the only thing that changed is what was asked.
        ("the previous record shares no leg",
         {"rows": ROWS}, OTHER_LEG, [], {"comparison-skipped"}, 1),

        # Not one step ran: the run is interrupted during the first one, so
        # nothing is appended and the summary is a summary of nothing. This
        # is the real path -- ctrl-C on a long cross-test -- and the only one
        # that reaches it, which is why the case sends a real SIGINT rather
        # than a flag invented to be tested.
        ("the run is interrupted during the first step",
         {"rows": ROWS, "sleep": 30}, ROWS, [], {"nothing-ran"}, 1),
    ]


# A narrowed run (--legs) records fewer legs than the record before it, and
# that is not a change in their server. It fires nothing and exits 0, so the
# case table cannot hold it -- assertlib would refuse it as a duplicate of the
# control, which is the truth: the table cannot tell them apart. What tells
# them apart is what the summary says, so it is graded on that.
def narrowed():
    """The scenario, the previous record, and what the summary must and must
    not say."""
    return ({"rows": ROWS}, WIDER,
            ["not compared (only one record carries them): nodes"],
            ["nodes"])


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    s.listen(8)
    return s, s.getsockname()[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", default=os.path.join(ROOT, "tools/check/crossrun.py"))
    ap.add_argument("-v", "--verbose", action="store_true", help="echo each run's output")
    args = ap.parse_args()

    for path in (os.path.join(ROOT, "build", "prometheiad"),
                 os.path.join(ROOT, "build", "prometheia-wire-client")):
        if not os.path.exists(path):
            # Not 0: crossrun.py refuses to run without them, so every case
            # would exit 2 and the table would grade nothing.
            print(f"cannot run: {path} is missing; build it first")
            return 2

    tmp = os.environ.get("CLAUDE_JOB_DIR")
    tmp = os.path.join(tmp, "tmp") if tmp else os.path.join(ROOT, "build")
    work = os.path.join(tmp, "crossruntest")

    table = cases()
    by_name = {c[0]: c for c in table}

    # Two sockets that accept and say nothing: crossrun.py only asks whether
    # something answers. Ours looks already up, so it starts no daemon and
    # stops none; theirs is a socket in this process and not their server.
    ours_sock, ours_port = free_port()
    theirs_sock, theirs_port = free_port()

    def run_one(name):
        _, scn, previous, extra, _, _ = by_name[name]
        shutil.rmtree(work, ignore_errors=True)
        check = os.path.join(work, "check")
        records = os.path.join(work, "records")
        os.makedirs(check)
        os.makedirs(records)
        for step in STEPS:
            shutil.copy(os.path.join(ROOT, "tools/check", "fakecross.py"),
                        os.path.join(check, step))
        if previous is not None:
            record(os.path.join(records, "2026-09-20a.tsv"), previous)
        env = dict(os.environ, PROMETHEIA_FAKECROSS=json.dumps(scn))
        argv = [sys.executable, args.tool, "--no-build", "--record",
                "--ours-port", str(ours_port), "--theirs", f"127.0.0.1:{theirs_port}",
                "--check-dir", check, "--records-dir", records,
                "--ephemeris", os.path.join(ROOT, "tools/check", "fakecross.py")] + extra
        if not scn.get("sleep"):
            out = subprocess.run(argv, capture_output=True, text=True, timeout=300, env=env)
            return out.returncode, out.stdout + out.stderr
        # Interrupt it once the first step is running, and only that process:
        # the scripted step is a child of it, not of this test.
        p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, env=env)
        seen = []
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            line = p.stdout.readline()
            if not line:
                break
            seen.append(line)
            if line.startswith("=== crosstest"):
                time.sleep(0.5)
                os.kill(p.pid, signal.SIGINT)
                break
        rest = p.stdout.read()
        p.wait(timeout=60)
        return p.returncode, "".join(seen) + rest

    code = assertlib.drive(args.tool, [(c[0], c[4], c[5]) for c in table],
                           run_one, verbose=args.verbose)

    scn, previous, must_say, must_not_diff = narrowed()
    by_name["narrowed"] = ("narrowed", scn, previous, [], set(), 0)
    rc, text = run_one("narrowed")
    note = []
    if rc != 0:
        note.append(f"exit {rc}, wanted 0: running fewer legs than the last record is "
                    "not a finding about their server")
    for want in must_say:
        if want not in text:
            note.append(f"said nothing containing {want!r}")
    changed = text.split("changed since")[-1] if "changed since" in text else ""
    for leg in must_not_diff:
        if leg in changed:
            note.append(f"reported {leg} as a change, and the only thing that changed "
                        "is which legs were asked for")
    print(f"{'ok' if not note else 'FAILED':6}  a run narrower than the last record")
    for line in note:
        print(f"          {line}")
    if note:
        code = 1
    ours_sock.close()
    theirs_sock.close()
    shutil.rmtree(work, ignore_errors=True)
    return code


if __name__ == "__main__":
    sys.exit(main())

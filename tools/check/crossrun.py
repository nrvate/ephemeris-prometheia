#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the whole cross-test in one command, and leave a record.

The cross-test needs two daemons, so it stays out of tools/gate.sh and has
always been run by hand: start ours, check theirs is up, run crosstest.py,
then corrapplied.py and ratesweep.py against each server, copy the numbers
into docs/CROSS-TEST.md. Every step is cheap and every step is forgettable,
and a record only existed when someone remembered the whole sequence. This
is that sequence.

    tools/check/crossrun.py --record

What it does, in order:

  1. builds the binaries it needs (--no-build to skip);
  2. starts `prometheiad` on --ours-port, unless something already answers
     there, and stops it at the end -- only ever the process it started;
  3. checks theirs answers on --theirs, and never starts, stops or restarts
     it: it is not ours. 47391 is the Astrolog side's long-running daemon
     and is never to be touched from here; 47392 is the spare they put up
     for cross-tests, and is the default;
  4. runs crosstest.py over every leg, writing docs/crosstest/<date><letter>
     when --record is given (the letter continues the sequence: after
     2026-09-20u comes v);
  5. runs corrapplied.py and ratesweep.py against each server in turn;
  6. prints one summary, and exits non-zero if any step did.

The record is a file, not a claim: what it means still has to be read and
written up in docs/CROSS-TEST.md, by whoever ran it.
"""

import argparse
import datetime
import os
import re
import signal
import socket
import string
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHECK = os.path.join(REPO, "tools", "check")
RECORDS = os.path.join(REPO, "docs", "crosstest")

# The Astrolog side's long-running daemon. Reading from it is fine; this
# script never manages any daemon but the one it starts, and says so here so
# that nobody adds a "restart theirs if it is down" convenience later.
THEIRS_PRODUCTION_PORT = 47391


def endpoint(s):
    host, _, port = s.partition(":")
    return host or "127.0.0.1", int(port or 0)


def listening(host, port, timeout=0.3):
    try:
        with socket.create_connection((host, port), timeout):
            return True
    except OSError:
        return False


def wait_until_listening(host, port, deadline_s, proc=None):
    end = time.monotonic() + deadline_s
    while time.monotonic() < end:
        if proc is not None and proc.poll() is not None:
            return False
        if listening(host, port):
            return True
        time.sleep(0.05)
    return False


def bump(letter):
    """'a' -> 'b', 'z' -> 'aa', 'az' -> 'ba': the sequence never runs out."""
    out, carry = [], True
    for c in reversed(letter):
        if not carry:
            out.append(c)
        elif c == "z":
            out.append("a")
        else:
            out.append(chr(ord(c) + 1))
            carry = False
    if carry:
        out.append("a")
    return "".join(reversed(out))


def next_record_letter():
    """The next letter in the record sequence, which runs across days: the
    records are 2026-09-18b .. 2026-09-19t .. 2026-09-20u, one sequence."""
    seen = set()
    if os.path.isdir(RECORDS):
        for name in os.listdir(RECORDS):
            m = re.fullmatch(r"\d{4}-\d{2}-\d{2}([a-z]+)\.tsv", name)
            if m:
                seen.add(m.group(1))
    if not seen:
        return "a"
    return bump(max(seen, key=lambda s: (len(s), s)))


def read_table(path):
    import csv
    with open(path) as f:
        lines = [ln for ln in f if not ln.startswith("#")]
    return list(csv.DictReader(lines, delimiter="\t"))


def latest_record(before=None):
    """The newest existing record, by the sequence letter."""
    best = None
    if os.path.isdir(RECORDS):
        for name in sorted(os.listdir(RECORDS)):
            m = re.fullmatch(r"\d{4}-\d{2}-\d{2}([a-z]+)\.tsv", name)
            if not m or (before and name == os.path.basename(before)):
                continue
            key = (len(m.group(1)), m.group(1))
            if best is None or key > best[0]:
                best = (key, os.path.join(RECORDS, name))
    return best[1] if best else None


def compare_records(new, prev):
    """What changed since the last record, by leg and verdict.

    The counts alone read badly out of context: this matrix carries 99
    standing findings against their server and 548 rows their client does
    not ask for, all adjudicated in CROSS-TEST.md. A run that reproduces
    them exactly has found nothing, and should say so in those words --
    otherwise every run looks alarming and soon nobody reads one.
    """
    import collections
    a = collections.Counter((r["leg"], r["verdict"]) for r in read_table(prev))
    b = collections.Counter((r["leg"], r["verdict"]) for r in read_table(new))
    out = []
    for key in sorted(set(a) | set(b)):
        if a[key] != b[key]:
            out.append(f"    {key[0]:22} {key[1]:22} {a[key]:5} -> {b[key]}")
    return out


class Step:
    """One command, its output kept for the summary."""

    def __init__(self, name, argv):
        self.name, self.argv = name, argv
        self.code, self.tail = None, ""


def run_step(step, verbose):
    print(f"\n=== {step.name}\n    {' '.join(step.argv)}", flush=True)
    p = subprocess.run(step.argv, cwd=REPO, text=True, capture_output=True)
    out = (p.stdout or "") + (p.stderr or "")
    if verbose:
        print(out, end="", flush=True)
    else:
        lines = [ln for ln in out.splitlines() if ln.strip()]
        for ln in lines[-12:]:
            print("    " + ln, flush=True)
    step.code = p.returncode
    # The lines that say what happened, not how much of it: the verdict
    # counts, and every row a step called out by name. A bare "FAIL" in the
    # summary sends the reader back to the scrollback, which is where the
    # by-hand sequence lost things in the first place.
    keep = [ln.strip() for ln in out.splitlines()
            if ln.strip().startswith(("verdicts:", "worst ", "OVER", "table:", "  - "))]
    step.tail = "\n         ".join(keep[:14])
    return step


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--ours-port", type=int, default=47190)
    ap.add_argument("--theirs", default="127.0.0.1:47392",
                    help="their daemon's endpoint; never started or stopped from here")
    ap.add_argument("--ephemeris", default=os.path.join(REPO, "ephe", "linux_p1550p2650.440"))
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--legs", help="passed through to crosstest.py (default: all of them)")
    ap.add_argument("--record", action="store_true",
                    help=f"write the leg table to {os.path.relpath(RECORDS, REPO)}/<date><letter>.tsv")
    ap.add_argument("--out", help="write the leg table here instead")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--no-sweeps", action="store_true",
                    help="the leg table only: skip corrapplied.py and ratesweep.py")
    ap.add_argument("--keep", action="store_true",
                    help="leave our daemon running (it is stopped by default)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    client = os.path.join(REPO, "build", "prometheia-wire-client")
    daemon = os.path.join(REPO, "build", "prometheiad")
    theirs_host, theirs_port = endpoint(args.theirs)
    ours = f"127.0.0.1:{args.ours_port}"

    if not args.no_build:
        print("=== build", flush=True)
        b = subprocess.run(
            ["cmake", "--build", "build", "--target", "prometheiad", "prometheia-wire-client",
             "prometheia-ut1", "-j8"],
            cwd=REPO, text=True, capture_output=True)
        if b.returncode != 0:
            sys.stderr.write(b.stdout + b.stderr)
            return "the binaries do not build"
    for path in (client, daemon):
        if not os.path.exists(path):
            return f"{os.path.relpath(path, REPO)} is missing (build it, or pass --no-build off)"
    if not os.path.exists(args.ephemeris):
        return f"{args.ephemeris} is missing; pass --ephemeris"

    # Theirs: read-only, always. A daemon we did not start is one we never
    # stop, and the production port is never even the default.
    if not listening(theirs_host, theirs_port):
        print(f"their daemon does not answer on {args.theirs}.", file=sys.stderr)
        print("Ask the Astrolog side to start their spare (CROSS-TEST.md, \"Setup\"):",
              file=sys.stderr)
        print(f"  astrolog-ephd --bind 127.0.0.1 --port {theirs_port} --threads 1 "
              "--ephe \"<tree>/ephem;<tree>\"", file=sys.stderr)
        if theirs_port != THEIRS_PRODUCTION_PORT:
            print(f"  (their long-running daemon on {THEIRS_PRODUCTION_PORT} is theirs to "
                  "manage, never restarted from here)", file=sys.stderr)
        return 2

    # Ours: start it only if the port is free, and then it is ours to stop.
    started = None
    if listening("127.0.0.1", args.ours_port):
        print(f"ours   already answering on {ours}; leaving it alone")
    else:
        print(f"ours   starting prometheiad on {ours}")
        log = open(os.path.join(REPO, "build", "crossrun-prometheiad.log"), "w")
        started = subprocess.Popen(
            [daemon, "--ephemeris", args.ephemeris, "--port", str(args.ours_port),
             "--threads", str(args.threads)],
            cwd=REPO, stdout=log, stderr=subprocess.STDOUT)
        if not wait_until_listening("127.0.0.1", args.ours_port, 30.0, started):
            started.send_signal(signal.SIGTERM)
            return f"prometheiad did not come up on {ours}; see build/crossrun-prometheiad.log"

    out = args.out
    previous = latest_record()
    if args.record and not out:
        today = datetime.date.today().isoformat()
        out = os.path.join(RECORDS, f"{today}{next_record_letter()}.tsv")

    steps = []
    try:
        argv = [sys.executable, os.path.join(CHECK, "crosstest.py"),
                "--ours", ours, "--theirs", args.theirs]
        if args.legs:
            argv += ["--legs", args.legs]
        if out:
            argv += ["--out", out]
        if args.verbose:
            argv += ["-v"]
        steps.append(run_step(Step("crosstest", argv), args.verbose))

        if not args.no_sweeps:
            for who, ep in (("ours", ours), ("theirs", args.theirs)):
                host, port = endpoint(ep)
                steps.append(run_step(Step(
                    f"corrapplied {who}",
                    [sys.executable, os.path.join(CHECK, "corrapplied.py"),
                     "--host", host, "--port", str(port), "--client", client]), args.verbose))
                steps.append(run_step(Step(
                    f"ratesweep {who}",
                    [sys.executable, os.path.join(CHECK, "ratesweep.py"),
                     "--server", ep, "--client", client]), args.verbose))
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
    finally:
        if started is not None and not args.keep:
            started.send_signal(signal.SIGTERM)
            try:
                started.wait(timeout=10)
            except subprocess.TimeoutExpired:
                started.kill()
            print(f"\nours   stopped (pid {started.pid})")
        elif started is not None:
            print(f"\nours   left running (pid {started.pid}), as asked")

    changes, compared_to = None, None
    if out and os.path.exists(out) and previous and os.path.abspath(previous) != os.path.abspath(out):
        try:
            changes, compared_to = compare_records(out, previous), os.path.basename(previous)
        except Exception as e:  # noqa: BLE001 -- a readable record beats a traceback
            print(f"\n(could not compare against {previous}: {e})", file=sys.stderr)

    print("\n=== summary")
    for s in steps:
        mark = "ok  " if s.code == 0 else "FAIL"
        if s.code != 0 and s.name == "crosstest" and changes == []:
            mark = "same"
        print(f"  {mark} {s.name}" + (f": {s.tail}" if s.tail else ""))
    if compared_to is not None:
        if not changes:
            print(f"\n  every verdict is what {compared_to} already carries: this run found "
                  "nothing new")
        else:
            print(f"\n  changed since {compared_to}:")
            for line in changes:
                print(line)
    if out:
        inside = os.path.abspath(out).startswith(REPO + os.sep)
        print(f"  record {os.path.relpath(out, REPO) if inside else os.path.abspath(out)}")
        print("  a record is a file, not a verdict: write up what it means in "
              "docs/CROSS-TEST.md")
    bad = [s.name for s in steps if s.code != 0]
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

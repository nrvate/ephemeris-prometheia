#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Falsify every assertion prometheia-load makes, and require the right one to fail.

Every fault injection this project has done was done by hand, once, and
written up in prose: docs/SERVER.md says a check was "fault-injected four
ways", and nothing re-runs those four ways. A falsification recorded as prose
is a measurement taken once. This is the structural version for the load
tool's assertions.

Each case breaks one thing and names the assertion that must catch it. A case
passes only when **that** assertion fires and no other one does. That last
clause is the whole point: a selftest asking only "did the run go red" passes
a gate whose bound has been deleted, because some other check will red on the
same broken input and the selftest cannot tell which. (The Astrolog side
found exactly that in their own selftest on 2026-09-20 by weakening a bound
to a number nothing could exceed: their plateau check caught the case the
bound was supposed to, and only a per-assertion selftest noticed.)

Seven of the ten cases need no sabotage hook at all -- they are induced by
how the server is started and how the tool is invoked, which is the strongest
kind. The three canary cases use `prometheia-load --sabotage`, which damages
the client's own copy of an answer next to the grading it falsifies, inside
the binary that ships rather than in a copy of the check.

**There is no list of assertions in this file.** The first version kept one
-- nine names and nine regexes -- and it was wrong on the day it was written:
`canary-none-graded` and `failures` had no case, so either could have been
deleted from prometheia-load with every case here still green, under a
docstring claiming to falsify every assertion the tool makes. Adding a
refusal when the two disagreed fixed half of it and left the half that
matters, because both lists were still in Python: an assertion added to the
binary appeared in neither.

So the binary names its own. `prometheia-load --list-assertions` is the
table, every run ends with `assertions evaluated [...] fired [...]`, and this
script reads names rather than matching patterns. A pattern would rot in the
worst direction -- stop matching, derived list shrinks, everything green --
which is the objection the Astrolog side raised against their own
message-grepping version the same afternoon. Three checks come out of it:
an assertion the binary declares that no case fires stops the script; a case
expecting an assertion the binary does not declare stops it; and an
assertion no case ever *reaches* fails the run at the end.

Falsified against itself, 2026-09-20, each fault failing exactly one case by
name: deleting the bound assertion ("did not fire: memory-over"); reporting
the failure count as zero, which fails only the sham-server case; making the
cache check fire unconditionally, which fails the two cases it should not
have fired in, one of which was otherwise reddening correctly on its own
assertion; adding a tenth assertion to the binary with no case, which stops
the script before it starts a daemon.

The silence guard is the one worth keeping. Deleting it used to fail its own
case while the run **still exited 1**, because the exit status counted an
ungraded canary separately -- red on the right input for the wrong reason,
with the assertion gone, which a selftest asking only "did it go red" passes.
prometheia-load's exit status is now exactly "did any assertion fire" and
counts nothing on its own, so deleting that judge now reports `exit 0, wanted
1` as well as `did not fire`. A hole found by falsification, closed in the
thing falsified rather than in the falsifier.

Those paragraphs are the one layer that stays prose -- falsifying the
falsifier means patching and rebuilding the binary, and nothing re-runs it.
The case table is the part that does.

It starts its own daemon and stops only what it started. It needs an
ephemeris, so it is not in tools/gate.sh, which must stay seconds.

    tools/check/loadselftest.py --ephemeris ephe/linux_p1550p2650.440
"""
import argparse
import os
import re
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# prometheia-load names its own assertions: --list-assertions is the whole
# table, and every run ends with the ones it evaluated and the ones that
# fired. So there is no table here and no pattern here -- both would be a
# second copy of a decision in the binary, and a pattern rots in the worst
# direction (it stops matching, the derived list shrinks, everything goes
# green).
REPORT = re.compile(r"^assertions evaluated \[([^\]]*)\] fired \[([^\]]*)\]", re.M)


def parse_report(text):
    """(evaluated, fired) as name sets, or (None, None) if the tool said nothing."""
    m = REPORT.search(text)
    if m is None:
        return None, None
    return set(m.group(1).split()), set(m.group(2).split())


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def listening(port):
    with socket.socket() as s:
        s.settimeout(0.3)
        return s.connect_ex(("127.0.0.1", port)) == 0


class Daemon:
    """A prometheiad this script started, and will stop."""

    def __init__(self, exe, ephemeris, cache_mb, budget=None):
        self.port = free_port()
        argv = [exe, "--ephemeris", ephemeris, "--port", str(self.port), "--threads", "1",
                "--max-conns-per-ip", "0", "--cache-mb", str(cache_mb), "--log-level", "quiet"]
        if budget is None:
            argv += ["--cells-per-sec", "0"]  # no compute limit
        else:
            max_cells, per_sec = budget
            argv += ["--max-cells", str(max_cells), "--cells-per-sec", str(per_sec)]
        self.proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(200):
            if listening(self.port):
                return
            if self.proc.poll() is not None:
                raise SystemExit(f"prometheiad exited {self.proc.returncode} before listening")
            time.sleep(0.1)
        self.stop()
        raise SystemExit(f"prometheiad never listened on {self.port}")

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)


class Sham:
    """An HTTP server that is not prometheiad: no /metrics worth the name.

    The 'no metrics' case has to be a server that answers and does not carry
    the counter, not a closed port -- a closed port fails to connect, which is
    a different refusal and would let the real one rot.
    """

    def __init__(self):
        self.port = free_port()
        self.proc = subprocess.Popen(
            [sys.executable, "-m", "http.server", str(self.port), "--bind", "127.0.0.1"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if listening(self.port):
                return
            time.sleep(0.05)
        self.stop()
        raise SystemExit("the sham HTTP server never listened")

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=5)


def all_assertions(load_exe):
    """The binary's own list, which is the only list."""
    done = subprocess.run([load_exe, "--list-assertions"], capture_output=True, text=True,
                          timeout=60)
    if done.returncode != 0 or not done.stdout.strip():
        raise SystemExit(f"{load_exe} --list-assertions said nothing; is it the current build?")
    return [n for n in done.stdout.split() if n]


def run_load(exe, port, pid, extra, seconds):
    argv = [exe, "--port", str(port), "--conns", "4", "--seconds", str(seconds), "--report", "600"]
    if pid:
        argv += ["--pid", str(pid)]
    argv += extra
    done = subprocess.run(argv, capture_output=True, text=True, timeout=seconds + 120)
    return done.returncode, done.stdout + done.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ephemeris", default=os.path.join(ROOT, "ephe/linux_p1550p2650.440"))
    ap.add_argument("--load", default=os.path.join(ROOT, "build/prometheia-load"))
    ap.add_argument("--daemon", default=os.path.join(ROOT, "build/prometheiad"))
    ap.add_argument("--seconds", type=int, default=10, help="each case's load (default 10)")
    ap.add_argument("-v", "--verbose", action="store_true", help="echo each run's output")
    args = ap.parse_args()

    for path in (args.load, args.daemon):
        if not os.path.exists(path):
            raise SystemExit(f"{path} is not built")
    if not os.path.exists(args.ephemeris):
        raise SystemExit(f"{args.ephemeris} is not there; pass --ephemeris")

    # **Every memory case needs its own daemon**, and finding that out is the
    # first thing this script did. Growth is measured from the moment a run
    # starts, so a server whose cache is already at its plateau grows by
    # nothing and passes any bound. Reusing one daemon made "the bound is
    # exceeded" pass with the bound at 1 MB, because the control case before
    # it had already filled the 8 MB cache. The assertion was right and the
    # test was wrong -- but the same arithmetic says --memory-bound against a
    # long-running server measures nothing, which is now written down in
    # docs/SERVER.md instead of being a trap.
    # A server spec is the daemon's --cache-mb and optional (--max-cells,
    # --cells-per-sec), or None for "not prometheiad at all".
    cases = [
        # name, server, extra argv, what must fire, exit
        ("control", (8, None), ["--memory-bound", "40"], set(), 0),
        ("the bound is exceeded", (8, None), ["--memory-bound", "1"], {"memory-over"}, 1),
        ("the cache stores nothing", (0, None), ["--memory-bound", "40"],
         {"memory-not-caching"}, 1),
        ("a canary value is wrong", (8, None), ["--sabotage", "values"], {"canary-differed"}, 1),
        ("a canary shape is wrong", (8, None), ["--sabotage", "shape"],
         {"canary-differed", "canary-shape"}, 1),
        ("a canary object is the wrong body", (8, None), ["--sabotage", "identity"],
         {"canary-differed", "canary-identity"}, 1),
        # The budget is sized so the baseline spends all of it: 8 canaries of
        # 10 rows is 800 cells, and one more request needs 100 more at 1 a
        # second, which a ten-second run cannot reach. Every request under
        # load is refused, so nothing is graded -- and silence is not a pass.
        ("every canary is refused", (8, (800, 1)), ["--rows", "10", "--canaries", "8"],
         {"canary-none-graded"}, 1),
        # A connection that never becomes a connection: the sham answers the
        # upgrade with plain HTTP. --canaries 0 so the baseline is not
        # attempted and this case reds on the failure count alone.
        ("the server is not prometheiad", None, ["--canaries", "0"], {"failures"}, 1),
    ]

    # The check this script failed when it was written: an assertion with no
    # case could be deleted from prometheia-load and nothing here would
    # notice. The list comes from the binary, so an assertion added there
    # and nowhere else stops this script instead of passing unseen.
    # Two cases that red identically are one case, and the table does not
    # say so: delete either and everything still passes. Checked on the
    # real list rather than by reading the file, because a scan that misses
    # a case reports exactly what a table with no duplicates reports -- I
    # tried it the other way first and it silently saw seven of eight.
    # (The Astrolog side measured the same property on their table and
    # found it already held; this is the same question asked here.)
    same = {}
    for name, _, _, expect, want_exit in cases:
        key = (frozenset(expect), want_exit)
        if key in same:
            raise SystemExit(f"'{name}' and '{same[key]}' assert the same thing "
                             f"({sorted(expect) or 'nothing'}, exit {want_exit}): "
                             "either is redundant, and deleting it would go unnoticed")
        same[key] = name

    declared = all_assertions(args.load)
    covered = {a for c in cases for a in c[3]} | {"refused-no-pid", "refused-no-metrics"}
    unexercised = [a for a in declared if a not in covered]
    if unexercised:
        raise SystemExit("prometheia-load declares these and no case fires them: "
                         + ", ".join(unexercised))
    stray = sorted(covered - set(declared))
    if stray:
        raise SystemExit("cases expect assertions the binary does not declare: "
                         + ", ".join(stray))

    failures = []
    evaluated = set()  # every assertion any case actually reached
    caching = None
    sham = Sham()
    try:
        for name, server, extra, expect, want_exit in cases:
            if server is None:
                port, pid = sham.port, 0
            else:
                if caching is not None:
                    caching.stop()
                cache_mb, budget = server
                caching = Daemon(args.daemon, args.ephemeris, cache_mb=cache_mb, budget=budget)
                port, pid = caching.port, caching.proc.pid
            code, out = run_load(args.load, port, pid, extra, args.seconds)
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
                extra_fired = sorted(got - expect)
                if missing:
                    note.append("did not fire: " + ", ".join(missing))
                if extra_fired:
                    # The Astrolog side's case: red, but not on its own
                    # assertion. Without this the selftest is blind to a
                    # deleted check.
                    note.append("fired but should not have: " + ", ".join(extra_fired))
            status = "ok" if not note else "FAILED"
            print(f"{status:6}  {name}")
            for line in note:
                print(f"          {line}")
            if note:
                failures.append(name)

        # The two refusals need no load at all, and must happen before any is
        # applied: a tool that half-asserts and reports a pass is the failure
        # being guarded against.
        for name, port, extra, expect in [
            ("--memory-bound without --pid", caching.port, ["--memory-bound", "40"],
             {"refused-no-pid"}),
            ("--memory-bound with no /metrics", sham.port, ["--memory-bound", "40"],
             {"refused-no-metrics"}),
        ]:
            pid = caching.proc.pid if "no /metrics" in name else 0
            code, out = run_load(args.load, port, pid, extra, 3)
            seen, got = parse_report(out)
            note = []
            if got is None:
                note.append("the refusal printed no assertion report")
                got = set()
            else:
                evaluated |= seen
            if code != 2:
                note.append(f"exit {code}, wanted 2 (refused before any load)")
            if got != expect:
                note.append(f"assertions fired {sorted(got)}, wanted {sorted(expect)}")
            print(f"{'ok' if not note else 'FAILED':6}  {name}")
            for line in note:
                print(f"          {line}")
            if note:
                failures.append(name)
    finally:
        if caching is not None:
            caching.stop()
        sham.stop()

    print()
    never = [a for a in declared if a not in evaluated]
    if never:
        # Declared, and no case ever even reached it: it could be deleted
        # from prometheia-load with every case above still green.
        print("no case reaches: " + ", ".join(never))
    if failures:
        print(f"{len(failures)} of {len(cases) + 2} cases FAILED: " + ", ".join(failures))
    if failures or never:
        return 1
    print(f"all {len(cases) + 2} cases: each of the {len(declared)} assertions "
          f"prometheia-load declares is reached, and fires on its own fault and no other")
    return 0


if __name__ == "__main__":
    sys.exit(main())

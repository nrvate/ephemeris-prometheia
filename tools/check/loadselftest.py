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

The table below is the list this script is checked against, and the check is
to read it: the first version of this file declared nine assertions and
exercised seven. `canary-none-graded` and `failures` had no case, so either
could have been deleted from prometheia-load and every case here would still
have passed -- the same overclaim ("falsifies every assertion") that the rest
of this docstring is about. Both turned out to need no hook either, so if a
new assertion is added to ASSERTIONS without a case, say so rather than
leaving the count to be recounted by hand later.

Falsified against itself, 2026-09-20, each fault failing exactly one case by
name: deleting the bound assertion from prometheia-load ("did not fire:
memory-over"); deleting the silence guard, which fails only "every canary is
refused" -- and that run still exited 1, because the exit status still counts
an ungraded canary, so a selftest asking only whether something went red
would have passed a deleted assertion; reporting the failure count as zero,
which fails only the sham-server case; making the cache check fire
unconditionally, which fails the two cases it should not have fired in
("fired but should not have: memory-not-caching"), one of which was otherwise
reddening correctly on its own assertion.

That paragraph is the one layer that stays prose -- falsifying the falsifier
means patching and rebuilding the binary, and nothing re-runs it. The case
table is the part that does.

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

# Every distinct way prometheia-load can report a failure, as a name and the
# pattern that says it fired. A case declares which of these it expects; any
# other one firing fails the case, whatever the exit status.
ASSERTIONS = {
    "canary-differed": re.compile(r"^canaries .*, ([1-9]\d*) differed", re.M),
    "canary-shape": re.compile(r"^  canary \d+ .* shape: ", re.M),
    "canary-identity": re.compile(r"came back as NAIF", re.M),
    "canary-none-graded": re.compile(r"nothing was graded", re.M),
    "memory-over": re.compile(r"^  OVER: ", re.M),
    "memory-not-caching": re.compile(r"^  NOT CACHING: ", re.M),
    "refused-no-pid": re.compile(r"--memory-bound needs --pid", re.M),
    "refused-no-metrics": re.compile(r"--memory-bound needs the server's /metrics", re.M),
    "failures": re.compile(r"^failures   ([1-9]\d*)", re.M),
}


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


def fired(text):
    return {name for name, pattern in ASSERTIONS.items() if pattern.search(text)}


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
    # notice.
    unexercised = set(ASSERTIONS) - {a for c in cases for a in c[3]} - {
        "refused-no-pid", "refused-no-metrics"}
    if unexercised:
        raise SystemExit("no case exercises: " + ", ".join(sorted(unexercised)))

    failures = []
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
            got = fired(out)
            note = []
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
            got = fired(out)
            note = []
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
    if failures:
        print(f"{len(failures)} of {len(cases) + 2} cases FAILED: " + ", ".join(failures))
        return 1
    print(f"all {len(cases) + 2} cases: each assertion fires on its own fault and no other")
    return 0


if __name__ == "__main__":
    sys.exit(main())

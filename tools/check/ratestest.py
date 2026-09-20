#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Falsify every assertion ratesweep.py makes, and require the right one to fail.

ratesweep.py asks whether a server's RATE columns describe its own positions,
and it is the second tool here whose output is an accusation against another
project: it found the Astrolog server exceeding its advertised rate bound by
16x, which that project fixed.  Until 2026-09-20 nothing graded it, and what
that hid was not an arithmetic error but a missing assertion.  `asked` and
`answered` were counted, printed, and compared to nothing.  A sweep in which
every single object came back unanswered found no exceedance, printed "within
its advertisement everywhere this sweep reached", and exited 0.

Breadth is that tool's entire claim -- it exists because "a bound is only as
wide as the object list that measured it" -- so a sweep that reached almost
nothing is not a weaker pass.  It is a different statement, and five of the
six assertions are now about the sweep having happened at all.

Each case below breaks one thing and names the assertion that must catch it;
assertlib.drive requires that one to fire and no other, reads the declared
list from `ratesweep.py --list-assertions`, and refuses a case naming an
assertion the tool does not declare, an assertion no case fires, and an
assertion no case ever *reaches*.

The fault goes in at the client, as in corrtest.py: tools/check/fakewire.py
answers `--jd/--step/--count` from a smooth analytic ephemeris whose rate
columns are the exact derivative, so the only difference ratesweep.py can
measure is the one the scenario asked for.  No daemon, no ephemeris, no
socket.  The grid is cut to one configuration, one epoch and the eleven Moon
objects (`--configs 1 --epochs 1 --only Moon`), which is why this runs in
seconds; the full sweep is 3,575 requests and belongs in tools/scheduled.sh.

**What this cannot see:** fakewire.py reproduces the client's output rather
than speaking v4, so nothing here grades wirelib.py against a real client.
It rots red -- the regexes stop matching, no reply carries a WELCOME, and the
`no-welcome` case is the one that notices.
"""

import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assertlib  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

# One configuration, one epoch, the eleven objects whose name says Moon: the
# body, its four points in mean and osculating form, and the two natural
# apsides. Four of the eleven are nodes, which is what makes the min-fraction
# case a minority without a second knob.
GRID = ["--configs", "1", "--epochs", "1", "--only", "Moon"]

# Wide enough that the analytic model's own truncation error (~1e-14 deg/day)
# is nowhere near it, tight enough that the injected error is unambiguous.
BOUND = [1e-3, 1e-7]


def cases():
    """name, scenario, extra argv, the assertions that must fire, exit code."""
    return [
        # The control. Without it the table below shows only that broken
        # servers go red, not that correct ones stay green -- and this tool
        # points at somebody else's server, so a false accusation costs more
        # than a missed one.
        ("rates that describe the positions",
         {"mode": "rates", "ratesbound": BOUND}, [], set(), 0),

        # Ten times the advertised bound in the longitude rate. This is the
        # shape of the real finding: the Astrolog server's rates were honest
        # arithmetic against an advertisement that was too tight for the
        # objects this sweep reaches.
        ("a longitude rate outside the advertisement",
         {"mode": "rates", "ratesbound": BOUND, "rate_error_deg": 1e-2},
         [], {"exceeds-advertisement"}, 1),

        # Every object refused. No rate was measured, so "no exceedance" is a
        # statement about nothing. This is the case that did not exist.
        ("the server answers nothing",
         {"mode": "rates", "ratesbound": BOUND, "answer_objects": ["no such object"]},
         [], {"nothing-answered"}, 1),

        # Four of eleven: enough to look like a sweep, far too few to be one.
        # A server that refuses apsides and answers nodes is exactly how a
        # bound comes to be the width of a list. The substrings match the
        # *wire* name the client is given (`node 301.a.m`), not the display
        # name -- matching "node" here would have taken ten of the eleven and
        # this case would have gone green.
        ("the server answers only the lunar nodes",
         {"mode": "rates", "ratesbound": BOUND, "answer_objects": ["301.a.", "301.d."]},
         [], {"min-fraction"}, 1),

        # No WELCOME anywhere, which is what a drift in wirelib's regex looks
        # like from here. Every verdict would then be reached against the A.3
        # default rather than this server's advertisement, and the rows still
        # pass, so nothing else can notice.
        ("no reply carries a WELCOME",
         {"mode": "rates", "no_welcome": True}, [], {"no-welcome"}, 1),

        # One server, two advertisements in one sweep. The first is enforced
        # and the rest were silently dropped; the rows are inside both, so
        # only this assertion can see it.
        ("the bound changes mid-sweep",
         {"mode": "rates", "ratesbound": BOUND, "ratesbound_alt": [5e-3, 1e-7],
          "ratesbound_alt_when": "node"},
         [], {"bound-varies"}, 1),

        # Clean at the ΔT the grid fixes, ten times the bound at a ΔT it does
        # not. This is the 2026-09-20 Polaris shape, mirrored: the grid would
        # report this server within its advertisement everywhere it reached,
        # and be wrong because of a constant nobody was looking at. Before the
        # widening pass existed, this scenario exited 0 with no output about
        # ΔT at all.
        ("a rate outside the advertisement only at a delta T the grid skips",
         {"mode": "rates", "ratesbound": BOUND,
          "rate_error_deg_alt": 1e-2, "rate_error_alt_under": 25.0},
         [], {"deltat-undersampled"}, 1),

        # The grid selected nothing at all. Judged before the others on
        # purpose: an empty grid also answers nothing, and two assertions
        # firing on one input is how a deleted check stays invisible.
        ("the object filter matches nothing",
         {"mode": "rates", "ratesbound": BOUND}, ["--only", "no such object"],
         {"nothing-asked"}, 1),
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", default=os.path.join(ROOT, "tools/check/ratesweep.py"))
    ap.add_argument("--fake", default=os.path.join(ROOT, "tools/check/fakewire.py"))
    ap.add_argument("-v", "--verbose", action="store_true", help="echo each run's output")
    args = ap.parse_args()

    tmp = os.environ.get("CLAUDE_JOB_DIR")
    tmp = os.path.join(tmp, "tmp") if tmp else os.path.join(ROOT, "build")
    os.makedirs(tmp, exist_ok=True)

    table = cases()
    by_name = {c[0]: c for c in table}

    def run_one(name):
        _, scenario, extra, _, _ = by_name[name]
        path = os.path.join(tmp, "ratescenario.json")
        with open(path, "w") as f:
            json.dump(scenario, f)
        env = dict(os.environ, FAKEWIRE_SCENARIO=path)
        out = subprocess.run([sys.executable, args.tool, "--client", args.fake] + GRID + extra,
                             capture_output=True, text=True, timeout=600, env=env)
        return out.returncode, out.stdout + out.stderr

    return assertlib.drive(args.tool, [(c[0], c[3], c[4]) for c in table],
                           run_one, verbose=args.verbose)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Which arguments can each cross-test leg not see arriving?

A leg asks both servers a question and compares the answers.  If the
argument that makes the question interesting never reaches either server --
the client drops it, the leg forgets to pass it, a server ignores it -- both
answer the same thing, the comparison agrees, and the row is green.  The
deflection leg met this on 2026-09-20: dropping `--topo` left all twenty of
its rows green, and only the site-shift rows, which compare a server with
its *own* other answer, noticed (docs/HANDOFF.md, "Next" item 5).

This measures it.  For each leg:

  1. run it with **our daemon on both endpoints** (`--self-compare`), which
     makes every cross-server verdict trivially `agree`, so only a leg's
     non-comparative checks -- an anchor, a shift from the server's own
     other answer, a refusal it expects -- can still go red;
  2. record every argument the leg actually sent, from the client's own
     argv rather than from a list written beside this file;
  3. re-run once per argument with that argument stripped from every
     request, and see whether the leg goes red.

Green with the argument gone means the leg cannot tell it arrived.  That is
not automatically a fault -- a pure two-server comparison is blind to
anything that fails on both sides, and that is what a cross-test is -- but
it is the leg's reach, and it should be known and not discovered by a drop
that mattered.

**What this cannot see:** whether a leg asks the right question, whether its
band is the right band, and anything that needs two different servers.  It
measures reach, not correctness.

Needs our daemon and no other: their server is never contacted.

Usage:
  tools/check/blindspots.py --port 47190
  tools/check/blindspots.py --port 47190 --legs deflection-topo,stars
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
CHECK = os.path.join(ROOT, "tools", "check")

ALL_LEGS = ("surfaces,same,horizons,hamburg,helio,apparent,topo,bary,deflection,"
            "deflection-geo,deflection-topo,arrival,points,rates,sidereal,sidsweep,"
            "sidinstant,stars")

# Arguments that are not part of a leg's question: the endpoint it is asked
# of, and the transport. Dropping one of these does not test a leg's reach,
# it stops the request from being made at all.
PLUMBING = {"--host", "--port"}

ASSERTIONS = (
    "no-leg-measured",   # not one leg produced a map
    "control-red",       # a leg's own control run is not green, so nothing follows
    "no-arguments",      # a leg sent no argument to drop: "0 blind spots" of nothing
    "not-injected",      # a drop removed nothing, so the leg staying green means nothing
    "reach-shrank",      # a leg no longer sees an argument the record says it saw
    "reach-grew",        # a leg sees one the record says it did not
    "unrecorded",        # a leg or an argument the record does not mention
)

# The record: what each leg could see when it was last measured. It is
# committed, so a leg that quietly loses a non-comparative check -- the one
# kind of check that survives both servers being wrong together -- reds here
# instead of going unnoticed until a drop matters. Graded in both directions,
# because a record of exceptions rots both ways.
EXPECTED = os.path.join(CHECK, "blindspots.json")


def run_leg(args, leg, drop=None, log=None, counter=None):
    """One crosstest run. Returns (exit code, output)."""
    env = dict(os.environ)
    env["PROMETHEIA_DROP_CLIENT"] = args.client
    for var in ("PROMETHEIA_DROP_FLAG", "PROMETHEIA_DROP_LOG", "PROMETHEIA_DROP_COUNT"):
        env.pop(var, None)
    if drop:
        env["PROMETHEIA_DROP_FLAG"] = drop
    if log:
        env["PROMETHEIA_DROP_LOG"] = log
    if counter:
        env["PROMETHEIA_DROP_COUNT"] = counter
    ep = f"127.0.0.1:{args.port}"
    argv = [sys.executable, os.path.join(CHECK, "crosstest.py"),
            "--ours", ep, "--theirs", ep, "--self-compare",
            "--client", os.path.join(CHECK, "dropflag.py"), "--legs", leg]
    out = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True,
                         timeout=args.timeout, env=env)
    return out.returncode, out.stdout + out.stderr


def arguments_sent(path):
    """Every flag the leg's own requests carried, from the client's argv."""
    flags = set()
    with open(path) as f:
        for line in f:
            for word in line.rstrip("\n").split("\t"):
                if word.startswith("--"):
                    flags.add(word)
    return sorted(flags - PLUMBING)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=47190, help="our daemon; both endpoints")
    ap.add_argument("--legs", default=ALL_LEGS)
    ap.add_argument("--client", default=os.path.join(ROOT, "build", "prometheia-wire-client"))
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--json", help="also write the map here")
    ap.add_argument("--expected", default=EXPECTED,
                    help="the committed map this run is graded against")
    ap.add_argument("--write-expected", action="store_true",
                    help="rewrite that record from this run. For a deliberate change to a "
                         "leg, with the run that measured it; never to make a red go away")
    ap.add_argument("--list-assertions", action="store_true",
                    help="print the assertions this tool makes, one per line, and exit")
    a = ap.parse_args()
    if a.list_assertions:
        return assertlib.Assertions(ASSERTIONS).list_and_exit()
    asserts = assertlib.Assertions(ASSERTIONS)

    if not os.path.exists(a.client):
        print(f"cannot run: {a.client} is missing; build it first")
        return 2

    tmp = os.environ.get("CLAUDE_JOB_DIR")
    tmp = os.path.join(tmp, "tmp") if tmp else os.path.join(ROOT, "build")
    work = os.path.join(tmp, "blindspots")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)

    fails = []
    out = {}
    for leg in a.legs.split(","):
        log = os.path.join(work, f"{leg}.argv")
        code, text = run_leg(a, leg, log=log)
        banner = "SELF-COMPARE:" in text
        if asserts.judge("control-red", code != 0 or not banner,
                         f"{leg}: the control run "
                         + ("never reached the leg" if not banner else f"exited {code}")
                         + "; an argument dropped from a leg that is already red proves "
                           "nothing, so this leg is not measured", fails):
            out[leg] = {"control": "red"}
            print(f"{leg:18s} CONTROL RED -- not measured")
            continue
        flags = arguments_sent(log) if os.path.exists(log) else []
        if asserts.judge("no-arguments", not flags,
                         f"{leg}: sent no argument this could drop, so \"nothing blind\" "
                         "would be a statement about nothing", fails):
            out[leg] = {"control": "green", "seen": [], "blind": []}
            print(f"{leg:18s} NO ARGUMENTS")
            continue
        seen, blind = [], []
        for flag in flags:
            counter = os.path.join(work, f"{leg}.count")
            if os.path.exists(counter):
                os.remove(counter)
            c, t = run_leg(a, leg, drop=flag, counter=counter)
            taken = 0
            if os.path.exists(counter):
                with open(counter) as f:
                    taken = sum(int(x) for x in f.read().split())
            if asserts.judge("not-injected", taken == 0,
                             f"{leg}: dropping {flag} removed nothing from any request, "
                             "so this leg staying green says nothing about it -- the "
                             "argument was discovered from the control run's own argv, "
                             "so the two runs did not ask the same thing", fails):
                blind.append(flag + " (not injected)")
                continue
            if "SELF-COMPARE:" not in t:
                # The run never reached the leg: dropping this argument broke
                # the WELCOME probe crosstest.py makes before any leg. That is
                # not the leg noticing.
                blind.append(flag + " (probe)")
            elif c != 0:
                seen.append(flag)
            else:
                blind.append(flag)
        out[leg] = {"control": "green", "seen": seen, "blind": blind}
        print(f"{leg:18s} sees {len(seen)}/{len(flags)}: "
              + (" ".join(seen) if seen else "nothing")
              + ("   blind to " + " ".join(blind) if blind else ""))

    print()
    asserts.judge("no-leg-measured", not any(v.get("control") == "green" for v in out.values()),
                  "not one leg was measured, so this map is empty and says nothing about "
                  "any leg's reach", fails)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(out, f, indent=2, sort_keys=True)
        print(f"map: {a.json}")
    measured = {leg: v for leg, v in out.items() if v.get("control") == "green"}
    if a.write_expected:
        with open(a.expected, "w") as f:
            json.dump(measured, f, indent=2, sort_keys=True)
        print(f"record rewritten: {a.expected} ({len(measured)} legs)")
    elif os.path.exists(a.expected):
        with open(a.expected) as f:
            want = json.load(f)
        shrank, grew, unknown = [], [], []
        for leg, got in sorted(measured.items()):
            if leg not in want:
                unknown.append(f"{leg} (the whole leg)")
                continue
            was, now = set(want[leg]["seen"]), set(got["seen"])
            both = set(want[leg]["seen"]) | set(want[leg]["blind"])
            for flag in sorted(set(got["seen"]) | set(got["blind"])):
                if flag not in both:
                    unknown.append(f"{leg} {flag}")
            shrank += [f"{leg} {f}" for f in sorted(was - now) if f in both]
            grew += [f"{leg} {f}" for f in sorted(now - was) if f in both]
        asserts.judge("reach-shrank", bool(shrank),
                      "these no longer red when the argument is dropped, so a leg has "
                      "lost the only kind of check that survives both servers being "
                      "wrong together: " + ", ".join(shrank), fails)
        asserts.judge("reach-grew", bool(grew),
                      "these now red when the argument is dropped and the record says "
                      "they did not; the leg gained a check -- rerun with "
                      "--write-expected: " + ", ".join(grew), fails)
        asserts.judge("unrecorded", bool(unknown),
                      "the record does not mention these, so nothing said whether they "
                      "should red: " + ", ".join(unknown), fails)
    else:
        print(f"no record at {a.expected}; --write-expected writes one")

    for line in fails:
        print("FAIL  " + line)
    asserts.report()
    return 1 if asserts.any_fired() else 0


if __name__ == "__main__":
    sys.exit(main())

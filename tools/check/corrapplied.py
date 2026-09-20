#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a live v4 server's corrApplied against its own behaviour.

Protocol v4 3.4 makes the per-object ``corrApplied`` byte a statement of
STRUCTURAL availability: a bit is clear when the engine cannot apply that
correction here at all, and set when its model ran -- even when the model
contributed nothing.  3.5a then forbids gating a conformance comparison on
it, so this is deliberately NOT a cross-server diff.  Every check below
compares one server against itself:

  independence  corrApplied is the same whatever the request's mask asked
                for.  A server that echoes the request into the slot fails
                here and nowhere else, and echoing is the easy bug -- this
                repository shipped it until the v4 rewrite.

  declared      corrApplied is a subset of the correction bits WELCOME says
                can be honoured for that observer AND object kind (A.3
                0x0004, widened or narrowed per kind by 0x0014).  Two things
                the same server said, disagreeing.

  truthful      asking for exactly one correction either moves the position
                or does not.  A clear bit that moves the sky is a false
                denial.  A set bit that moves nothing is fine and is
                reported as a note, because that is what a term
                contributing ~0 looks like (aberration at the barycentre).

Only a clear movement accuses.  Anything under the noise ceiling is a note,
never a failure: this harness is pointed at other people's servers, so it
stays quiet unless it is sure.

A.3 0x0004 lists the EXACT masks a server honours per observer, 0x0014 says
which of them apply to which object kind, and any other combination is ERROR
11 (3.5a), so the masks asked for come from the server's own WELCOME -- both
records, since reading 0x0004 alone accuses a server of claiming what it
declared in the other one.  A check whose masks the server does not honour is reported as
inapplicable -- never as passed.  And checking nothing is not a pass: the run
fails if no case was checked, if fewer than --min-fraction of them were, or if
any of the three checks never ran at all.  A green that could not have been
red is worse than no check, because someone believes it.

Usage:
    tools/check/corrapplied.py [--host H] [--port N] [--client PATH] [-v]
"""

import argparse
import math
import shutil
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wirelib  # noqa: E402  (the one reader of the client's output)

LIGHT_TIME, DEFLECTION, ABERRATION = 1, 2, 4
BIT_NAME = {LIGHT_TIME: "light time", DEFLECTION: "deflection", ABERRATION: "aberration"}
OBS_BIT = {"geo": 0, "topo": 1, "helio": 2, "bary": 3, "body": 4}

# A movement this size is real and the server must own it; one under the
# floor is indistinguishable from f64 noise in a degree-valued column.
MOVED_ARCSEC = 1e-3
STILL_ARCSEC = 1e-5



class Answer:
    """One object's answer: its META claim and its first row."""

    def __init__(self, name, corr_applied, err, err_text, lon_deg, lat_deg):
        self.name = name
        self.corr_applied = corr_applied
        self.err = err
        self.err_text = err_text
        self.lon_deg = lon_deg
        self.lat_deg = lat_deg


# The assertions this tool makes, by name. tools/check/corrtest.py reads
# this table with --list-assertions rather than keeping a copy: a list beside
# the thing it describes rots toward green, and this project has shipped that
# twice (prometheia-load, adjudicatetest.py). The exit status is exactly "did
# any assertion fire" and counts nothing on its own, so deleting a judge
# below reports both "did not fire" and "exit 0, wanted 1" -- a deleted check
# cannot hide behind a neighbouring exit condition.
ASSERTIONS = (
    "welcome-no-corrmasks",   # A.3 0x0004 is empty: nothing can be asked
    "independence",           # corrApplied tracks the request (3.4 says structural)
    "declared",               # corrApplied claims what no WELCOME record carries
    "truthful",               # a clear bit that moves the sky is a false denial
    "nothing-checked",        # no case survived to be checked at all
    "min-fraction",           # too few did, which is a green nobody should believe
    "check-never-ran",        # a case was checked and one of the three checks never ran
)


class Assertions:
    """Which assertions this run evaluated, and which fired.

    Evaluated is not fired and neither is inferable from the output: a check
    that never reaches its comparison is the failure mode a per-assertion
    selftest exists to catch, so both lists are printed.
    """

    def __init__(self):
        self.evaluated = set()
        self.fired = set()

    def judge(self, name, bad, message=None, into=None):
        assert name in ASSERTIONS, name
        self.evaluated.add(name)
        if bad:
            self.fired.add(name)
            if into is not None and message is not None:
                into.append(message)
        return bool(bad)

    def any_fired(self):
        return bool(self.fired)

    def report(self):
        print(f"assertions evaluated [{' '.join(sorted(self.evaluated))}] "
              f"fired [{' '.join(sorted(self.fired))}]")


def separation_arcsec(a, b):
    """Angle between two spherical directions, in arcseconds.

    Longitudes are differenced as an angle, never as a number: a degree of
    longitude is a degree of sky only on the equator, and a body at 5 degrees
    of latitude has bitten this project's cross-engine comparisons before.

    atan2(|a x b|, a.b), not acos(a.b).  The corrections this checks are
    milliarcsecond-scale, and the cosine of a milliarcsecond rounds to
    exactly 1.0 in f64 -- an acos form reports every one of them as a clean
    zero, which reads like a server doing nothing rather than a formula
    losing the answer.
    """
    lon1, lat1 = math.radians(a.lon_deg), math.radians(a.lat_deg)
    lon2, lat2 = math.radians(b.lon_deg), math.radians(b.lat_deg)
    u = (math.cos(lat1) * math.cos(lon1), math.cos(lat1) * math.sin(lon1), math.sin(lat1))
    v = (math.cos(lat2) * math.cos(lon2), math.cos(lat2) * math.sin(lon2), math.sin(lat2))
    dot = sum(p * q for p, q in zip(u, v))
    cross = (
        u[1] * v[2] - u[2] * v[1],
        u[2] * v[0] - u[0] * v[2],
        u[0] * v[1] - u[1] * v[0],
    )
    return math.degrees(math.atan2(math.sqrt(sum(c * c for c in cross)), dot)) * 3600.0


def ask(client, host, port, obj_args, observer_args, mask, jd, verbose):
    """One request: (answer, report, None), or (None, report, why) on error."""
    rep = wirelib.run(client, host, port,
                      ["--jd", str(jd), "--corrections", str(mask)] + obj_args + observer_args,
                      verbose, timeout=60)
    if rep.returncode != 0 or not rep.objects:
        return None, rep, rep.stderr or f"exit {rep.returncode}"
    meta, row = rep.objects[0], rep.row(0)
    if meta.err != 0 or row is None:
        return Answer(meta.name, meta.corr, meta.err, meta.err_text, 0.0, 0.0), rep, None
    return Answer(meta.name, meta.corr, 0, "", row[0], row[1]), rep, None


def declared_for(rep, observer_bit, kind):
    """Every correction bit this server declares for this (observer, kind).

    A.3 0x0004 is per observer; 0x0014 widens or narrows it per object kind,
    and this read only 0x0004 until 2026-09-20. Against a server that
    declares deflection and aberration for an orbit point seen from the Sun
    or the barycentre through 0x0014 alone -- which Astrolog's does -- the
    check reported its honest corrApplied as a claim it had never made. Two
    FAILs against a server doing exactly what it advertised: the third time
    in two days that the instrument was wrong and the program was fine.
    """
    allowed = 0
    for observers, mask in rep.corrmasks:
        if observers & (1 << observer_bit):
            allowed |= mask
    for observers, kinds, mask in rep.corrkinds:
        if observers & (1 << observer_bit) and kinds & (1 << kind):
            allowed |= mask
    return allowed


class Case:
    def __init__(self, label, obj_args, observer, observer_args, kind=0):
        self.label = label
        self.obj_args = obj_args
        self.observer = observer
        self.observer_args = observer_args
        self.kind = kind  # A.12, for the 0x0014 declarations


def cases():
    """Objects and observers chosen for where corrApplied can be wrong.

    A star has no light time to apply; the Sun's own light cannot be
    deflected by the Sun; at the Sun's centre nothing can; at the barycentre
    aberration runs and contributes ~0, which is the case that separates
    'did not apply' from 'applied, and it was nothing'.  An orbit point takes
    all three as conventions (3.5a) and is where the two engines' accounts of
    themselves legitimately differ.
    """
    out = []
    for obs, obs_args in (
        ("geo", []),
        ("topo", ["--topo", "0,51.48,0"]),
        ("helio", ["--helio"]),
        ("bary", ["--bary"]),
        ("body", ["--center", "5"]),
    ):
        for label, obj_args, kind in (
            ("Jupiter", ["--obj", "5"], 0),
            ("Saturn", ["--obj", "6"], 0),
            ("Sun", ["--obj", "10"], 0),
            ("Moon", ["--obj", "301"], 0),
            ("Sirius", ["--star", "Sirius"], 2),
            ("Jupiter asc node", ["--node", "5.a"], 1),
        ):
            # An observer at Jupiter's centre cannot look at Jupiter.
            if obs == "body" and obj_args == ["--obj", "5"]:
                continue
            out.append(Case(f"{label} [{obs}]", obj_args, obs, obs_args, kind))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=47190)
    ap.add_argument("--client", default="build/prometheia-wire-client")
    ap.add_argument("--jd", type=float, default=2451545.0, help="TT instant to probe (default J2000.0)")
    ap.add_argument("--min-fraction", type=float, default=0.5,
                    help="fail unless at least this fraction of cases is checked (default 0.5)")
    ap.add_argument("-v", "--verbose", action="store_true", help="echo each request")
    ap.add_argument("--list-assertions", action="store_true",
                    help="print the assertions this tool makes, one per line, and exit")
    args = ap.parse_args()

    if args.list_assertions:
        for name in ASSERTIONS:
            print(name)
        return 0

    asserts = Assertions()
    client = shutil.which(args.client) or args.client
    fails, notes, skipped, inapplicable = [], [], [], []
    runs = {"independence": 0, "declared": 0, "truthful": 0}
    checked = 0
    all_cases = cases()

    print(f"corrApplied against live behaviour: {args.host}:{args.port}, JD {args.jd} TT")

    # WELCOME first: the masks this server honours, per observer. Any request
    # carries the WELCOME back, whether or not its REQUEST is answered.
    _, welcome, _ = ask(client, args.host, args.port, ["--obj", "10"], [], 7, args.jd,
                        args.verbose)
    if asserts.judge("welcome-no-corrmasks", not welcome.corrmasks):
        print("\nFAIL: WELCOME advertises no correction masks (A.3 0x0004); nothing can be asked")
        asserts.report()
        return 1

    for case in all_cases:
        obs = OBS_BIT[case.observer]
        masks = [m for m in (0, LIGHT_TIME, DEFLECTION, ABERRATION, 7)
                 if welcome.permitted(obs, case.kind, m)]
        if not masks:
            inapplicable.append(f"{case.label}: WELCOME honours none of masks 0, 1, 2, 4, 7 here")
            continue
        answers = {}
        broken = None
        for mask in masks:
            answer, _, why = ask(
                client, args.host, args.port, case.obj_args, case.observer_args, mask,
                args.jd, args.verbose,
            )
            if answer is None:
                broken = why
                break
            if answer.err != 0:
                broken = f"errCode {answer.err}: {answer.err_text}"
                break
            answers[mask] = answer
        if broken is not None:
            skipped.append(f"{case.label}: {broken}")
            continue

        checked += 1
        first = masks[0]
        claimed = answers[first].corr_applied

        # independence: the slot is availability, not an echo of the ask.
        if len(answers) >= 2:
            runs["independence"] += 1
            varying = {m: a.corr_applied for m, a in answers.items() if a.corr_applied != claimed}
            shown = ", ".join(f"mask {m} -> {c}" for m, c in sorted(varying.items()))
            asserts.judge(
                "independence", varying,
                f"{case.label}: corrApplied tracks the request "
                f"(mask {first} -> {claimed}, but {shown}); 3.4 says it is structural",
                fails)
        else:
            inapplicable.append(f"{case.label}: independence needs two honoured masks")

        # declared: corrApplied against WELCOME's own advertisement. Every
        # answer, not just the first -- a server that echoes the request
        # reports 0 on mask 0, and 0 is a subset of anything.
        runs["declared"] += 1
        allowed = declared_for(welcome, obs, case.kind)
        over = 0
        for answer in answers.values():
            over |= answer.corr_applied & ~allowed
        named = ", ".join(
            BIT_NAME[b] for b in (LIGHT_TIME, DEFLECTION, ABERRATION) if over & b
        )
        asserts.judge(
            "declared", over,
            f"{case.label}: corrApplied claims {named}, which no mask WELCOME "
            f"honours for this observer and kind carries",
            fails)

        # truthful: a clear bit must not move the sky. Needs mask 0 and the
        # bit asked for alone, both honoured.
        for bit in (LIGHT_TIME, DEFLECTION, ABERRATION):
            if 0 not in answers or bit not in answers:
                inapplicable.append(
                    f"{case.label}: truthful for {BIT_NAME[bit]} needs masks 0 and {bit}, "
                    f"not both honoured"
                )
                continue
            runs["truthful"] += 1
            moved = separation_arcsec(answers[0], answers[bit])
            has = bool(claimed & bit)
            false_denial = asserts.judge(
                "truthful", not has and moved > MOVED_ARCSEC,
                f"{case.label}: corrApplied says no {BIT_NAME[bit]}, but asking for it "
                f"alone moves the position {moved:.4f}\"",
                fails)
            if false_denial:
                continue
            if has and moved < STILL_ARCSEC:
                notes.append(
                    f"{case.label}: {BIT_NAME[bit]} is reported applied and contributes "
                    f"{moved:.2e}\" -- allowed by 3.4 (the model ran, it was nothing)"
                )
            elif not has:
                notes.append(
                    f"{case.label}: {BIT_NAME[bit]} not applied, and asking for it moves "
                    f"{moved:.2e}\" -- consistent"
                )

    total = len(all_cases)
    print(f"\ncoverage: {checked} of {total} case(s) checked, {len(skipped)} skipped; "
          f"checks run: independence {runs['independence']}, declared {runs['declared']}, "
          f"truthful {runs['truthful']}; {len(inapplicable)} inapplicable")
    if skipped:
        print("\nskipped (the server could not answer; not a corrApplied verdict):")
        for s in skipped:
            print(f"  - {s}")
    if inapplicable and args.verbose:
        print("\ninapplicable (the server does not honour the masks they need):")
        for s in inapplicable:
            print(f"  - {s}")
    elif inapplicable:
        print(f"{len(inapplicable)} inapplicable; -v to show them")
    if notes and args.verbose:
        print("\nnotes:")
        for n in notes:
            print(f"  - {n}")
    elif notes:
        print(f"{len(notes)} note(s); -v to show them")

    # Checking nothing is not a pass.
    problems = []
    if not asserts.judge("nothing-checked", checked == 0,
                         f"NOTHING CHECKED: 0 of {total} cases", problems):
        asserts.judge("min-fraction", checked < args.min_fraction * total,
                      f"only {checked} of {total} cases checked "
                      f"(--min-fraction {args.min_fraction})", problems)
    for name, n in runs.items():
        if checked:
            asserts.judge("check-never-ran", n == 0,
                          f"the {name} check never ran", problems)
    if problems or fails:
        print("\nFAIL:")
        for f in problems + fails:
            print(f"  - {f}")
    else:
        print("\nOK: corrApplied matches this server's own behaviour and its own WELCOME")
    asserts.report()
    return 1 if asserts.any_fired() else 0


if __name__ == "__main__":
    sys.exit(main())

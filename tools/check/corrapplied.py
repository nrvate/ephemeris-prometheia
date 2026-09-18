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

  declared      corrApplied is a subset of the correction mask WELCOME
                advertised for that observer (A.3 0x0004).  Two things the
                same server said, disagreeing.

  truthful      asking for exactly one correction either moves the position
                or does not.  A clear bit that moves the sky is a false
                denial.  A set bit that moves nothing is fine and is
                reported as a note, because that is what a term
                contributing ~0 looks like (aberration at the barycentre).

Only a clear movement accuses.  Anything under the noise ceiling is a note,
never a failure: this harness is pointed at other people's servers, so it
stays quiet unless it is sure.

Usage:
    tools/check/corrapplied.py [--host H] [--port N] [--client PATH] [-v]
"""

import argparse
import math
import re
import shutil
import subprocess
import sys

LIGHT_TIME, DEFLECTION, ABERRATION = 1, 2, 4
BIT_NAME = {LIGHT_TIME: "light time", DEFLECTION: "deflection", ABERRATION: "aberration"}
OBS_BIT = {"geo": 0, "topo": 1, "helio": 2, "bary": 3, "body": 4}

# A movement this size is real and the server must own it; one under the
# floor is indistinguishable from f64 noise in a degree-valued column.
MOVED_ARCSEC = 1e-3
STILL_ARCSEC = 1e-5

META_RE = re.compile(r'^# object (\d+) name "(.*)" rowsOk (-?\d+) corr (\d+) err (\d+) "(.*)"$')
CORRMASK_RE = re.compile(r"^# corrmask observers (\d+) corrections (\d+)$")
ROW_RE = re.compile(r"^(\d+) (\d+) (.+)$")


class Answer:
    """One object's answer: its META claim and its first row."""

    def __init__(self, name, corr_applied, err, err_text, lon_deg, lat_deg):
        self.name = name
        self.corr_applied = corr_applied
        self.err = err
        self.err_text = err_text
        self.lon_deg = lon_deg
        self.lat_deg = lat_deg


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
    """One request. Returns (answer, corrmasks) or (None, corrmasks) on error."""
    cmd = [client, "--host", host, "--port", str(port), "--jd", str(jd), "--corrections", str(mask)]
    cmd += obj_args + observer_args
    if verbose:
        print("    $ " + " ".join(cmd), file=sys.stderr)
    run = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    corrmasks = []
    meta = None
    row = None
    for line in run.stdout.splitlines():
        m = CORRMASK_RE.match(line)
        if m:
            corrmasks.append((int(m.group(1)), int(m.group(2))))
            continue
        m = META_RE.match(line)
        if m:
            meta = m
            continue
        m = ROW_RE.match(line)
        if m and row is None:
            row = [float(v) for v in m.group(3).split()]
    if run.returncode != 0 or meta is None:
        why = run.stderr.strip() or f"exit {run.returncode}"
        return None, corrmasks, why
    err = int(meta.group(5))
    if err != 0 or row is None:
        return (
            Answer(meta.group(2), int(meta.group(4)), err, meta.group(6), 0.0, 0.0),
            corrmasks,
            None,
        )
    return (
        Answer(meta.group(2), int(meta.group(4)), 0, "", row[0], row[1]),
        corrmasks,
        None,
    )


def declared_for(corrmasks, observer_bit):
    """The union of the masks WELCOME advertised for this observer."""
    allowed = 0
    stated = False
    for observers, mask in corrmasks:
        if observers & (1 << observer_bit):
            allowed |= mask
            stated = True
    return allowed if stated else None


class Case:
    def __init__(self, label, obj_args, observer, observer_args):
        self.label = label
        self.obj_args = obj_args
        self.observer = observer
        self.observer_args = observer_args


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
        for label, obj_args in (
            ("Jupiter", ["--obj", "5"]),
            ("Saturn", ["--obj", "6"]),
            ("Sun", ["--obj", "10"]),
            ("Moon", ["--obj", "301"]),
            ("Sirius", ["--star", "Sirius"]),
            ("Jupiter asc node", ["--node", "5.a"]),
        ):
            # An observer at Jupiter's centre cannot look at Jupiter.
            if obs == "body" and obj_args == ["--obj", "5"]:
                continue
            out.append(Case(f"{label} [{obs}]", obj_args, obs, obs_args))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=47190)
    ap.add_argument("--client", default="build/prometheia-wire-client")
    ap.add_argument("--jd", type=float, default=2451545.0, help="TT instant to probe (default J2000.0)")
    ap.add_argument("-v", "--verbose", action="store_true", help="echo each request")
    args = ap.parse_args()

    client = shutil.which(args.client) or args.client
    fails, notes, skipped, checked = [], [], [], 0

    print(f"corrApplied against live behaviour: {args.host}:{args.port}, JD {args.jd} TT")

    for case in cases():
        answers = {}
        broken = None
        corrmasks = []
        for mask in (0, LIGHT_TIME, DEFLECTION, ABERRATION, 7):
            answer, marks, why = ask(
                client, args.host, args.port, case.obj_args, case.observer_args, mask,
                args.jd, args.verbose,
            )
            if marks:
                corrmasks = marks
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
        claimed = answers[0].corr_applied

        # independence: the slot is availability, not an echo of the ask.
        varying = {m: a.corr_applied for m, a in answers.items() if a.corr_applied != claimed}
        if varying:
            shown = ", ".join(f"mask {m} -> {c}" for m, c in sorted(varying.items()))
            fails.append(
                f"{case.label}: corrApplied tracks the request "
                f"(mask 0 -> {claimed}, but {shown}); 3.4 says it is structural"
            )

        # declared: corrApplied against WELCOME's own advertisement. Every
        # answer, not just the mask-0 one -- a server that echoes the request
        # reports 0 there, and 0 is a subset of anything.
        allowed = declared_for(corrmasks, OBS_BIT[case.observer])
        if allowed is not None:
            over = 0
            for answer in answers.values():
                over |= answer.corr_applied & ~allowed
            if over:
                named = ", ".join(
                    BIT_NAME[b] for b in (LIGHT_TIME, DEFLECTION, ABERRATION) if over & b
                )
                fails.append(
                    f"{case.label}: corrApplied claims {named}, which WELCOME's "
                    f"corrMasks ({allowed}) does not advertise for this observer"
                )

        # truthful: a clear bit must not move the sky.
        for bit in (LIGHT_TIME, DEFLECTION, ABERRATION):
            moved = separation_arcsec(answers[0], answers[bit])
            has = bool(claimed & bit)
            if not has and moved > MOVED_ARCSEC:
                fails.append(
                    f"{case.label}: corrApplied says no {BIT_NAME[bit]}, but asking for it "
                    f"alone moves the position {moved:.4f}\""
                )
            elif has and moved < STILL_ARCSEC:
                notes.append(
                    f"{case.label}: {BIT_NAME[bit]} is reported applied and contributes "
                    f"{moved:.2e}\" -- allowed by 3.4 (the model ran, it was nothing)"
                )
            elif not has:
                notes.append(
                    f"{case.label}: {BIT_NAME[bit]} not applied, and asking for it moves "
                    f"{moved:.2e}\" -- consistent"
                )

    print(f"\n{checked} case(s) checked, {len(skipped)} skipped")
    if skipped:
        print("\nskipped (the server could not answer; not a corrApplied verdict):")
        for s in skipped:
            print(f"  - {s}")
    if notes and args.verbose:
        print("\nnotes:")
        for n in notes:
            print(f"  - {n}")
    elif notes:
        print(f"{len(notes)} note(s); -v to show them")
    if fails:
        print("\nFAIL:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("\nOK: corrApplied matches this server's own behaviour and its own WELCOME")
    return 0


if __name__ == "__main__":
    sys.exit(main())

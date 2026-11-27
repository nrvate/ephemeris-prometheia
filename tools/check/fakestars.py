#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A scripted stand-in for build/prometheia-wire-client, for starstest.py.

stars_fk5.py asks a server for star directions and compares them with the FK5.
To grade its assertions, something has to answer -- and a real daemon is the
wrong thing to answer with: it takes an ephemeris, a port and a build, and its
answers move when the star catalogue does, which is exactly the input each
case here mutates.

This answers from the **pristine** FK5 (`--raw-dir`, the real stars-raw/),
never from the copy the case has mutated, so a case that shifts a catalogue
record shifts the reference alone and the separation opens.  For the stars
whose orbit the engine carries it adds the orbit offset that stars_fk5.py
subtracts, so the control case compares a barycentre with a barycentre.

`--silent` answers nothing at all: a server that accepted the request and
produced no row, which is what `unanswered` is about.

**What this cannot see:** whether `reference()` reads the FK5's columns
correctly.  It uses that same function, so a wrong column would move both
sides and the control case would stay green.  That question belongs to the
live run against a real server; this grades the assertions.
"""
import argparse
import math
import os
import sys
import warnings

import erfa

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import binary_orbits  # noqa: E402
import stars_fk5  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--raw-dir", default=os.path.join(stars_fk5.ROOT, "stars-raw"))
    ap.add_argument("--silent", action="store_true", help="answer no rows at all")
    ap.add_argument("--jd", type=float, default=2451545.0)
    ap.add_argument("--star", action="append", default=[])
    # Accepted and ignored: the frame flags stars_fk5.py passes through.
    for flag in ("--bary", "--icrs", "--eq", "--no-corrections"):
        ap.add_argument(flag, action="store_true")
    ap.add_argument("--port", default="")
    a = ap.parse_args()
    if a.silent:
        return 0

    hr_of, hip_of, fk5_of_hr, fk5 = stars_fk5.load(a.raw_dir)
    orbits = binary_orbits.load(a.raw_dir)
    for k, name in enumerate(a.star):
        n = fk5_of_hr.get(hr_of.get(name, 0))
        if n is None or n not in fk5:
            continue
        star, _ = stars_fk5.reference(fk5[n])
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", erfa.ErfaWarning)
            p = erfa.pmsafe(*star, 2451545.0, 0.0, a.jd, 0.0)
        ra, dec = math.degrees(p[0]) % 360.0, math.degrees(p[1])
        orbit = orbits.get(hip_of.get(name, 0))
        if orbit:
            # A server answers with the star, not the barycentre; stars_fk5.py
            # takes this back off before comparing.
            de, dn = binary_orbits.offset(orbit, a.jd)
            ra += de / 3600.0 / math.cos(math.radians(dec))
            dec += dn / 3600.0
        print(f"{k} 0 {ra:.9f} {dec:.9f} 1.000000000 0.0 0.0 0.0")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The same calls through pyswisseph and through prometheia.swe (docs/PYTHON.md).

pyswisseph is an output oracle only, as swetest is: installed in the oracle
venv, called, never read. It runs on the same JPL DE440 file (FLG_JPLEPH), so
what remains is the two engines' own differences, which docs/VALIDATION.md
and docs/HOUSES.md explain. Prints the worst difference per area and exits 0;
it grades nothing, and the numbers go into docs/PYTHON.md.

Usage:
  .venv-oracle/bin/pip install pyswisseph
  PYTHONPATH=python .venv-oracle/bin/python tools/check/pyswe_oracle.py [--ephe-dir ephe]
"""
import argparse
import os
import random
import sys

import swisseph as sw

import prometheia
import prometheia.swe as pr

JDS = [2378496.71, 2415020.13, 2440587.9, 2451545.0, 2461300.42, 2488069.66]  # UT1, 1800-2100
BODIES = [pr.SUN, pr.MOON, pr.MERCURY, pr.VENUS, pr.MARS, pr.JUPITER, pr.SATURN, pr.URANUS,
          pr.NEPTUNE, pr.PLUTO, pr.MEAN_NODE, pr.TRUE_NODE, pr.MEAN_APOG, pr.OSCU_APOG]


def ang(a, b):
    return abs((a - b + 180.0) % 360.0 - 180.0)


class Worst:
    def __init__(self):
        self.rows = {}

    def add(self, area, value, where):
        if area not in self.rows or value > self.rows[area][0]:
            self.rows[area] = (value, where)

    def show(self):
        for area, (value, where) in self.rows.items():
            print("%-44s %14.6g   at %s" % (area, value, where))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ephe-dir", default="ephe")
    args = ap.parse_args()
    de440 = os.path.join(args.ephe_dir, "linux_p1550p2650.440")
    sw.set_ephe_path(args.ephe_dir)
    sw.set_jpl_file("linux_p1550p2650.440")
    prometheia.set_ephe_path(de440)
    w = Worst()
    J = sw.FLG_JPLEPH

    flagsets = [
        ("apparent, ecliptic of date", J | sw.FLG_SPEED),
        ("equatorial", J | sw.FLG_SPEED | sw.FLG_EQUATORIAL),
        ("J2000, no nutation", J | sw.FLG_SPEED | sw.FLG_J2000 | sw.FLG_NONUT),
        ("heliocentric", J | sw.FLG_SPEED | sw.FLG_HELCTR),
        ("true position (geometric)", J | sw.FLG_SPEED | sw.FLG_TRUEPOS | sw.FLG_NOABERR |
         sw.FLG_NOGDEFL),
        ("topocentric (Zurich)", J | sw.FLG_SPEED | sw.FLG_TOPOCTR),
        ("sidereal Lahiri", J | sw.FLG_SPEED | sw.FLG_SIDEREAL),
    ]
    sw.set_topo(8.55, 47.37, 500.0)
    pr.set_topo(8.55, 47.37, 500.0)
    sw.set_sid_mode(sw.SIDM_LAHIRI)
    pr.set_sid_mode(pr.SIDM_LAHIRI)
    # calc() at TT isolates the engines; calc_ut() adds each side's delta T,
    # which differ by 110 s at 2100 (predicted) and so move the Moon a minute
    # of arc there. calc_ut() is compared where delta T is observed.
    # A topocentric place turns with the Earth, which takes UT1, so it too
    # carries delta T: compared where delta T is observed.
    cases = [(label, flags, jd, False) for label, flags in flagsets
             for jd in (JDS[1:5] if flags & sw.FLG_TOPOCTR else JDS)]
    cases += [("UT, observed delta T (1900-2026): " + label, flags, jd, True)
              for label, flags in flagsets[:1] for jd in JDS[1:5]]
    for label, flags, jd, ut in cases:
            for body in BODIES:
                if flags & sw.FLG_HELCTR and body in (pr.SUN, pr.MOON) or body >= pr.MEAN_NODE and (
                        flags & (sw.FLG_HELCTR | sw.FLG_TOPOCTR)):
                    continue
                try:
                    a, _ = (sw.calc_ut if ut else sw.calc)(jd, body, flags)
                except sw.Error:
                    continue
                b, ret = (pr.calc_ut if ut else pr.calc)(jd, body, flags)
                name = sw.get_planet_name(body)
                kind = "Sun, Moon, planets" if body <= pr.PLUTO else "lunar points"
                w.add("%s: %s position (arcsec)" % (label, kind),
                      max(ang(a[0], b[0]), abs(a[1] - b[1])) * 3600, (name, jd))
                if body not in (pr.MEAN_APOG, pr.OSCU_APOG, pr.MEAN_NODE, pr.TRUE_NODE):
                    w.add(label + ": distance (relative)", abs(a[2] - b[2]) / a[2], (name, jd))
                w.add(label + ": lon speed (arcsec/day)", abs(a[3] - b[3]) * 3600, (name, jd))
                w.add(label + ": lat speed (arcsec/day)", abs(a[4] - b[4]) * 3600, (name, jd))
                if ret != flags & ~(sw.FLG_SWIEPH | sw.FLG_MOSEPH) | sw.FLG_JPLEPH:
                    w.add(label + ": retflags differ (count)", 1, (name, jd))

    # Each side's reported rate against the derivative of its own positions
    # (central difference, h = 1e-3 day): which rate is the positions' own.
    for label, flags in (flagsets[0], flagsets[5]):
        for jd in JDS[1:5]:
            for body in (pr.SUN, pr.MOON, pr.MERCURY, pr.MARS):
                for name, m in (("pyswisseph", sw), ("prometheia", pr)):
                    rep = m.calc(jd, body, flags)[0]
                    lo = m.calc(jd - 1e-3, body, flags & ~sw.FLG_SPEED)[0]
                    hi = m.calc(jd + 1e-3, body, flags & ~sw.FLG_SPEED)[0]
                    num = [((hi[k] - lo[k] + 180.0) % 360.0 - 180.0 if k == 0 else hi[k] - lo[k])
                           / 2e-3 for k in (0, 1)]
                    w.add("%s rates vs own derivative, %s (arcsec/day)" % (name, label),
                          max(abs(rep[3] - num[0]), abs(rep[4] - num[1])) * 3600,
                          (sw.get_planet_name(body), jd))

    for jd in JDS:
        for h in "PKORCAWBMXT":
            a = sw.houses(jd, 47.37, 8.55, h.encode())
            b = pr.houses(jd, 47.37, 8.55, h.encode())
            d = max(max(ang(x, y) for x, y in zip(a[0], b[0])),
                    max(ang(x, y) for x, y in zip(a[1][:5], b[1][:5])))
            w.add("houses at 47.37N (arcsec)", d * 3600, (h, jd))
        w.add("sidereal time (s of time)", abs(sw.sidtime(jd) - pr.sidtime(jd)) * 3600, jd)
        w.add("mean ayanamsa, Lahiri (arcsec)",
              abs(sw.get_ayanamsa_ut(jd) - pr.get_ayanamsa_ut(jd)) * 3600, jd)
        w.add("true ayanamsa, Lahiri (arcsec)",
              abs(sw.get_ayanamsa_ex_ut(jd, J)[1] - pr.get_ayanamsa_ex_ut(jd, J)[1]) * 3600, jd)
        w.add("delta T (s)", abs(sw.deltat(jd) - pr.deltat(jd)) * 86400, jd)

    for y, m, d, h in [(-4712, 1, 1, 12.0), (1582, 10, 15, 0.0), (2000, 1, 1, 12.0),
                       (2026, 9, 29, 21.9), (-500, 3, 1, 6.5)]:
        for cal in (sw.GREG_CAL, sw.JUL_CAL):
            a, b = sw.julday(y, m, d, h, cal), pr.julday(y, m, d, h, cal)
            w.add("julday (days)", abs(a - b), (y, m, d, cal))
            ra, rb = sw.revjul(a, cal), pr.revjul(a, cal)
            w.add("revjul (hours, date must match)",
                  abs(ra[3] - rb[3]) + (0 if ra[:3] == rb[:3] else 1e9), (y, m, d, cal))
    for stamp in [(1990, 6, 15, 12, 30, 0.0), (2016, 12, 31, 23, 59, 60.0), (2024, 2, 29, 0, 0, 0.5)]:
        a, b = sw.utc_to_jd(*stamp, sw.GREG_CAL), pr.utc_to_jd(*stamp, pr.GREG_CAL)
        w.add("utc_to_jd TT (s)", abs(a[0] - b[0]) * 86400, stamp)
        w.add("utc_to_jd UT1 (s)", abs(a[1] - b[1]) * 86400, stamp)
    for jd in [2451545.0, 2457754.5, 2461300.42]:
        a, b = sw.jdut1_to_utc(jd), pr.jdut1_to_utc(jd)
        w.add("jdut1_to_utc (s, date must match)",
              abs(a[5] - b[5]) + (0 if a[:5] == b[:5] else 1e9), jd)
    # split_deg over every flag combination, at chosen and random values.
    rnd = random.Random(7)
    xs = [0.0, 12.99999, 29.99999, 123.456789, 359.9999999, -12.5, -200.25, 13.3333333,
          26.6666, 45.999999, 13.333333333, -0.0000001, 89.99999999, 719.5]
    xs += [rnd.uniform(-720, 720) for _ in range(300)]
    xs += [rnd.randrange(-360, 360) + rnd.choice([0.9999999, 0.99999, 0.999, 0.5, 0.49999])
           for _ in range(300)]
    mismatches = total = 0
    for x in xs:
        for flag in list(range(64)) + [f | 1024 for f in range(64)]:
            a, b = sw.split_deg(x, flag), pr.split_deg(x, flag)
            total += 1
            if not (a[:3] == b[:3] and a[4] == b[4] and abs(a[3] - b[3]) < 1e-6):
                mismatches += 1
                w.add("split_deg first mismatch", 1, (x, flag, a, b))
    w.add("split_deg mismatches (of %d)" % total, mismatches, "")
    for c in [(10.0, 5.0, 1.0), (200.0, -30.0, 2.0)]:
        a, b = sw.cotrans(c, -23.44), pr.cotrans(c, -23.44)
        w.add("cotrans (arcsec)", max(ang(a[0], b[0]), abs(a[1] - b[1])) * 3600, c)
    w.show()
    prometheia.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

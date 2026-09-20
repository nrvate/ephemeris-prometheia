#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fixed stars against an outside catalogue: the FK5 (docs/STARS.md, "Checked
against FK5").

Both v4 servers take their stars from Hipparcos-derived catalogues, so the
cross-test's stars leg agreeing to milliarcseconds says nothing about an error
they share. The Basic FK5 (Fricke et al. 1988, CDS I/149A) is ground-based and
pre-Hipparcos. This asks each server for the stars leg's stars as barycentric
directions (ICRS, equatorial, no corrections) at 1900, 2000 and 2100 and
compares them with the FK5 entry of the same star, carried into the Hipparcos
frame and to the epoch by ERFA's fk52h and pmsafe (pyerfa, an output oracle
only, as in tools/gen/gen_star_fixtures.py).

Stars are matched by the Bright Star Catalogue's FK5 number, through the HR
number in src/star_catalog.inc, so a star the BSC gives no FK5 number (Castor,
HR 2891) is reported as absent rather than compared with the other component.

The band is an estimate, not a measurement: the FK5's stated mean errors leave
out its system errors (its ReadMe says so), and the Hipparcos proper motions of
astrometric binaries carry a few years of orbital motion, which a century
multiplies. BAND covers the first; BINARIES names the second, each with the
reason, and prints its separation without holding it to BAND. The check is
meant to catch a wrong star, a wrong proper motion or a wrong epoch, which are
arcseconds to arcminutes; it cannot referee milliarcseconds.

Needs: stars-raw/ with the fk5 and bsc5 sources (tools/fetch/stars_fetch.py
--only fk5 --only bsc5), pyerfa (tools/requirements-oracle.txt: .venv-oracle), build/prometheia-wire-client and one or
more servers.

Exit status is exactly "did any assertion fire"; `--list-assertions` prints
them and tools/check/starstest.py drives one mutated catalogue per
assertion.  Two of them are not about the sky at all.  A run that compared
*no* star -- a truncated `stars-raw/` file, a catalogue whose columns moved
-- used to print "all within band" and exit 0, and a star the server did not
answer for was counted as DIFFERS, which accuses another project's server of
disagreeing with the FK5 when it said nothing.

Usage:
  stars_fk5.py                                   # ours on 47190
  stars_fk5.py --server ours=47190 --server theirs=47391
"""
import argparse
import gzip
import math
import os
import re
import subprocess
import sys
import warnings

import erfa

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assertlib  # noqa: E402
import binary_orbits  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# The cross-test's stars leg (tools/check/crosstest.py, STARS).
STARS = ["Aldebaran", "Regulus", "Spica", "Antares", "Fomalhaut", "Sirius", "Algol", "Vega",
         "Polaris", "Betelgeuse", "Rigel", "Arcturus", "Canopus", "Achernar", "Deneb", "Altair",
         "Pollux", "Castor", "Procyon", "Capella", "Alcyone", "Zubenelgenubi", "Zubeneschamali",
         "Bellatrix", "Acrux", "Hadar", "Mirach", "Alphecca", "Scheat"]
EPOCHS = [2415020.5, 2451545.0, 2488069.5]  # 1900, 2000, 2100 (TT)

# Checked here but not in the stars leg: alpha Cen A, whose FK5 entry is the
# referee for its barycentre (below).
EXTRA = ["Rigil Kentaurus"]

BAND = 1.0  # arcsec; an estimate (see the docstring)

# Stars this check knowingly cannot compare, with the reason. A fixed list,
# so the skip is a decision and not a silence: any *other* star that falls
# out of the comparison fires `star-without-fk5`, and a star here that turns
# out to have an entry after all fires `expected-absence-is-present`. Both
# directions, because a list of exceptions rots in both.
NO_FK5 = {
    "Castor": "src/star_catalog.inc names HR 2891, which the BSC gives no FK5 "
              "number; FK5 287 is HR 2890, the fainter component, and comparing one "
              "with the other would be worse than not comparing at all",
}

ASSERTIONS = (
    "nothing-compared",             # no star survived to be compared at all
    "star-without-fk5",             # a star fell out that NO_FK5 does not name
    "expected-absence-is-present",  # a NO_FK5 star does have an entry: the list has rotted
    "unanswered",                   # a server did not answer a star at an epoch
    "differs",                      # a separation outside the band
)
BINARIES = {
    # Separation allowed, and why. Each has a companion whose orbit moves the
    # bright star's photocentre.
    "Polaris": (3.0, "astrometric binary (Polaris Ab), no orbit applied"),
    "Achernar": (3.0, "binary (Achernar B), no orbit applied"),
    # These carry their orbit in the engine (binary_orbits.py). The FK5 fits a
    # straight line to two centuries of the star wobbling about its
    # barycentre, which finds the barycentre, so what is compared is ours: the
    # server's star less the orbit's offset, computed here independently. What
    # remains is the two catalogues' barycentric proper motions.
    "Sirius": (3.0, "barycentre compared (Sirius B's orbit applied)"),
    "Procyon": (3.0, "barycentre compared (Procyon B's orbit applied)"),
    "Rigil Kentaurus": (3.0, "barycentre compared (alpha Cen B's orbit applied)"),
}


def unit(ra_deg, de_deg):
    a, d = math.radians(ra_deg), math.radians(de_deg)
    return (math.cos(d) * math.cos(a), math.cos(d) * math.sin(a), math.sin(d))


def sep_arcsec(u, v):
    # atan2(|u x v|, u.v): exact at small angles, unlike acos.
    x = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    return math.degrees(math.atan2(math.sqrt(sum(t * t for t in x)),
                                   sum(s * t for s, t in zip(u, v)))) * 3600


def load(raw):
    hr_of, hip_of = {}, {}
    pat = re.compile(r'\s*\{(\d+), \d+, (\d+), .*"([^"]*)"\},$')
    with open(os.path.join(ROOT, "src", "star_catalog.inc")) as f:
        for line in f:
            m = pat.match(line)
            if m:
                for n in m.group(3).split("|"):
                    if n:
                        hr_of.setdefault(n, int(m.group(1)))
                        hip_of.setdefault(n, int(m.group(2)))
    fk5_of_hr = {}
    with gzip.open(os.path.join(raw, "V_50_catalog.gz"), "rt", encoding="latin-1") as f:
        for l in f:
            if l[37:41].strip():
                fk5_of_hr[int(l[0:4])] = int(l[37:41])
    fk5 = {}
    with gzip.open(os.path.join(raw, "I_149A_catalog.gz"), "rt") as f:
        for l in f:
            fk5[int(l[0:4])] = l
    return hr_of, hip_of, fk5_of_hr, fk5


def reference(l):
    """An FK5 line -> (ERFA Hipparcos-frame star data at J2000, error model)."""
    ra = (int(l[5:7]) + int(l[8:10]) / 60 + float(l[11:17]) / 3600) * 15
    de = (int(l[27:29]) + int(l[30:32]) / 60 + float(l[33:38]) / 3600) * (-1 if l[26] == "-" else 1)
    pmra = float(l[18:25]) * 15 / 3600 / 100  # s of RA per century -> deg per year
    pmde = float(l[39:46]) / 3600 / 100
    px = float(l[138:144]) if l[138:144].strip() else 0.0
    rv = float(l[146:152]) if l[146:152].strip() else 0.0
    if px <= 0:
        # ERFA caps the space motion of a star at zero parallax below the speed
        # of light, which destroys its proper motion. A nominal 1 mas with no
        # radial velocity leaves the perspective terms negligible over a century.
        px, rv = 0.001, 0.0
    star = erfa.fk52h(math.radians(ra), math.radians(de), math.radians(pmra), math.radians(pmde),
                      px, rv)
    err = dict(cosd=math.cos(math.radians(de)), eRA=float(l[95:99]), epmRA=float(l[100:105]),
               eDE=float(l[112:116]), epmDE=float(l[117:122]), epRA=1900 + float(l[89:94]),
               epDE=1900 + float(l[106:111]))
    return star, err


def sigma(err, jd):
    """The FK5's own mean error at an epoch (arcsec), system errors excluded."""
    yr = 2000 + (jd - 2451545.0) / 365.25
    sa = 15 * err["cosd"] * math.hypot(err["eRA"], (yr - err["epRA"]) / 100 * err["epmRA"]) / 1000
    sd = math.hypot(err["eDE"], (yr - err["epDE"]) / 100 * err["epmDE"]) / 100
    return math.hypot(sa, sd)


def ask(client, port, names, jd):
    args = [client, "--port", str(port), "--bary", "--icrs", "--eq", "--no-corrections",
            "--jd", repr(jd)]
    for n in names:
        args += ["--star", n]
    out = subprocess.run(args, capture_output=True, text=True).stdout
    rows = {}
    for line in out.splitlines():
        v = line.split()
        if len(v) >= 4 and v[0].isdigit() and v[1] == "0":
            rows[int(v[0])] = (float(v[2]), float(v[3]))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--raw-dir", default=os.path.join(ROOT, "stars-raw"))
    ap.add_argument("--client", default=os.path.join(ROOT, "build", "prometheia-wire-client"))
    ap.add_argument("--server", action="append", default=[],
                    help="NAME=PORT (repeatable; default ours=47190)")
    ap.add_argument("--list-assertions", action="store_true",
                    help="print the assertions this tool makes, one per line, and exit")
    a = ap.parse_args()
    if a.list_assertions:
        return assertlib.Assertions(ASSERTIONS).list_and_exit()

    asserts = assertlib.Assertions(ASSERTIONS)
    servers = [s.split("=", 1) for s in a.server] or [["ours", "47190"]]
    hr_of, hip_of, fk5_of_hr, fk5 = load(a.raw_dir)
    orbits = binary_orbits.load(a.raw_dir)

    refs = {}
    missing, unexpected = [], []
    for name in STARS + EXTRA:
        hr = hr_of.get(name)
        n = fk5_of_hr.get(hr) if hr else None
        if name in NO_FK5:
            # Never compared, whatever the catalogue holds: the list says not
            # to. What is checked is the list's own reason, so an entry
            # appearing for one of these is a fault in the list and not in
            # the star -- and the star staying out keeps that one fault from
            # also arriving as a wild separation.
            if n is not None and n in fk5:
                unexpected.append(name)
            print(f"{name}: not compared ({NO_FK5[name]})")
            continue
        if n is None or n not in fk5:
            missing.append(name)
            print(f"{name}: HR {hr} has no FK5 entry; not compared")
            continue
        refs[name] = (n, *reference(fk5[n]))
    names = list(refs)

    fails = []
    # Always evaluated: it cannot fire when nothing was found, so it does not
    # need the cascade below and should not hide inside it.
    asserts.judge("expected-absence-is-present", bool(unexpected),
                  "NO_FK5 names " + ", ".join(unexpected) + " as having no FK5 entry and "
                  "this catalogue has one; the exception list has rotted and the star is "
                  "going uncompared for a reason that is no longer true", fails)

    differs = 0
    for label, port in servers:
        print(f"\n== {label} (port {port}): separation from FK5, arcsec (FK5 sigma)")
        print(f"{'star':15s} {'FK5':>4s}  " + "  ".join(f"{y:>16s}" for y in ("1900", "2000", "2100")))
        answers = [ask(a.client, port, names, jd) for jd in EPOCHS]
        worst = 0.0
        for k, name in enumerate(names):
            n, star, err = refs[name]
            band, why = BINARIES.get(name, (BAND, ""))
            cells, bad, silent = [], False, []
            for jd, rows in zip(EPOCHS, answers):
                if k not in rows:
                    cells.append(f"{'unanswered':>16s}")
                    silent.append(jd)
                    continue
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore", erfa.ErfaWarning)
                    p = erfa.pmsafe(*star, 2451545.0, 0.0, jd, 0.0)
                ra, dec = rows[k]
                orbit = orbits.get(hip_of.get(name, 0))
                if orbit:
                    # The server's star less its orbit's offset: our barycentre.
                    de, dn = binary_orbits.offset(orbit, jd)
                    ra -= de / 3600.0 / math.cos(math.radians(dec))
                    dec -= dn / 3600.0
                s = sep_arcsec(unit(math.degrees(p[0]), math.degrees(p[1])), unit(ra, dec))
                bad |= s > band
                if not why:
                    worst = max(worst, s)
                cells.append(f"{s:7.3f} ({sigma(err, jd):.3f})")
            # Silence and disagreement are different accusations, and this
            # tool is pointed at another project's server. A star the server
            # never answered for used to be printed as DIFFERS, which says it
            # disagrees with the FK5 when it said nothing at all.
            asserts.judge("unanswered", bool(silent),
                          f"{label}: no answer for {name} at "
                          + ", ".join(f"JD {j}" for j in silent), fails)
            asserts.judge("differs", bad,
                          f"{label}: {name} is outside the {band}\" band", fails)
            differs += bad
            note = ("  DIFFERS" if bad else "") + ("  UNANSWERED" if silent else "")
            note += f"  [{why}]" if why else ""
            print(f"{name:15s} {n:4d}  " + "  ".join(cells) + note)
        print(f"worst single star: {worst:.3f}\" (band {BAND}\", an estimate)")

    print()
    # The cascade: a catalogue that yielded nothing would also report every
    # star as falling out, and two assertions firing on one input is how a
    # deleted check stays invisible. The specific truth first.
    if not asserts.judge(
            "nothing-compared", not names,
            f"not one of the {len(STARS) + len(EXTRA)} stars had an FK5 entry to compare "
            f"against. No separation was measured, so \"all within band\" would have been "
            f"a statement about nothing -- check {a.raw_dir} and the catalogue's columns",
            fails):
        asserts.judge("star-without-fk5", bool(missing),
                      "no FK5 entry for " + ", ".join(missing)
                      + ", which NO_FK5 does not name: either the match by BSC FK5 number "
                      "has drifted, or the exception belongs in NO_FK5 with its reason",
                      fails)

    for line in fails:
        print("FAIL  " + line)
    if not fails:
        print(f"all within band: {len(names)} stars x {len(EPOCHS)} epochs x "
              f"{len(servers)} server(s)")
    asserts.report()
    return 1 if asserts.any_fired() else 0


if __name__ == "__main__":
    sys.exit(main())

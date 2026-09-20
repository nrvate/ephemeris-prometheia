#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Does one v4 server's RATE columns describe its own positions, everywhere?

A server advertises a bound on that error in A.3 `0x0013`
(`ratesDegPerDay`, `ratesAuPerDay`); absent, the registry's default of
1e-5 deg/day and 1e-9 AU/day applies. This reads whichever is in force off
the wire and then tries to break it.

The point is breadth, and it is the one thing a server's own rate check
structurally cannot do for itself. A bound is only as wide as the object
list that measured it, and a harness written beside a server tends to
measure the objects its author was thinking about. The Astrolog project
advertised 3e-6 deg/day (its sweep was geocentric only), then 5e-3 (its
sweep had bodies and the Moon's points but no planetary apsides). Neither
figure was dishonest; both were the width of a list. So this sweeps every
kind the server will answer -- bodies, both nodes and both apses of six
bodies in both mean and osculating form, the natural apsides, and stars --
across observers, frames and epochs, and reports the worst it can find.

The oracle is the server's own positions: five rows at t-2h .. t+2h with
h = 1/1024 day (84.375 s, so every row time is exact in f64), differenced
five-point. There is no outside reference here and none is wanted: the
question is internal consistency, not correctness.

Usage:
    ratesweep.py [--server HOST:PORT] [--client PATH] [--epochs N] [-v]
    ratesweep.py --server 127.0.0.1:47392 --out sweep.tsv
    ratesweep.py --only Moon --configs 1 --epochs 1      (a quick probe)

Exit status is exactly "did any assertion below fire"; `--list-assertions`
prints them, and tools/check/ratestest.py drives one scripted server per
assertion.  Until 2026-09-20 the status was "did any row exceed the bound",
and `asked`/`answered` were printed and asserted by nothing: a sweep in
which every single object came back unanswered found no exceedances, said
"within its advertisement everywhere this sweep reached", and exited 0.  A
green that could not have been red, in the tool whose output is an
accusation against another project's server.
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assertlib  # noqa: E402
import wirelib  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# 1/1024 day. At h = 0.001 d the JD quantization alone (a JD near 2.45e6
# resolves ~40 us in f64) showed as 7e-7 deg/day on the Moon.
H_DAYS = 1.0 / 1024.0
# A.3: absent 0x0013, a server claims no more than this.
DEFAULT_DEG_PER_DAY = 1e-5
DEFAULT_AU_PER_DAY = 1e-9

# Well inside every server's coverage at the ends, since a refusal here says
# nothing about rates. 1800 is included because that is where Astrolog's
# files stop and refusals are informative about the edge, not the arithmetic.
EPOCHS = [2378496.5, 2415020.5, 2451545.0, 2461300.5, 2488069.5]

BODIES = [("--obj", "10", "Sun"), ("--obj", "301", "Moon"), ("--obj", "199", "Mercury"),
          ("--obj", "299", "Venus"), ("--obj", "399", "Earth"), ("--obj", "4", "Mars"),
          ("--obj", "5", "Jupiter"), ("--obj", "6", "Saturn"), ("--obj", "7", "Uranus"),
          ("--obj", "8", "Neptune"), ("--obj", "9", "Pluto")]

# Both nodes and both apses, mean and osculating, for the Moon and five
# planets; plus the Moon's natural (interpolated) apsides, method 2.
POINTS = []
for _naif, _name in (("301", "Moon"), ("199", "Mercury"), ("299", "Venus"), ("4", "Mars"),
                     ("5", "Jupiter"), ("6", "Saturn")):
    for _p, _pn in (("a", "asc node"), ("d", "desc node"), ("p", "perihelion"),
                    ("A", "aphelion")):
        for _m, _mn in (("m", "mean"), ("o", "osc")):
            POINTS.append(("--node", f"{_naif}.{_p}.{_m}", f"{_name} {_mn} {_pn}"))
POINTS += [("--node", "301.A.2", "Moon natural apogee"),
           ("--node", "301.p.2", "Moon natural perigee")]

# Aldebaran is here because the Astrolog side's own sweep found its worst
# distance rate on it (2026-09-20), topocentric, and a bound either side
# recommends should be measured over the other's worst object too.
STARS = [("--star", n, n)
         for n in ("Sirius", "Vega", "Polaris", "Rigil Kentaurus", "Aldebaran")]

OBJECTS = BODIES + POINTS + STARS

CONFIGS = [
    ("geo apparent, true of date, ecliptic", ["--corrections", "7"]),
    ("geo apparent, true of date, equatorial", ["--corrections", "7", "--eq"]),
    ("geo astrometric, ICRF, equatorial", ["--corrections", "1", "--icrs", "--eq"]),
    ("geo apparent, J2000, ecliptic", ["--corrections", "7", "--j2000"]),
    ("geo geometric, true of date, ecliptic", ["--corrections", "0"]),
    ("helio astrometric, true of date, ecliptic", ["--helio", "--corrections", "1"]),
    ("bary astrometric, true of date, ecliptic", ["--bary", "--corrections", "1"]),
    ("topo Zurich, apparent", ["--corrections", "7", "--topo", "8.55,47.37,500"]),
    ("topo Quito, apparent", ["--corrections", "7", "--topo", "-78.47,-0.18,2850"]),
    ("geo apparent, lahiri", ["--corrections", "7", "--sid", "lahiri"]),
    ("geo apparent, fagan-bradley", ["--corrections", "7", "--sid", "fagan-bradley"]),
    # Their worst distance-rate cell was topocentric on J2000 axes, which no
    # config above reaches: the two topocentric ones are true of date.
    ("topo Zurich, apparent, J2000", ["--corrections", "7", "--j2000",
                                      "--topo", "8.55,47.37,500"]),
]

DELTA_T = 69.2

# What this sweep asserts, by name; tools/check/ratestest.py reads the table
# from `--list-assertions` rather than keeping a copy. Five of the six are
# about the sweep having happened at all, which is the failure this tool had:
# breadth is its entire claim ("the one thing a server's own rate check
# structurally cannot do for itself"), so a sweep that reached almost nothing
# is not a weaker pass, it is a different statement.
ASSERTIONS = (
    "nothing-asked",           # the config/epoch/object grid selected no request
    "no-welcome",              # no reply carried a WELCOME: the bound was never read
    "bound-varies",            # the server advertised more than one bound in one sweep
    "nothing-answered",        # every object came back unanswered
    "min-fraction",            # too few did to call this a sweep
    "exceeds-advertisement",   # a rate error is larger than the bound in force
)


def rate_error(v):
    """|reported rate - central difference| from rows t-2h .. t+2h, as
    (lon, lat, dist_abs, dist_rel): degrees a day, then AU a day ABSOLUTE
    and the same divided by the distance. None when a row is missing.
    Five points: error ~ h^4 f5/30.

    The bound is tested against the absolute figure, because that is the
    unit 3.5a and A.3 0x0013 are written in ("1e-9 AU/day", with no per-AU
    qualifier, in both ephproto.h and registries.json). The relative figure
    is kept because it is the one that is comparable between a body at
    0.0027 AU and one at 30, but reading it as if it were the spec's
    understates a distant body by its distance -- 30x for Pluto. This side
    reported the relative figure as though it were the bound until
    2026-09-20, when the Astrolog session pointed at the units."""
    if any(r is None or any(math.isnan(x) for x in r) for r in v):
        return None
    mid = v[2]

    def d(i, wrap=False):
        f = [r[i] for r in v]
        if wrap:
            f = [f[2] + ((x - f[2] + 180.0) % 360.0 - 180.0) for x in f]
        return (f[0] - 8.0 * f[1] + 8.0 * f[3] - f[4]) / (12.0 * H_DAYS)

    dist_abs = abs(mid[5] - d(2))
    return (abs(mid[3] - d(0, True)), abs(mid[4] - d(1)),
            dist_abs, dist_abs / max(1.0, mid[2]))


def advertised(rep):
    """(deg/day, AU/day, "advertised"|"A.3 default") in force for this server."""
    if rep.rates_bound is not None:
        return rep.rates_bound[0], rep.rates_bound[1], "advertised"
    return DEFAULT_DEG_PER_DAY, DEFAULT_AU_PER_DAY, "A.3 default"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="127.0.0.1:47190")
    ap.add_argument("--client", default=os.path.join(REPO, "build", "prometheia-wire-client"))
    ap.add_argument("--epochs", type=int, default=len(EPOCHS),
                    help="how many of the epoch list to use (default: all)")
    ap.add_argument("--configs", type=int, default=len(CONFIGS),
                    help="how many of the configuration list to use (default: all)")
    ap.add_argument("--only", default="",
                    help="only objects whose name contains this (default: every object)")
    ap.add_argument("--min-fraction", type=float, default=0.5,
                    help="fail unless at least this fraction of requests was answered "
                         "(default 0.5)")
    ap.add_argument("--out", help="write every measured row here as TSV")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--list-assertions", action="store_true",
                    help="print the assertions this tool makes, one per line, and exit")
    args = ap.parse_args()
    if args.list_assertions:
        return assertlib.Assertions(ASSERTIONS).list_and_exit()

    asserts = assertlib.Assertions(ASSERTIONS)
    host, _, port = args.server.rpartition(":")
    host, port = host or "127.0.0.1", int(port)
    epochs = EPOCHS[:max(1, args.epochs)]
    configs = CONFIGS[:max(1, args.configs)]
    objects = [o for o in OBJECTS if args.only in o[2]]

    # Not None: absent 0x0013 the registry's default IS the bound in force,
    # and it has to be usable before the first reply arrives. What a reply can
    # do is replace it -- once, and only from a reply that actually carried a
    # WELCOME. Latching from any reply meant a first request that failed
    # outright locked the sweep to 1e-5 deg/day and then judged a server
    # advertising 5e-3 against it, which is four hundred false accusations
    # against somebody else's server from one dropped connection.
    bound_deg, bound_au = DEFAULT_DEG_PER_DAY, DEFAULT_AU_PER_DAY
    source = "A.3 default"
    advertisements = set()
    worst = [0.0, 0.0, 0.0, 0.0]
    worst_where = ["", "", "", ""]
    over = []
    rows = []
    asked = answered = 0

    for label, cfg in configs:
        for jd in epochs:
            base = ["--jd", repr(jd - 2 * H_DAYS), "--step", repr(H_DAYS * 86400.0),
                    "--count", "5", "--deltat", str(DELTA_T)] + cfg
            # One request per object: a server that refuses the whole request
            # over one bad object would otherwise cost the rest of the sweep.
            for spec in objects:
                flag, value, name = spec
                rep = wirelib.run(args.client, host, port, base + [flag, value],
                                  verbose=args.verbose)
                asked += 1
                if rep.server is not None:
                    seen = advertised(rep)
                    if not advertisements:
                        bound_deg, bound_au, source = seen
                    advertisements.add(seen[:2])
                e = rate_error([rep.row(0, r) for r in range(5)])
                if e is None:
                    err = rep.objects[0].err if rep.objects else -1
                    rows.append((label, jd, name, "", "", "", "", f"unanswered errCode {err}"))
                    continue
                answered += 1
                bad = e[0] > bound_deg or e[1] > bound_deg or e[2] > bound_au
                rows.append((label, jd, name, "%.4e" % e[0], "%.4e" % e[1], "%.4e" % e[2],
                             "%.4e" % e[3], "OVER" if bad else ""))
                for i in range(4):
                    if e[i] > worst[i]:
                        worst[i] = e[i]
                        worst_where[i] = f"{name}, {label}, JD {jd}"
                if bad:
                    over.append((name, label, jd, e))
                    print(f"  OVER  {name:26s} {label:40s} JD {jd:.1f}  "
                          f"lon {e[0]:.3e} lat {e[1]:.3e} deg/d  dist {e[2]:.3e} AU/d")

    print(f"\nserver {args.server}: bound {bound_deg:g} deg/day, {bound_au:g} AU/day absolute "
          f"({source})")
    print(f"asked {asked}, answered {answered}")
    print(f"{len(configs)} configuration(s) x {len(epochs)} epoch(s) x {len(objects)} object(s)"
          + (f", --only {args.only!r}" if args.only else ""))
    print(f"worst lon  {worst[0]:.4e} deg/day   ({worst_where[0]})")
    print(f"worst lat  {worst[1]:.4e} deg/day   ({worst_where[1]})")
    print(f"worst dist {worst[2]:.4e} AU/day ABSOLUTE, the bound's unit   ({worst_where[2]})")
    print(f"worst dist {worst[3]:.4e} AU/day per AU (diagnostic)   ({worst_where[3]})")
    if args.out:
        with open(args.out, "w") as f:
            f.write(f"# ratesweep {args.server} bound {bound_deg:g} {bound_au:g} ({source})\n")
            f.write("config\tepoch_tt\tobject\tlon_err\tlat_err\t"
                    "dist_err_au_per_day\tdist_err_per_au\tflag\n")
            for r in rows:
                f.write("\t".join(str(x) for x in r) + "\n")
        print(f"table: {args.out} ({len(rows)} rows)")
    print()
    fails = []
    # The cascade is deliberate. Each condition is judged once and only where
    # it is the *most specific* true thing: an empty grid would also answer
    # nothing, and two assertions firing on one input mean deleting either
    # changes nothing anybody sees, which is how a deleted check stays
    # invisible.
    if not asserts.judge(
            "nothing-asked", asked == 0,
            f"the grid selected no request at all ({len(configs)} configuration(s), "
            f"{len(epochs)} epoch(s), {len(objects)} object(s)"
            + (f", --only {args.only!r} matched nothing" if args.only else "")
            + "): this sweep measured the empty set and would have reported a pass", fails):
        asserts.judge(
            "no-welcome", not advertisements,
            f"not one of {asked} replies carried a WELCOME line, so the bound above is a "
            "guess and not this server's advertisement -- every verdict below was reached "
            "against a number the server never sent", fails)
        asserts.judge(
            "bound-varies", len(advertisements) > 1,
            "this server advertised more than one rate bound during a single sweep "
            + ", ".join(f"{d:g}/{a:g}" for d, a in sorted(advertisements))
            + " -- the first is what was enforced above and the rest were ignored", fails)
        if not asserts.judge(
                "nothing-answered", answered == 0,
                f"all {asked} requests came back unanswered. No rate was measured, so "
                "'no exceedance' is not a property of this server", fails):
            asserts.judge(
                "min-fraction", answered < args.min_fraction * asked,
                f"{answered} of {asked} requests answered, under the {args.min_fraction:g} "
                "this sweep requires. Breadth is the whole claim here; a sweep that "
                "reached a corner of the object list is a different statement", fails)
            asserts.judge(
                "exceeds-advertisement", bool(over),
                f"{len(over)} of {answered} answered row(s) exceed what this server "
                "advertises", fails)

    for line in fails:
        print("FAIL  " + line)
    if not fails:
        print(f"within its advertisement everywhere this sweep reached "
              f"({answered} of {asked} requests answered).")
    asserts.report()
    return 1 if asserts.any_fired() else 0


if __name__ == "__main__":
    sys.exit(main())

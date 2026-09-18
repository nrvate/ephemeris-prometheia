#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Client-server cross-test: one client, two v4 servers, an anchor where one exists.

docs/CROSS-TEST.md is the plan and says what counts as a pass; this runs the
legs of its runbook that need nothing but the reference client and two
running daemons:

  surfaces   each server's WELCOME, side by side (a record, not a verdict)
  same       both servers, the same question: mask 0, ICRF, equatorial,
             geocentric, the Horizons corpus's bodies at its epochs
  horizons   each server against JPL Horizons at the corpus's own points:
             mask 1 (astrometric), ICRF, equatorial, geocentric
  hamburg    the eight Hamburg points by name (kind 3) from both servers:
             heliocentric, mask 0, mean ecliptic of J2000
  helio      each server against Horizons from the Sun's centre: mask 1,
             ICRF, equatorial (Horizons' Sun-centred APPARENT place is
             referred to the Sun's equator and is never used)
  apparent   both servers, apparent place of date, every observer kind
             (geocentric, topocentric, heliocentric, barycentric, from
             Jupiter's system); no anchor -- Horizons' apparent place carries
             published frame offsets -- so a Moon gap is judged against the
             geocentric anchor, as for 'same'

The topocentric anchor is not run: Horizons' UT1 has to be recovered from its
sidereal time first (tests/test_horizons.cpp does), and until it is, a
topocentric anchor would compare two different Earth rotations.

Every comparison is an angular separation (atan2 of cross and dot), never a
difference of longitudes.  A leg is green when the servers agree AND the
anchor agrees; two servers agreeing with each other and not with the anchor
is a finding, never a pass.  Every numeric leg sends delta T explicitly and
asks on the TT scale, so no delta T model enters a comparison.

The leg table is written as TSV (--out), one row per comparison, with the
two servers' identities and both repositories' commits in its header.

Usage:
  crosstest.py --ours 127.0.0.1:47190 --theirs 127.0.0.1:47291 \\
               [--legs surfaces,same,horizons,hamburg] [--out table.tsv]
"""

import argparse
import datetime
import json
import math
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools", "fetch"))
sys.path.insert(0, os.path.join(REPO, "tools", "gen"))
import wirelib  # noqa: E402
import horizons_fetch as hf  # noqa: E402  (the corpus's request declarations)
import gen_horizons_corpus as gen  # noqa: E402  (its reader of a Horizons table)

# Bands, in arcseconds, and where each comes from.  A band is the difference
# the named cause accounts for; beyond it a row is a finding, not a failure.
SAME_BAND = 0.002  # Swiss .se1 files: a refit of DE441, ~0.001" stated for the planets
# Each server against Horizons (which integrates DE441), astrometric ICRF:
OURS_HORIZONS = {"planets": 1e-4, "moon": 0.03}  # docs/VALIDATION.md gates
THEIRS_HORIZONS = {"planets": 0.002, "moon": 0.03}  # the refit's fidelity, and the Moon's gate
HAMBURG_BAND = 0.002  # same elements; the J1900 precession models differ sub-mas
DELTA_T = 69.2  # sent explicitly; a TT request does not use it, a UT1 one would

HAMBURG = ["cupido", "hades", "zeus", "kronos", "apollon", "admetos", "vulcanus", "poseidon"]
HAMBURG_EPOCHS = [2415020.0, 2451545.0, 2488070.0]


def endpoint(s):
    host, _, port = s.rpartition(":")
    return host or "127.0.0.1", int(port)


def unit(lon_deg, lat_deg):
    lo, la = math.radians(lon_deg), math.radians(lat_deg)
    return (math.cos(la) * math.cos(lo), math.cos(la) * math.sin(lo), math.sin(la))


def sep_arcsec(a, b):
    """Angle between two (lon, lat) directions in degrees; atan2, not acos."""
    u, v = unit(*a), unit(*b)
    c = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    return math.degrees(math.atan2(math.sqrt(sum(x * x for x in c)),
                                   sum(p * q for p, q in zip(u, v)))) * 3600.0


class Table:
    COLUMNS = ["leg", "epoch_tt", "object", "observer", "frame", "plane", "mask", "deltat",
               "ours", "theirs", "anchor", "anchor_source", "sep_servers", "sep_ours_anchor",
               "sep_theirs_anchor", "band", "tier", "verdict", "note"]

    def __init__(self):
        self.rows = []

    def add(self, **kw):
        self.rows.append({c: kw.get(c, "") for c in self.COLUMNS})

    def write(self, path, header):
        with open(path, "w") as f:
            for line in header:
                f.write(f"# {line}\n")
            f.write("\t".join(self.COLUMNS) + "\n")
            for r in self.rows:
                f.write("\t".join(fmt(r[c]) for c in self.COLUMNS) + "\n")


def fmt(v):
    if isinstance(v, float):
        return f"{v:.3e}" if (v != 0 and abs(v) < 1e-3) else f"{v:.9g}"
    if isinstance(v, tuple):
        return "/".join(f"{x:.10f}" for x in v)
    return str(v)


def ask(client, srv, args, verbose):
    return wirelib.run(client, srv[0], srv[1], args, verbose)


def leg_surfaces(client, ours, theirs, table, verbose):
    a = ask(client, ours, ["--obj", "10", "--corrections", "0"], verbose)
    b = ask(client, theirs, ["--obj", "10", "--corrections", "0"], verbose)
    print("\n== surfaces (a record, not a verdict)")
    keys = sorted(set(a.caps) | set(b.caps))
    for k in keys:
        va, vb = a.caps.get(k), b.caps.get(k)
        show = (lambda v: ",".join(v) if isinstance(v, list) else str(v))
        same = va == vb
        print(f"  {k:14s} {'=' if same else '≠'}  ours {show(va)[:60]}  theirs {show(vb)[:60]}")
        table.add(leg="surfaces", object=k, ours=show(va), theirs=show(vb),
                  verdict="same" if same else "differs")
    ma = {(o, m) for o, m in a.corrmasks}
    mb = {(o, m) for o, m in b.corrmasks}
    for mask in range(8):
        oa = sum(o for o, m in ma if m == mask)
        ob = sum(o for o, m in mb if m == mask)
        table.add(leg="surfaces", object=f"corrmask {mask} observers", ours=oa, theirs=ob,
                  verdict="same" if oa == ob else "differs")
    return a, b


def corpus(prefix="geo-", center="'500@399'"):
    """(name, body, [(jd_tt, ra, dec)]) of every corpus request from one centre:
    the astrometric ICRF RA/Dec (Horizons quantity 1) at each of its instants."""
    raw = os.path.join(REPO, "horizons-raw")
    out = []
    for name, _, params in hf.requests():
        if not name.startswith(prefix) or params["CENTER"] != center:
            continue
        with open(os.path.join(raw, name + ".json")) as f:
            result = json.load(f)["result"]
        cols, rows = gen.table(result)
        idx = {c: i for i, c in enumerate(cols)}
        pts = [(gen.num(r[idx["Date_________JDTT"]]), gen.num(r[idx["R.A.___(ICRF)"]]),
                gen.num(r[idx["DEC____(ICRF)"]])) for r in rows]
        out.append((name, int(params["COMMAND"].strip("'")), pts))
    return out


def leg_same_and_horizons(client, ours, theirs, table, verbose, do_same, do_horizons):
    corpus_ = corpus()
    return compare_against_anchor(client, ours, theirs, table, verbose, corpus_, [],
                                  "geo", do_same, do_horizons)


def compare_against_anchor(client, ours, theirs, table, verbose, corpus, observer_args,
                           observer, do_same, do_horizons):
    epochs = sorted({p[0] for _, _, pts in corpus for p in pts})
    bodies = [(name, body) for name, body, _ in corpus]
    suffix = "" if observer == "geo" else "-" + observer
    anchors = {(body, p[0]): (p[1], p[2]) for _, body, pts in corpus for p in pts}
    worst = {}
    for mask, leg0, wanted in ((0, "same", do_same), (1, "horizons", do_horizons)):
        leg = leg0 + suffix
        if not wanted:
            continue
        print(f"\n== {leg}: mask {mask}, ICRF, equatorial, {observer}, "
              f"{len(bodies)} bodies x {len(epochs)} epochs")
        for jd in epochs:
            args = ["--jd", repr(jd), "--icrs", "--eq", "--corrections", str(mask),
                    "--deltat", str(DELTA_T)] + observer_args
            for _, body in bodies:
                args += ["--obj", str(body)]
            ra = ask(client, ours, args, verbose)
            rb = ask(client, theirs, args, verbose)
            for k, (name, body) in enumerate(bodies):
                va, vb = ra.row(k), rb.row(k)
                ea = ra.objects[k].err if k < len(ra.objects) else -1
                eb = rb.objects[k].err if k < len(rb.objects) else -1
                if va is None or vb is None or any(math.isnan(x) for x in va[:2] + vb[:2]):
                    table.add(leg=leg, epoch_tt=jd, object=body, observer=observer, frame="ICRF",
                              plane="equator", mask=mask, deltat=DELTA_T, verdict="unanswered",
                              note=f"errCode ours {ea} theirs {eb}")
                    continue
                pa, pb = (va[0], va[1]), (vb[0], vb[1])
                s = sep_arcsec(pa, pb)
                row = dict(leg=leg, epoch_tt=jd, object=body, observer=observer, frame="ICRF",
                           plane="equator", mask=mask, deltat=DELTA_T, ours=pa, theirs=pb,
                           sep_servers=s)
                if leg0 == "same":
                    row.update(band=SAME_BAND, tier=2,
                               verdict="agree" if s <= SAME_BAND else "finding",
                               note="DE440 against Swiss .se1 (DE441 refit)")
                    worst[("same", body)] = max(worst.get(("same", body), 0.0), s)
                else:
                    anc = anchors.get((body, jd))
                    cls = "moon" if body == 301 else "planets"
                    so = sep_arcsec(pa, anc)
                    st = sep_arcsec(pb, anc)
                    ok_o = so <= OURS_HORIZONS[cls]
                    ok_t = st <= THEIRS_HORIZONS[cls]
                    both_agree = s <= SAME_BAND
                    if ok_o and ok_t:
                        verdict = "agree"
                    elif both_agree:
                        verdict = "finding"  # the servers agree, the sky does not
                    else:
                        verdict = "finding (ours)" if not ok_o else "finding (theirs)"
                    row.update(anchor=anc, anchor_source=f"Horizons {name} q1",
                               sep_ours_anchor=so, sep_theirs_anchor=st,
                               band=f"ours {OURS_HORIZONS[cls]} theirs {THEIRS_HORIZONS[cls]}",
                               tier=2, verdict=verdict)
                    worst[("ours", body)] = max(worst.get(("ours", body), 0.0), so)
                    worst[("theirs", body)] = max(worst.get(("theirs", body), 0.0), st)
                table.add(**row)
        for (who, body), w in sorted(worst.items()):
            if (leg0 == "same") == (who == "same"):
                label = {"same": "ours vs theirs", "ours": "ours vs Horizons",
                         "theirs": "theirs vs Horizons"}[who]
                print(f"  body {body:4d}  worst {label}: {w:.6f}\"")


def leg_hamburg(client, ours, theirs, table, verbose):
    print("\n== hamburg: kind 3 by token, heliocentric, mask 0, mean ecliptic of J2000")
    for jd in HAMBURG_EPOCHS:
        args = ["--jd", repr(jd), "--helio", "--j2000", "--corrections", "0",
                "--deltat", str(DELTA_T)]
        for t in HAMBURG:
            args += ["--hyp", t]
        ra = ask(client, ours, args, verbose)
        rb = ask(client, theirs, args, verbose)
        for k, t in enumerate(HAMBURG):
            va, vb = ra.row(k), rb.row(k)
            if va is None or vb is None or any(math.isnan(x) for x in va[:2] + vb[:2]):
                table.add(leg="hamburg", epoch_tt=jd, object=t, verdict="unanswered")
                continue
            s = sep_arcsec((va[0], va[1]), (vb[0], vb[1]))
            table.add(leg="hamburg", epoch_tt=jd, object=t, observer="helio", frame="J2000",
                      plane="ecliptic", mask=0, deltat=DELTA_T, ours=(va[0], va[1]),
                      theirs=(vb[0], vb[1]), sep_servers=s, band=HAMBURG_BAND, tier=2,
                      verdict="agree" if s <= HAMBURG_BAND else "finding",
                      note=f"dist ours {va[2]:.9f} theirs {vb[2]:.9f}")
            print(f"  {jd:.1f} {t:9s} {s:.6f}\"  dDist {va[2] - vb[2]:+.2e} AU")


APPARENT_BODIES = [10, 301, 199, 299, 4, 5, 6, 7, 8, 9]
ZURICH = hf.SITES["zurich"]  # lon deg E, lat deg, height km


def leg_apparent(client, ours, theirs, table, verbose):
    """Leg 8: apparent place of date, every observer kind, server against server."""
    epochs = sorted({p[0] for _, _, pts in corpus() for p in pts})
    site = f"{ZURICH[0]},{ZURICH[1]},{ZURICH[2] * 1000.0}"  # the client takes metres
    observers = [
        ("geo", [], 7),
        ("topo", ["--topo", site], 7),
        ("helio", ["--helio"], 5),  # the Sun's centre honours no deflection
        ("bary", ["--bary"], 7),
        ("jupiter", ["--center", "5"], 7),
    ]
    print("\n== apparent: true ecliptic of date, every observer, server against server")
    for obs, obs_args, mask in observers:
        bodies = [b for b in APPARENT_BODIES
                  if not (obs == "helio" and b == 10) and not (obs == "jupiter" and b == 5)]
        worst = {}
        for jd in epochs:
            args = ["--jd", repr(jd), "--corrections", str(mask), "--deltat", str(DELTA_T)] + obs_args
            for b in bodies:
                args += ["--obj", str(b)]
            ra = ask(client, ours, args, verbose)
            rb = ask(client, theirs, args, verbose)
            for k, b in enumerate(bodies):
                va, vb = ra.row(k), rb.row(k)
                if va is None or vb is None or any(math.isnan(x) for x in va[:2] + vb[:2]):
                    ea = ra.objects[k].err if k < len(ra.objects) else -1
                    eb = rb.objects[k].err if k < len(rb.objects) else -1
                    table.add(leg="apparent", epoch_tt=jd, object=b, observer=obs,
                              frame="true of date", plane="ecliptic", mask=mask, deltat=DELTA_T,
                              verdict="unanswered", note=f"errCode ours {ea} theirs {eb}")
                    continue
                sep = sep_arcsec((va[0], va[1]), (vb[0], vb[1]))
                worst[b] = max(worst.get(b, 0.0), sep)
                table.add(leg="apparent", epoch_tt=jd, object=b, observer=obs,
                          frame="true of date", plane="ecliptic", mask=mask, deltat=DELTA_T,
                          ours=(va[0], va[1]), theirs=(vb[0], vb[1]), sep_servers=sep,
                          band=SAME_BAND, tier=2,
                          verdict="agree" if sep <= SAME_BAND else "finding",
                          note="no anchor: Horizons' apparent place carries frame offsets")
        print(f"  {obs:8s} " + "  ".join(f"{b}:{w:.4f}" for b, w in sorted(worst.items())))


def adjudicate_same(table):
    """Two servers disagreeing is not yet a verdict; the anchor decides.

    A 'same' row beyond its band is re-read against the Horizons leg at the
    same body and epoch (astrometric, so the nearest anchored question). If
    both servers sit inside their own bands there, the gap between them is
    the documented difference of their sources -- DE440 against DE441 for
    the Moon, the .se1 refit for the planets -- not a defect in either. If
    the anchor could not be asked, the row is left unadjudicated.
    """
    anchored = {(r["epoch_tt"], r["object"]): r for r in table.rows if r["leg"] == "horizons"}
    anchored_helio = {(r["epoch_tt"], r["object"]): r for r in table.rows
                      if r["leg"] == "horizons-helio"}
    for r in table.rows:
        if r["leg"] not in ("same", "same-helio", "apparent") or r["verdict"] != "finding":
            continue
        pool = anchored_helio if r["leg"] == "same-helio" else anchored
        h = pool.get((r["epoch_tt"], r["object"]))
        if h is None or h["verdict"] == "unanswered":
            r["verdict"] = "unadjudicated"
            r["note"] += "; the anchor leg was not answered here"
        elif h["verdict"] == "agree":
            r["verdict"] = "expected-difference"
            r["note"] += ("; each server inside its own Horizons band here, so the gap is "
                          "their sources' (DE440 vs DE441 / the .se1 refit)")
        else:
            r["note"] += f"; anchor verdict: {h['verdict']}"


def git_head(path):
    try:
        head = subprocess.run(["git", "-C", path, "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True, timeout=10).stdout.strip() or "?"
        dirty = subprocess.run(["git", "-C", path, "status", "--porcelain", "--untracked-files=no"],
                               capture_output=True, text=True, timeout=10).stdout.strip()
        return head + ("+dirty" if dirty else "")
    except Exception:  # noqa: BLE001
        return "?"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ours", default="127.0.0.1:47190")
    ap.add_argument("--theirs", default="127.0.0.1:47291")
    ap.add_argument("--client", default=os.path.join(REPO, "build", "prometheia-wire-client"))
    ap.add_argument("--legs", default="surfaces,same,horizons,hamburg,helio,apparent")
    ap.add_argument("--out", help="write the leg table (TSV) here")
    ap.add_argument("--astrolog", default="/nvm/work/ephv4", help="the Astrolog tree, for its commit")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    client = shutil.which(args.client) or args.client
    ours, theirs = endpoint(args.ours), endpoint(args.theirs)
    legs = set(args.legs.split(","))
    table = Table()

    a = ask(client, ours, ["--obj", "10", "--corrections", "0"], args.verbose)
    b = ask(client, theirs, ["--obj", "10", "--corrections", "0"], args.verbose)
    if not a.server or not b.server:
        sys.exit("both servers must answer a WELCOME: " + (a.stderr or b.stderr))
    print(f"ours   {a.server}  {a.dataset}")
    print(f"theirs {b.server}  {b.dataset}")

    if "surfaces" in legs:
        leg_surfaces(client, ours, theirs, table, args.verbose)
    if legs & {"same", "horizons"}:
        leg_same_and_horizons(client, ours, theirs, table, args.verbose,
                              "same" in legs, "horizons" in legs)
    if "hamburg" in legs:
        leg_hamburg(client, ours, theirs, table, args.verbose)
    if "helio" in legs:
        compare_against_anchor(client, ours, theirs, table, args.verbose,
                               corpus("helio-", "'500@10'"), ["--helio"], "helio", True, True)
    if "apparent" in legs:
        leg_apparent(client, ours, theirs, table, args.verbose)
    if "horizons" in legs or "helio" in legs:
        adjudicate_same(table)

    counts = {}
    for r in table.rows:
        if r["leg"] != "surfaces":
            counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    print("\nverdicts: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    if args.out:
        header = [
            f"crosstest {datetime.datetime.now(datetime.timezone.utc):%Y-%m-%dT%H:%M:%SZ}",
            f"ours   {a.server} dataset {a.dataset} prometheia {git_head(REPO)}",
            f"theirs {b.server} dataset {b.dataset} astrolog {git_head(args.astrolog)}",
            "angles in arcsec; positions lon/lat or RA/Dec in degrees; docs/CROSS-TEST.md",
        ]
        table.write(args.out, header)
        print(f"table: {args.out} ({len(table.rows)} rows)")
    numeric = sum(counts.values())
    if numeric == 0 and legs - {"surfaces"}:
        print("FAIL: NOTHING COMPARED")
        return 1
    return 0 if not any(k.startswith("finding") or k == "unanswered" for k in counts) else 1


if __name__ == "__main__":
    sys.exit(main())

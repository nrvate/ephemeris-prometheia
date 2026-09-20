#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Client-server cross-test: one client, two v4 servers, an anchor where one exists.

docs/CROSS-TEST.md is the plan and says what counts as a pass; this runs the
legs of its runbook that need nothing but the reference client and two
running daemons:

  surfaces   each server's WELCOME, side by side (a record, not a verdict),
             then every (observer, kind, mask): a listed mask (0x0004
             and 0x0014) must be served, an unlisted one must draw ERROR 11
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
             Jupiter's system), at the fullest mask both WELCOMEs list for
             that observer; no anchor of its own -- Horizons' apparent place
             carries published frame offsets -- so a gap is judged by the
             anchor leg from the same observer, where there is one
  bary       the Sun from the barycentre, mask 0, against Horizons'
             geometric vectors, judged as a length (km)
  deflection from Jupiter's centre, each server's bending of the light
             (mask 3 against its own mask 1) against the textbook formula
  deflection-geo, deflection-topo
             the same textbook referee at the observers both servers
             advertise -- the Earth's centre, then two sites -- with each
             body searched to its closest approach to the Sun, where the
             term is large enough for a verdict to mean anything
  points   orbit points (Moon and planets, mean and osculating) by
             direction and distance, mask 0, and the node of date asked in
             the J2000 frame (3.5a as amended); then the same points from
             the Sun, the barycentre and Mars's centre at masks 0 and 1, the
             Moon's held to the Earth answered beside them
  sidereal   the three A.8 sidereal planes for two zodiacs
  stars      29 fixed stars by name, tropical and sidereal, and the two
             IAU names of alpha Centauri
  topo       each server against Horizons from a site on the Earth: mask 1,
             ICRF, equatorial, with the Delta T that reproduces Horizons'
             local apparent sidereal time (build/prometheia-ut1), then
             apparent place at the same rows, server against server

Differences the two projects have agreed are deliberate are marked
expected-difference with the reason in the row's note: Swiss's heliocentric
light time, Swiss's topocentric site about the mean pole (only where the
geocentric answers agree at that instant), and a row one server refuses as
outside its coverage (errCode 3).

Every comparison is an angular separation (atan2 of cross and dot), never a
difference of longitudes.  A leg is green when the servers agree AND the
anchor agrees; two servers agreeing with each other and not with the anchor
is a finding, never a pass.  Every numeric leg sends delta T explicitly and
asks on the TT scale, so no delta T model enters a comparison.

The leg table is written as TSV (--out), one row per comparison, with the
two servers' identities and both repositories' commits in its header.

Usage:
  crosstest.py --ours 127.0.0.1:47190 --theirs 127.0.0.1:47391 \\
               [--legs surfaces,same,...,sidsweep,...] [--out table.tsv]
"""

import argparse
import datetime
import json
import math
import re
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
import binary_orbits  # noqa: E402
import horizons_fetch as hf  # noqa: E402  (the corpus's request declarations)
import gen_horizons_corpus as gen  # noqa: E402  (its reader of a Horizons table)

# Bands, in arcseconds, and where each comes from.  A band is the difference
# the named cause accounts for; beyond it a row is a finding, not a failure.
SAME_BAND = 0.002  # the Moon, and anything REFIT_KM does not name
# Each server against Horizons (which integrates DE441), astrometric ICRF:
OURS_HORIZONS = {"planets": 1e-4, "moon": 0.03}  # docs/VALIDATION.md gates
THEIRS_HORIZONS = {"moon": 0.03}  # the Moon's gate; the planets use REFIT_KM
# The Swiss .se1 files' error is fixed in POSITION, not in angle: the
# Astrolog side measured the same third of a kilometre on Mars at 0.37 AU
# and at 1.85 AU (tools/se1-fit-error.c in their tree), so "1 mas" is only a
# typical figure at typical distances. Measured here against Horizons over
# the corpus (2026-09-18): worst 0.44 km Sun, 0.46 Mercury, 0.55 Venus, 0.74
# Mars, 1.9 Jupiter, 4.9 Saturn, 4.5 Uranus, 7.4 Neptune, 8.3 Pluto; the Sun
# from the barycentre 1.05. Each band is that worst with about a third added.
REFIT_KM = {10: 1.5, 199: 0.7, 299: 0.75, 399: 0.75, 4: 1.0, 5: 2.5, 6: 6.5, 7: 6.5,
            8: 10.0, 9: 11.0}
# From a planet's centre, that planet's own position error adds.
OBSERVER_KM = {"jupiter": REFIT_KM[5]}
HAMBURG_BAND = 0.002  # same elements; the J1900 precession models differ sub-mas
# The Sun from the barycentre, as a length: ours is DE440, the same fit as
# Horizons' DE441 over the corpus (measured 3e-7 km); theirs is the refit's
# band for the Sun, since a position error does not care which observer it
# is seen from.
BARY_SUN_KM = {"ours": 0.001, "theirs": REFIT_KM[10]}
AU_KM = 149597870.7
# Our light-time range against Horizons' delta: measured 1.1 m geocentric and
# 0.25 m heliocentric (2026-09-18); the band is an estimate, 4x that. Theirs
# is recorded (1-22 km, their compressed files), not graded: nothing
# published gives a band for it.
RANGE_BAND_OURS_KM = 0.005
DELTA_T = 69.2  # sent explicitly; a TT request does not use it, a UT1 one would

HAMBURG = ["cupido", "hades", "zeus", "kronos", "apollon", "admetos", "vulcanus", "poseidon"]
HAMBURG_EPOCHS = [2415020.0, 2451545.0, 2488070.0]


def refit_band(body, dist_au, observer="geo", anchor=False):
    """The band (arcsec) for astrolog-ephd's answer on one body at this
    distance: REFIT_KM as an angle, plus the observer's own error. The Moon
    and unknown bodies keep their angular bands."""
    if body == 301 or body not in REFIT_KM or not dist_au or math.isnan(dist_au):
        return THEIRS_HORIZONS["moon"] if anchor and body == 301 else SAME_BAND
    km = REFIT_KM[body] + OBSERVER_KM.get(observer, 0.0)
    # The larger of the two: the length widens the band only at close range,
    # where a fixed angle is wrong. It does not tighten it for far bodies,
    # because the length was measured geocentrically, and from the
    # barycentre the refit's errors are larger (Neptune ~21 km).
    return max(SAME_BAND, math.degrees(km / (dist_au * AU_KM)) * 3600.0)


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
               "sep_theirs_anchor", "band", "tier", "verdict", "note", "req_ours",
               "req_theirs"]

    def __init__(self):
        self.rows = []
        # The two replies the next rows come from: each row records their
        # request ids, which prometheiad logs as req=<id>, so a finding can
        # be matched to the server lines behind it. Set by each leg.
        self.pair = (None, None)

    def asked(self, ra, rb):
        """Name the replies the rows that follow were computed from."""
        self.pair = (ra, rb)

    def add(self, **kw):
        ra, rb = self.pair
        kw.setdefault("req_ours", ra.request_id if ra is not None and ra.request_id else "")
        kw.setdefault("req_theirs", rb.request_id if rb is not None and rb.request_id else "")
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
    table.asked(a, b)
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
    leg_refusals(client, ours, theirs, a, b, table, verbose)
    return a, b


OBSERVERS = [("geo", 0, []), ("topo", 1, ["--topo", "8.55,47.37,500.0"]), ("helio", 2, ["--helio"]),
             ("bary", 3, ["--bary"]), ("jupiter", 4, ["--center", "5"])]


def advertised(rep, bit, kind=0):
    """The exact masks a server lists for one observer and object kind: 0x0004
    for the observer, union 0x0014's additions for the pair (the per-kind
    drop, 1.1). Kind 0 is a body, which is what the numeric legs ask for."""
    return {m for m in range(8) if rep.permitted(bit, kind, m)}


def common_mask(a, b, bit):
    """The fullest mask both servers list for this observer: the apparent
    leg sends only what both advertise, because 3.5a makes anything else
    ERROR 11 and a lenient server's answer to it is not a comparison."""
    both = advertised(a, bit) & advertised(b, bit)
    return max(both, key=lambda m: (bin(m).count("1"), m)) if both else None


REFUSAL_KINDS = [("body", 0, ["--obj", "4"]), ("orbit point", 1, ["--node", "4.a"])]


def leg_refusals(client, ours, theirs, a, b, table, verbose):
    """3.5a and the per-kind drop, from the wire: for every observer and
    object kind, a mask the server lists must be served and one it does not
    list must draw ERROR 11. Bodies and orbit points both, because 0x0014
    exists for the pair where they differ (an orbit point from the Sun's
    centre). Either failure is a finding: a client cannot know which
    behaviour it will meet."""
    print("\n== refusals: every (observer, kind, mask): listed is served, unlisted is ERROR 11")
    for who, srv, rep in (("ours", ours, a), ("theirs", theirs, b)):
        for obs, bit, obs_args in OBSERVERS:
            for kind_name, kind, obj_args in REFUSAL_KINDS:
                listed = advertised(rep, bit, kind)
                for mask in range(8):
                    r = ask(client, srv, obj_args + ["--jd", "2451545.0", "--corrections",
                                                     str(mask), "--deltat", str(DELTA_T)]
                            + obs_args, verbose)
                    table.asked(r if who == "ours" else None, r if who == "theirs" else None)
                    refused = "ERROR 11" in r.stderr
                    what = "ERROR 11" if refused else (
                        f"answered, errCode {r.objects[0].err}" if r.objects else r.stderr[:60])
                    if mask in listed:
                        verdict = "agree" if not refused else f"finding ({who})"
                        note = "listed in WELCOME, so it must be served"
                    else:
                        verdict = "agree" if refused else f"finding ({who})"
                        note = "unlisted in WELCOME; 3.5a requires ERROR 11"
                    table.add(leg="refusals", object=kind_name, observer=obs, mask=mask,
                              **{who: what}, verdict=verdict, note=note)
                    if verdict != "agree":
                        print(f"  {who:6s} {obs:8s} {kind_name:11s} mask {mask}: {what} ({note})")


def corpus_rows(name, params):
    """One corpus file's table, with its instant count asserted.

    horizons-raw/ is fetched, not committed, so a partial fetch or a reply
    that stopped short yields fewer instants -- and every leg built on it
    then reports "N of N agree" about a corpus that was not built. The
    request's own TLIST says how many instants were asked for, so the count
    is checked rather than merely printed. A number a check reports is a
    number the check should assert (the Astrolog side's farm-count finding,
    2026-09-20: their soak named a file count three times and never counted
    what landed on disk).
    """
    path = os.path.join(REPO, "horizons-raw", name + ".json")
    with open(path) as f:
        result = json.load(f)["result"]
    cols, rows = gen.table(result)
    want = len(params["TLIST"].split())
    if len(rows) != want:
        raise SystemExit(
            f"{os.path.relpath(path, REPO)} holds {len(rows)} instants, not the {want} its "
            f"own request asks for. Every row of every leg built on it would be a claim "
            f"about a corpus that was not fetched. Re-fetch: tools/fetch/horizons_fetch.py")
    return cols, rows


def corpus(prefix="geo-", center="'500@399'"):
    """(name, body, [(jd_tt, ra, dec, delta)]) of every corpus request from one
    centre: the astrometric ICRF RA/Dec (Horizons quantity 1) and the
    light-time range in AU (quantity 20, `delta`) at each of its instants."""
    out = []
    for name, _, params in hf.requests():
        if not name.startswith(prefix) or params["CENTER"] != center:
            continue
        cols, rows = corpus_rows(name, params)
        idx = {c: i for i, c in enumerate(cols)}
        pts = [(gen.num(r[idx["Date_________JDTT"]]), gen.num(r[idx["R.A.___(ICRF)"]]),
                gen.num(r[idx["DEC____(ICRF)"]]),
                gen.num(r[idx["delta"]]) if "delta" in idx else None) for r in rows]
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
    ranges = {(body, p[0]): p[3] for _, body, pts in corpus for p in pts}
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
            table.asked(ra, rb)
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
                band = refit_band(body, va[2], observer)
                if leg0 == "same":
                    row.update(band=band, tier=2,
                               verdict="agree" if s <= band else "finding",
                               note="DE440 against Swiss .se1 (DE441 refit)")
                    worst[("same", body)] = max(worst.get(("same", body), 0.0), s)
                else:
                    anc = anchors.get((body, jd))
                    cls = "moon" if body == 301 else "planets"
                    so = sep_arcsec(pa, anc)
                    st = sep_arcsec(pb, anc)
                    ok_o = so <= OURS_HORIZONS[cls]
                    band_t = refit_band(body, va[2], observer, anchor=True)
                    ok_t = st <= band_t
                    both_agree = s <= band
                    if ok_o and ok_t:
                        verdict = "agree"
                    elif both_agree:
                        verdict = "finding"  # the servers agree, the sky does not
                    else:
                        verdict = "finding (ours)" if not ok_o else "finding (theirs)"
                    row.update(anchor=anc, anchor_source=f"Horizons {name} q1",
                               sep_ours_anchor=so, sep_theirs_anchor=st,
                               band=f"ours {OURS_HORIZONS[cls]} theirs {band_t:.6f}",
                               tier=2, verdict=verdict)
                    worst[("ours", body)] = max(worst.get(("ours", body), 0.0), so)
                    worst[("theirs", body)] = max(worst.get(("theirs", body), 0.0), st)
                    # Distance: the positions above are angles only, blind to
                    # range. Horizons' delta is the light-time range, which is
                    # what mask 1 answers.
                    rng = ranges.get((body, jd))
                    if rng is not None:
                        ko, kt = abs(va[2] - rng) * AU_KM, abs(vb[2] - rng) * AU_KM
                        row["note"] = f"range vs Horizons: ours {ko:.4f} km, theirs {kt:.3f} km"
                        if ko > RANGE_BAND_OURS_KM:
                            row["verdict"] = "finding (ours)"
                            row["note"] += f" (ours over {RANGE_BAND_OURS_KM} km)"
                        worst[("ours-km", body)] = max(worst.get(("ours-km", body), 0.0), ko)
                        worst[("theirs-km", body)] = max(worst.get(("theirs-km", body), 0.0), kt)
                table.add(**row)
        for (who, body), w in sorted(worst.items()):
            if (leg0 == "same") == (who == "same"):
                label = {"same": "ours vs theirs", "ours": "ours vs Horizons",
                         "theirs": "theirs vs Horizons", "ours-km": "range ours vs Horizons",
                         "theirs-km": "range theirs vs Horizons"}[who]
                unit = " km" if who.endswith("-km") else "\""
                print(f"  body {body:4d}  worst {label}: {w:.6f}{unit}")


def leg_hamburg(client, ours, theirs, table, verbose):
    print("\n== hamburg: kind 3 by token, heliocentric, mask 0, mean ecliptic of J2000")
    for jd in HAMBURG_EPOCHS:
        args = ["--jd", repr(jd), "--helio", "--j2000", "--corrections", "0",
                "--deltat", str(DELTA_T)]
        for t in HAMBURG:
            args += ["--hyp", t]
        ra = ask(client, ours, args, verbose)
        rb = ask(client, theirs, args, verbose)
        table.asked(ra, rb)
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


def leg_apparent(client, ours, theirs, wel_a, wel_b, table, verbose):
    """Leg 8: apparent place of date, every observer kind, server against
    server, at the fullest correction mask both servers list for it."""
    epochs = sorted({p[0] for _, _, pts in corpus() for p in pts})
    site = f"{ZURICH[0]},{ZURICH[1]},{ZURICH[2] * 1000.0}"  # the client takes metres
    observers = [
        ("geo", [], 0),
        ("topo", ["--topo", site], 1),
        ("helio", ["--helio"], 2),
        ("bary", ["--bary"], 3),
        ("jupiter", ["--center", "5"], 4),
    ]
    print("\n== apparent: true ecliptic of date, every observer, server against server")
    for obs, obs_args, bit in observers:
        mask = common_mask(wel_a, wel_b, bit)
        if mask is None:
            print(f"  {obs:8s} no mask both servers list")
            continue
        print(f"  {obs:8s} mask {mask} (ours lists {sorted(advertised(wel_a, bit))}, "
              f"theirs {sorted(advertised(wel_b, bit))})")
        bodies = [b for b in APPARENT_BODIES
                  if not (obs == "helio" and b == 10) and not (obs == "jupiter" and b == 5)]
        worst = {}
        for jd in epochs:
            args = ["--jd", repr(jd), "--corrections", str(mask), "--deltat", str(DELTA_T)] + obs_args
            for b in bodies:
                args += ["--obj", str(b)]
            ra = ask(client, ours, args, verbose)
            rb = ask(client, theirs, args, verbose)
            table.asked(ra, rb)
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
                          band=refit_band(b, va[2], obs), tier=2,
                          verdict="agree" if sep <= refit_band(b, va[2], obs) else "finding",
                          note="no anchor: Horizons' apparent place carries frame offsets")
        print(f"  {obs:8s} " + "  ".join(f"{b}:{w:.4f}" for b, w in sorted(worst.items())))


def topo_corpus():
    """(name, body, site, [(jd_tt, ra, dec, tdb_minus_ut, last_hours)]) of every
    topocentric corpus request: astrometric ICRF RA/Dec and the Horizons
    columns that fix its Earth rotation."""
    out = []
    for name, _, params in hf.requests():
        if not name.startswith("topo-"):
            continue
        site = tuple(float(x) for x in params["SITE_COORD"].strip("'").split(","))
        cols, rows = corpus_rows(name, params)
        idx = {c: i for i, c in enumerate(cols)}
        pts = [tuple(gen.num(r[idx[c]]) for c in ("Date_________JDTT", "R.A.___(ICRF)",
                                                  "DEC____(ICRF)", "TDB-UT", "L_Ap_Sid_Time"))
               for r in rows]
        out.append((name, int(params["COMMAND"].strip("'")), site, pts))
    return out


def leg_bary(client, ours, theirs, table, verbose):
    """The Sun from the barycentre, geometric (mask 0), ICRF, equatorial,
    against Horizons' vectors: the anchor for the barycentric observer,
    where the Sun is close enough that its position error shows directly."""
    cols, rows = corpus_rows("bary-sun", next(p for n, _, p in hf.requests() if n == "bary-sun"))
    idx = {c: i for i, c in enumerate(cols)}
    print("\n== horizons-bary: the Sun from the barycentre, mask 0, ICRF, equatorial")
    for r in rows:
        jd = gen.num(r[idx["JDTDB"]])  # the corpus instant; TDB - TT moves the Sun < 1 cm
        x, y, z = (gen.num(r[idx[c]]) for c in ("X", "Y", "Z"))
        dist = math.sqrt(x * x + y * y + z * z)
        anc = (math.degrees(math.atan2(y, x)) % 360.0, math.degrees(math.asin(z / dist)))
        args = ["--jd", repr(jd), "--icrs", "--eq", "--corrections", "0", "--bary", "--obj", "10",
                "--deltat", str(DELTA_T)]
        ra, rb = ask(client, ours, args, verbose), ask(client, theirs, args, verbose)
        table.asked(ra, rb)
        va, vb = ra.row(0), rb.row(0)
        base = dict(leg="horizons-bary", epoch_tt=jd, object=10, observer="bary", frame="ICRF",
                    plane="equator", mask=0, deltat=DELTA_T)
        if va is None or vb is None or any(math.isnan(v) for v in va[:2] + vb[:2]):
            ea = ra.objects[0].err if ra.objects else -1
            eb = rb.objects[0].err if rb.objects else -1
            table.add(**base, verdict="unanswered", note=f"errCode ours {ea} theirs {eb}")
            continue
        pa, pb = (va[0], va[1]), (vb[0], vb[1])
        so, st = sep_arcsec(pa, anc), sep_arcsec(pb, anc)
        # Position error in km: the angle at this range plus the range itself.
        eo = math.hypot(math.radians(so / 3600.0) * dist, va[2] - dist) * AU_KM
        et = math.hypot(math.radians(st / 3600.0) * dist, vb[2] - dist) * AU_KM
        ok_o, ok_t = eo <= BARY_SUN_KM["ours"], et <= BARY_SUN_KM["theirs"]
        verdict = ("agree" if ok_o and ok_t else
                   "finding (ours)" if not ok_o else "finding (theirs)")
        table.add(**base, ours=pa, theirs=pb, anchor=anc, anchor_source="Horizons bary-sun vectors",
                  sep_servers=sep_arcsec(pa, pb), sep_ours_anchor=so, sep_theirs_anchor=st,
                  band=f"km ours {BARY_SUN_KM['ours']} theirs {BARY_SUN_KM['theirs']}", tier=2,
                  verdict=verdict, note=f"position error km ours {eo:.3f} theirs {et:.3f}")
        print(f"  {jd:.1f}  ours {so:.5f}\" {eo:8.3f} km   theirs {st:.5f}\" {et:8.3f} km")


# 2GM/c^2 of the Sun in AU (IAU 2015 nominal GM), for the textbook deflection.
SUN_2GM_C2_AU = 2.0 * 1.3271244e20 / 299792458.0 ** 2 / 1000.0 / AU_KM
DEFLECTION_BAND = 0.0002  # arcsec: retardation choices, not models, beyond this


def vector(v):
    """(RA deg, Dec deg, distance AU) -> cartesian AU."""
    ra, de = math.radians(v[0]), math.radians(v[1])
    return (v[2] * math.cos(de) * math.cos(ra), v[2] * math.cos(de) * math.sin(ra),
            v[2] * math.sin(de))


def textbook_deflection(obs_to_body, obs_to_sun):
    """The Sun's light deflection of a body, as in USNO Circular 179 (the
    NOVAS form): the observer-to-body direction bent by 2GM/(c^2 |E|) with E
    the Sun-to-observer vector. Written here from the formula, not from
    either engine, so it can referee the two. Returns the bent unit vector."""
    def unit3(v):
        n = math.sqrt(sum(x * x for x in v))
        return tuple(x / n for x in v)

    def dot(a, b):
        return sum(x * y for x, y in zip(a, b))

    e_vec = tuple(-x for x in obs_to_sun)
    q_vec = tuple(b - s for b, s in zip(obs_to_body, obs_to_sun))
    p, e, q = unit3(obs_to_body), unit3(e_vec), unit3(q_vec)
    fac1 = SUN_2GM_C2_AU / math.sqrt(dot(e_vec, e_vec))
    fac2 = 1.0 + dot(q, e)
    return tuple(p[i] + fac1 * (dot(p, q) * e[i] - dot(e, p) * q[i]) / fac2 for i in range(3))


def angle(a, b):
    cx = (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
    return math.degrees(math.atan2(math.sqrt(sum(c * c for c in cx)),
                                   sum(x * y for x, y in zip(a, b)))) * 3600.0


def leg_deflection(client, ours, theirs, wel_a, wel_b, table, verbose):
    """Deflection seen from Jupiter's centre, where no Horizons table observes:
    each server's mask-3 answer against the textbook deflection applied to its
    own mask-1 answer. Light time is in both, so what is compared is the
    bending alone; each server is judged against its own geometry."""
    # Each server is judged only if it lists both masks from a body centre; one
    # that does not is recorded as not advertising the term, not as passing.
    judged = {who: {1, 3} <= advertised(wel, 4)
              for who, wel in (("ours", wel_a), ("theirs", wel_b))}
    if not any(judged.values()):
        table.asked(None, None)
        print("\n== deflection: FAIL, neither server lists masks 1 and 3 from a body centre")
        table.add(leg="deflection", observer="jupiter", verdict="unanswered",
                  note="no server lists masks 1 and 3 from a body centre")
        return
    epochs = sorted({p[0] for _, _, pts in corpus() for p in pts})
    bodies = [b for b in APPARENT_BODIES if b != 5]  # the Sun included: its bending is nil
    print("\n== deflection: from Jupiter's centre, mask 3 against mask 1 + the textbook bending")
    worst = {}
    for jd in epochs:
        def ask_all(srv, mask, extra):
            args = ["--jd", repr(jd), "--icrs", "--eq", "--corrections", str(mask), "--deltat",
                    str(DELTA_T), "--center", "5"] + extra
            return ask(client, srv, args, verbose)
        objs = [a for b in bodies for a in ("--obj", str(b))]
        sun_a = ask_all(ours, 0, ["--obj", "10"]).row(0)
        sun_b = ask_all(theirs, 0, ["--obj", "10"]).row(0)
        got = {(who, m): ask_all(srv, m, objs) for who, srv in (("ours", ours), ("theirs", theirs))
               if judged[who] for m in (1, 3)}
        for k, b in enumerate(bodies):
            base = dict(leg="deflection", epoch_tt=jd, object=b, observer="jupiter", frame="ICRF",
                        plane="equator", mask=3, deltat=DELTA_T, tier=3,
                        anchor_source="textbook deflection (USNO Circular 179) on each "
                                      "server's own mask-1 answer")
            table.asked(got.get(("ours", 3)), got.get(("theirs", 3)))
            rows = {key: rep.row(k) for key, rep in got.items()}
            def err(who):
                rep_ = got.get((who, 3))
                return rep_.objects[k].err if rep_ and k < len(rep_.objects) else 0
            if (sun_a is None or sun_b is None or
                    any(v is None or math.isnan(v[0]) for v in rows.values())):
                table.add(**base, verdict="unanswered",
                          note=f"errCode ours {err('ours')} theirs {err('theirs')}")
                continue
            res = {}
            for who, sun in (("ours", sun_a), ("theirs", sun_b)):
                if not judged[who]:
                    continue
                bent = (vector(rows[(who, 1)]) if b == 10 else  # the Sun does not bend its own light
                        textbook_deflection(vector(rows[(who, 1)]), vector(sun)))
                res[who] = angle(vector(rows[(who, 3)]), bent)
                worst[who] = max(worst.get(who, 0.0), res[who])
            ok_o = res.get("ours", 0.0) <= DEFLECTION_BAND
            ok_t = res.get("theirs", 0.0) <= DEFLECTION_BAND
            verdict = ("agree" if ok_o and ok_t else "finding" if not ok_o and not ok_t else
                       "finding (ours)" if not ok_o else "finding (theirs)")
            unlisted = [who for who, j in judged.items() if not j]
            table.add(**base, sep_servers=(angle(vector(rows[("ours", 3)]),
                                                 vector(rows[("theirs", 3)]))
                                           if not unlisted else ""),
                      sep_ours_anchor=res.get("ours", ""), sep_theirs_anchor=res.get("theirs", ""),
                      band=DEFLECTION_BAND, verdict=verdict,
                      note="; ".join(f"{w} does not list masks 1 and 3 from a body centre"
                                     for w in unlisted))
    for who, w in sorted(worst.items()):
        print(f"  worst {who} vs textbook: {w:.6f}\"")


# Solar deflection falls off roughly as 1/elongation: 1.75" grazing the limb,
# ~0.004" at 90 degrees. A leg that samples a calendar grid therefore tests
# almost nothing -- most rows carry a term smaller than the band, so both
# servers pass by not being asked. These bodies are searched for their smallest
# elongation instead (superior conjunction), and every row records the size of
# the term it tested, so a pass is legible as a pass.
DEFLECT_GEO_BODIES = [199, 299, 4, 5, 6]
# The Sun's disc is ~0.267 deg; a request inside it is meaningless, so a
# candidate closer than this is skipped rather than graded.
DEFLECT_MIN_ELONG_DEG = 0.30
DEFLECT_SCAN_START = 2451545.0
DEFLECT_SCAN_STEP_DAYS = 2.0
DEFLECT_SCAN_COUNT = 800  # ~4.4 years, so every body reaches conjunction
# One wide-elongation row per body, as a control: the term is then far below
# the band, and a leg that reports "agree" there is reporting that it tested
# nothing. Recorded with its term size so the distinction is on the record.
DEFLECT_CONTROL_ELONG_DEG = 60.0


# The two sites the topocentric observer is measured from: the pair that
# exposed the frozen topocentric orbit points. One mid-latitude, one on the
# equator at altitude -- so a site that never reaches the computation shows
# as the same answer at both, which is how that defect read.
DEFLECT_SITES = [("Zurich", "8.55,47.37,500"), ("Quito", "-78.47,-0.18,2850")]


def _elongation_scan(client, srv, body, verbose, where=()):
    """(jd, elongation deg) rows for one body against the Sun, from one
    server's own mask-1 answers. The search runs on OUR server only: it picks
    *where to look*, and both servers are then asked the same instants, so a
    bias in the scan cannot flatter either of them.

    `where` places the observer, and is the same site flag the graded rows
    carry: the elongation that decides where to look has to be the one the
    graded observer sees."""
    args = ["--jd", repr(DEFLECT_SCAN_START), "--step", repr(DEFLECT_SCAN_STEP_DAYS * 86400.0),
            "--count", str(DEFLECT_SCAN_COUNT), "--icrs", "--eq", "--corrections", "1",
            "--deltat", str(DELTA_T), "--obj", str(body), "--obj", "10"] + list(where)
    rep = ask(client, srv, args, verbose)
    out = []
    for r in range(DEFLECT_SCAN_COUNT):
        vb, vs = rep.row(0, r), rep.row(1, r)
        if vb is None or vs is None or math.isnan(vb[0]) or math.isnan(vs[0]):
            continue
        out.append((DEFLECT_SCAN_START + r * DEFLECT_SCAN_STEP_DAYS,
                    angle(vector(vb), vector(vs)) / 3600.0))
    return out


def _leg_deflection_at(client, ours, theirs, wel_a, wel_b, table, verbose,
                       leg, observer, bit, where, probe=None):
    """Deflection at one observer, against the same textbook formula the
    Jupiter leg uses: each server's mask-3 answer against its own mask-1
    answer bent by hand.

    The planet-centred leg found a 0.544" error because no anchor observes
    from Jupiter. At the Earth there is an anchor, but neither project had
    used one: the `apparent` leg grades the two servers against each other,
    which cannot see a term they both get wrong, and Horizons' apparent place
    carries frame offsets that swamp a 1.75" effect. Asked for by the Astrolog
    session, 2026-09-20, as the one place a textbook referee says something
    new about a term they do advertise.

    `bit` is the A.5 observer (0 geocentric, 1 topocentric) and `where` the
    flags that place it. Every request in a row carries `where` -- the body,
    the Sun and the elongation scan -- because the formula needs the
    observer-to-body and observer-to-Sun vectors of ONE observer; mixing a
    geocentric Sun into a topocentric row would test the mixture.

    `probe`, if given, collects each server's mask-1 direction by
    (who, body, jd), for the site-reach check below."""
    judged = {who: {1, 3} <= advertised(wel, bit)
              for who, wel in (("ours", wel_a), ("theirs", wel_b))}
    if not any(judged.values()):
        table.asked(None, None)
        print(f"\n== {leg}: FAIL, neither server lists masks 1 and 3 at {observer}")
        table.add(leg=leg, observer=observer, verdict="unanswered",
                  note=f"no server lists masks 1 and 3 at {observer}")
        return
    print(f"\n== {leg}: from {observer}, mask 3 against mask 1 + the textbook bending")
    print("   (searching each body's closest approach to the Sun, where the term is largest)")
    targets = []
    for b in DEFLECT_GEO_BODIES:
        scan = _elongation_scan(client, ours, b, verbose, where)
        near = [(e, jd) for jd, e in scan if e >= DEFLECT_MIN_ELONG_DEG]
        if not near:
            continue
        e_min, jd_min = min(near)
        targets.append((b, jd_min, e_min))
        far = [(abs(e - DEFLECT_CONTROL_ELONG_DEG), jd, e) for jd, e in scan]
        if far:
            _, jd_far, e_far = min(far)
            targets.append((b, jd_far, e_far))
    worst = {}
    tested = 0
    for b, jd, elong in sorted(targets, key=lambda t: (t[0], t[1])):
        args = ["--jd", repr(jd), "--icrs", "--eq", "--deltat", str(DELTA_T)] + list(where)
        got = {(who, m): ask(client, srv, args + ["--corrections", str(m), "--obj", str(b)],
                             verbose)
               for who, srv in (("ours", ours), ("theirs", theirs)) if judged[who]
               for m in (1, 3)}
        sun = {who: ask(client, srv, args + ["--corrections", "0", "--obj", "10"], verbose).row(0)
               for who, srv in (("ours", ours), ("theirs", theirs)) if judged[who]}
        table.asked(got.get(("ours", 3)), got.get(("theirs", 3)))
        base = dict(leg=leg, epoch_tt=jd, object=b, observer=observer, frame="ICRF",
                    plane="equator", mask=3, deltat=DELTA_T, tier=3,
                    anchor_source="textbook deflection (USNO Circular 179) on each server's "
                                  "own mask-1 answer")
        rows = {k: r.row(0) for k, r in got.items()}
        if any(v is None or math.isnan(v[0]) for v in rows.values()) or \
                any(v is None or math.isnan(v[0]) for v in sun.values()):
            table.add(**base, verdict="unanswered", note=f"elongation {elong:.3f} deg")
            continue
        res, term = {}, {}
        for who in [w for w, j in judged.items() if j]:
            p1 = vector(rows[(who, 1)])
            if probe is not None:
                probe[(who, b, jd)] = p1
            bent = textbook_deflection(p1, vector(sun[who]))
            # How big the term the row actually tested is: the angle the
            # textbook moved the mask-1 direction. A verdict is only worth as
            # much as this number.
            term[who] = angle(p1, bent)
            res[who] = angle(vector(rows[(who, 3)]), bent)
            worst[who] = max(worst.get(who, 0.0), res[who])
        t = max(term.values())
        if t > DEFLECTION_BAND:
            tested += 1
        ok = {w: res[w] <= DEFLECTION_BAND for w in res}
        verdict = ("agree" if all(ok.values()) else
                   "finding" if not any(ok.values()) else
                   "finding (ours)" if not ok.get("ours", True) else "finding (theirs)")
        note = (f"elongation {elong:.3f} deg; the textbook term here is {t:.4f}\""
                + ("" if t > DEFLECTION_BAND else
                   f", below the {DEFLECTION_BAND}\" band: this row grades nothing"))
        table.add(**base, sep_servers=angle(vector(rows[("ours", 3)]), vector(rows[("theirs", 3)]))
                  if len(res) == 2 else "",
                  sep_ours_anchor=res.get("ours", ""), sep_theirs_anchor=res.get("theirs", ""),
                  band=DEFLECTION_BAND, verdict=verdict, note=note)
        print(f"  body {b:3d}  JD {jd:.1f}  elong {elong:7.3f} deg  term {t:8.4f}\"  " +
              "  ".join(f"{w} {res[w]:.6f}\"" for w in sorted(res)))
    print(f"  rows whose term exceeds the band (so actually testing something): {tested}")
    for who, w in sorted(worst.items()):
        print(f"  worst {who} vs textbook: {w:.6f}\"")


def leg_deflection_geo(client, ours, theirs, wel_a, wel_b, table, verbose):
    """The geocentric observer, A.5 value 0."""
    _leg_deflection_at(client, ours, theirs, wel_a, wel_b, table, verbose,
                       "deflection-geo", "geo", 0, [])


# A body's diurnal parallax at these rows is arcseconds -- 0.8" for Saturn at
# conjunction, 6" for Mercury -- so a site that reaches the computation moves
# the answer thousands of times the deflection band. It can still be small for
# one row (a body near the site's zenith or nadir shifts along the line of
# sight), so what is graded is the LARGEST shift over the rows, not each one.
DEFLECT_SITE_PARALLAX_MIN = 0.5  # arcsec


def _deflection_site_reach(client, ours, theirs, table, verbose, seen):
    """Did the site reach the computation at all?

    Every row above grades a server against its own mask-1 answer, so a
    server that silently ignored `--topo` would hand back geocentric
    directions for both masks and the row would still read "agree". That is
    not a hypothetical: the topocentric orbit points passed their comparison
    for months while their position column never left the Earth's centre.
    So measure the two things that verdict cannot see -- the shift from the
    geocentric answer, and the shift between the two sites -- and grade them."""
    srv = {"ours": ours, "theirs": theirs}
    names = [n for n, _ in DEFLECT_SITES]
    keys = sorted(set.intersection(*[set(seen[n]) for n in names]))
    if not keys:
        return
    geo = {}
    for who, b, jd in keys:
        rep = ask(client, srv[who], ["--jd", repr(jd), "--icrs", "--eq", "--deltat", str(DELTA_T),
                                     "--corrections", "1", "--obj", str(b)], verbose)
        r = rep.row(0)
        geo[(who, b, jd)] = None if r is None or math.isnan(r[0]) else vector(r)
    whos = sorted({k[0] for k in keys})

    def grade(observer, shift, source, note):
        worst = {w: max([shift(k) for k in keys if k[0] == w and shift(k) is not None] or [0.0])
                 for w in whos}
        ok = {w: worst[w] >= DEFLECT_SITE_PARALLAX_MIN for w in whos}
        verdict = ("agree" if all(ok.values()) else
                   "finding" if not any(ok.values()) else
                   "finding (ours)" if not ok.get("ours", True) else "finding (theirs)")
        table.add(leg="deflection-topo", observer=observer, frame="ICRF", plane="equator",
                  mask=1, deltat=DELTA_T, tier=3, anchor_source=source,
                  sep_ours_anchor=worst.get("ours", ""), sep_theirs_anchor=worst.get("theirs", ""),
                  band=DEFLECT_SITE_PARALLAX_MIN, verdict=verdict, note=note)
        print(f"  {observer:24s} largest shift " +
              "  ".join(f"{w} {worst[w]:.4f}\"" for w in whos) + f"   {verdict}")

    print("\n== deflection-topo: did the site reach the computation?")
    table.asked(None, None)
    for name in names:
        grade(f"topo {name} vs geo",
              lambda k, n=name: (None if geo[k] is None else angle(seen[n][k], geo[k])),
              "the same server's geocentric mask-1 answer at the same instant",
              f"diurnal parallax at {name} over {len(keys) // len(whos)} rows; below the band "
              "would mean the site never reached the computation")
    grade("topo " + " vs ".join(names),
          lambda k: angle(seen[names[0]][k], seen[names[1]][k]),
          "the same server's own answer from the other site",
          "the two sites' answers differ; equal answers would mean one fixed site, "
          "which is how the frozen topocentric orbit points read")


def leg_deflection_topo(client, ours, theirs, wel_a, wel_b, table, verbose):
    """The topocentric observer, A.5 value 1, which both servers also
    advertise and neither project had refereed. The honest expectation is
    that it is the same code path as the geocentric one and therefore right;
    the reason to measure it anyway is that the topocentric orbit points were
    expected to be the same code path too, and their position column never
    reached the site while their rate column did (CROSS-TEST.md). Two sites,
    so a site that never reaches the computation reads as two equal answers
    rather than as a pass."""
    seen = {}
    for name, site in DEFLECT_SITES:
        seen[name] = {}
        _leg_deflection_at(client, ours, theirs, wel_a, wel_b, table, verbose,
                           "deflection-topo", f"topo {name}", 1, ["--topo", site], seen[name])
    if all(seen.values()):
        _deflection_site_reach(client, ours, theirs, table, verbose, seen)


# The arrival leg. Every numeric leg here grades a server against an anchor or
# against the other server, and the two legs that cannot -- deflection from a
# body centre, deflection topocentrically -- grade each server against its own
# other answer. That construction is what makes them portable, and it is also
# blind in one direction: it measures whether a server is consistent, never
# whether the argument reached the computation. The topocentric leg's three
# site rows were added for that, and this generalises them to the observers
# nothing else interrogates that way.
#
# Two questions, asked geometrically (mask 0) so no correction convention
# enters: did this observer move the answer at all, and are two observers that
# should differ actually distinct? Plus one that should NOT differ -- a
# heliocentric observer and an observer at the Sun's centre are the same place,
# and at mask 0 they have nothing left to disagree about.
ARRIVAL_EPOCHS = [2415020.5, 2451545.0, 2461300.5]
ARRIVAL_BODIES = [4, 5, 6, 399]
ARRIVAL_MASK = 0
# Arcseconds. Every real observer change here moves the answer by degrees; the
# floor is low because the row asks whether the argument arrived, not how far.
ARRIVAL_FLOOR = 1.0
# The two spellings of the Sun's centre have to agree this closely, which is
# the deflection band: at mask 0 nothing separates them.
ARRIVAL_SAME_BAND = 0.0002
# (name, A.5 observer, flags, the body it cannot observe)
ARRIVAL_OBSERVERS = [("geo", 0, [], None), ("helio", 2, ["--helio"], None),
                     ("bary", 3, ["--bary"], None),
                     ("centre Jupiter", 4, ["--center", "5"], 5),
                     ("centre Mars", 4, ["--center", "4"], 4),
                     ("centre Sun", 4, ["--center", "10"], 10)]
# Pairs that must differ, and why each one is worth a row:
#   bary vs helio   -- a server that treats the barycentre as the Sun
#   Jupiter vs Mars -- a server that honours "a body's centre" but not which
ARRIVAL_DISTINCT = [("bary", "helio"), ("centre Jupiter", "centre Mars")]
# And the pair that must agree: two spellings of one place.
ARRIVAL_SAME = [("centre Sun", "helio")]


def _shift_row(table, leg, observer, keys, whos, shift, band, want, source, note):
    """One graded row: the worst shift over `keys`, per server, against `band`.
    `want` is "at least" when the row asks whether an observer arrived, and
    "at most" when it asks whether two spellings of one observer agree."""
    worst = {}
    for w in whos:
        vals = [v for v in (shift(k) for k in keys if k[0] == w) if v is not None]
        worst[w] = max(vals) if vals else None
    ok = {w: (worst[w] >= band if want == "at least" else worst[w] <= band)
          for w in whos if worst[w] is not None}
    if not ok:
        verdict = "unanswered"
    elif all(ok.values()):
        verdict = "agree"
    elif not any(ok.values()):
        verdict = "finding"
    else:
        verdict = "finding (ours)" if not ok.get("ours", True) else "finding (theirs)"
    table.add(leg=leg, observer=observer, frame="ICRF", plane="equator", mask=ARRIVAL_MASK,
              deltat=DELTA_T, tier=3, anchor_source=source,
              sep_ours_anchor=worst.get("ours") if worst.get("ours") is not None else "",
              sep_theirs_anchor=worst.get("theirs") if worst.get("theirs") is not None else "",
              band=band, verdict=verdict, note=note)
    shown = "  ".join(f"{w} {worst[w]:.4f}\"" for w in sorted(worst) if worst[w] is not None)
    print(f"  {observer:34s} {want:8s} {band:<8g} {shown}   {verdict}")


def leg_arrival(client, ours, theirs, wel_a, wel_b, table, verbose):
    """Does each observer reach the computation, and are observers distinct?"""
    srv = {"ours": ours, "theirs": theirs}
    wel = {"ours": wel_a, "theirs": wel_b}
    seen = {}
    for name, bit, where, cannot in ARRIVAL_OBSERVERS:
        bodies = [b for b in ARRIVAL_BODIES if b != cannot]
        seen[name] = {}
        for who in ("ours", "theirs"):
            if ARRIVAL_MASK not in advertised(wel[who], bit):
                continue
            for jd in ARRIVAL_EPOCHS:
                args = ["--jd", repr(jd), "--icrs", "--eq", "--corrections", str(ARRIVAL_MASK),
                        "--deltat", str(DELTA_T)] + list(where)
                for b in bodies:
                    args += ["--obj", str(b)]
                rep = ask(client, srv[who], args, verbose)
                for i, b in enumerate(bodies):
                    r = rep.row(i)
                    if r is not None and not math.isnan(r[0]):
                        seen[name][(who, b, jd)] = vector(r)
    print("\n== arrival: did the observer argument reach the computation?")
    print("   (mask 0, so nothing but the observer's own position is in the answer)")
    whos = sorted({k[0] for v in seen.values() for k in v})
    table.asked(None, None)

    def pair(a, b, band, want, source, note):
        keys = sorted(set(seen[a]) & set(seen[b]))
        if not keys:
            return
        _shift_row(table, "arrival", f"{a} vs {b}", keys, whos,
                   lambda k: angle(seen[a][k], seen[b][k]), band, want, source, note)

    for name, _, _, _ in ARRIVAL_OBSERVERS:
        if name == "geo":
            continue
        pair(name, "geo", ARRIVAL_FLOOR, "at least",
             "the same server's geocentric answer at the same instant",
             "an observer that never reached the computation would answer geocentrically")
    for a, b in ARRIVAL_DISTINCT:
        pair(a, b, ARRIVAL_FLOOR, "at least", "the same server's answer at the other observer",
             "these two observers are different places; equal answers mean one of them "
             "was not read")
    for a, b in ARRIVAL_SAME:
        pair(a, b, ARRIVAL_SAME_BAND, "at most",
             "the same server's answer at the other spelling of the same place",
             "a heliocentric observer and an observer at the Sun's centre are the same "
             "place, and at mask 0 nothing separates them")


# Orbit points (kind 1), geometric (mask 0) in the frame of date: the portable
# comparison, where light-time conventions for a point stay out. Bands from
# the first measurement (2026-09-18, the corpus epochs), each with its reason.
POINT_SPECS = ["301.a.m", "301.d.m", "301.p.m", "301.A.m", "301.a.o", "301.d.o", "301.p.o",
               "301.A.o", "199.a.m", "199.p.m", "199.a.o", "199.p.o", "4.a.m", "4.p.m",
               "4.a.o", "4.p.o", "5.a.m", "5.p.m", "5.a.o", "5.p.o", "6.a.m", "6.p.m", "6.a.o",
               "6.p.o", "301.A.2", "301.p.2"]
POINT_BANDS = {
    # The Moon's mean points follow analytic mean elements on both sides,
    # its osculating points the ephemeris: measured <= 0.49", 1 km.
    "moon": (1.0, 5.0),
    # A planet's osculating points come from its state vector, so the
    # refit's km-level position and velocity errors reach the node and
    # perihelion directions amplified: measured <= 1.8", 780 km.
    "osculating": (3.0, 1500.0),
    # Mean elements of the inner planets and Mars: measured <= 1.3", 405 km.
    "mean": (2.0, 500.0),
}


# The natural apsides (A.14 method 2): two published readings of "between
# the actual passages". Ours interpolates the passages over a deviation model
# fitted to DE440; Swiss's is analytic (its manual, section 2.2.4). Both pass
# the actual passages within 0.016 deg; between them, measured 1900-2100,
# apogee <= 0.10 deg and perigee <= 2.8 deg apart. The bands are estimates
# at 1.5x that. Distance is recorded, not graded.
NATURAL_BANDS = {"A": 540.0, "p": 15000.0}


def point_class(spec):
    naif, point, method = spec.split(".")
    if method == "2":
        return "natural"
    if naif == "301":
        return "moon"
    if method == "o":
        return "osculating"
    # Mean elements are model-defined: ours are fitted to DE440, Swiss's are
    # VSOP87's (its published manual, which says the two "are considerable"
    # apart); the giant planets differ by up to 3300" (Saturn's perihelion).
    return "mean" if naif in ("199", "299", "4") else "model"


# At the Moon's actual passages the natural apsides are the Moon itself (the
# published definition). Graded per server: ours is exact by construction, so
# the band is the passage finder's 1e-7-day timing (~5 mas); Swiss's measured
# worst is 0.016 deg over 1990-2010, so 72" is an estimate.
PASSAGE_BAND = {"ours": 0.05, "theirs": 72.0}


def natural_at_passages(client, ours, theirs, table, verbose, epochs):
    """The next apogee and perigee passage after each epoch, found from our
    server's geometric distance rate by bisection; there, each server's
    natural point against its own Moon."""
    print("  natural apsides at the Moon's actual passages (each server against its own Moon)")
    base_args = ["--corrections", "0", "--j2000", "--deltat", str(DELTA_T)]

    def rate(jd):
        r = ask(client, ours, ["--jd", repr(jd), "--obj", "301"] + base_args, verbose)
        return r.row(0)[5]

    worst = {"ours": 0.0, "theirs": 0.0}
    for jd0 in epochs:
        scan = ask(client, ours, ["--jd", repr(jd0), "--step", "86400", "--count", "32",
                                  "--obj", "301"] + base_args, verbose)
        found = {}
        for i in range(31):
            a, b = scan.row(0, i), scan.row(0, i + 1)
            if a is None or b is None or (a[5] > 0) == (b[5] > 0):
                continue
            kind = "A" if a[5] > 0 else "p"
            if kind in found:
                continue
            lo, hi, flo = jd0 + i, jd0 + i + 1, a[5]
            while hi - lo > 1e-7:
                mid = 0.5 * (lo + hi)
                fm = rate(mid)
                if (fm > 0) == (flo > 0):
                    lo, flo = mid, fm
                else:
                    hi = mid
            found[kind] = 0.5 * (lo + hi)
        for kind, tp in sorted(found.items()):
            args = ["--jd", repr(tp), "--node", f"301.{kind}.2", "--obj", "301"] + base_args
            ra, rb = ask(client, ours, args, verbose), ask(client, theirs, args, verbose)
            table.asked(ra, rb)
            seps = {}
            for who, r in (("ours", ra), ("theirs", rb)):
                n, m = r.row(0), r.row(1)
                if n is None or m is None or math.isnan(n[0]) or math.isnan(m[0]):
                    seps[who] = None
                    continue
                seps[who] = sep_arcsec((n[0], n[1]), (m[0], m[1]))
                worst[who] = max(worst[who], seps[who])
            base = dict(leg="points", epoch_tt=tp, object=f"301.{kind}.2 at passage",
                        observer="geo", frame="J2000", plane="ecliptic", mask=0,
                        deltat=DELTA_T, tier=2)
            if seps["ours"] is None or seps["theirs"] is None:
                table.add(**base, verdict="unanswered")
                continue
            ok_o = seps["ours"] <= PASSAGE_BAND["ours"]
            ok_t = seps["theirs"] <= PASSAGE_BAND["theirs"]
            table.add(**base, sep_ours_anchor=seps["ours"], sep_theirs_anchor=seps["theirs"],
                      anchor_source="the Moon at its actual passage",
                      band=f"ours {PASSAGE_BAND['ours']}\" theirs {PASSAGE_BAND['theirs']}\"",
                      verdict="agree" if ok_o and ok_t else
                      "finding (ours)" if not ok_o else "finding (theirs)",
                      note="natural point against the Moon at the passage, each server its own")
    print(f"    worst: ours {worst['ours']:.4f}\"  theirs {worst['theirs']:.2f}\"")


POINT_OBSERVERS = [("helio", ["--helio"]), ("bary", ["--bary"]), ("Mars", ["--center", "4"])]
# The Moon's points ride with the Earth, within its osculating apogee (~0.0029
# AU at most) of its centre; so from anywhere each must lie within that of the
# Earth answered in the same request, in direction and distance.
MOON_REACH_AU = 0.003
# With light time (mask 1) a Moon point is retarded as the Earth is: its own
# motion about the Earth over ~500 s moves it <= 0.5" more (osculating apsides
# swing fastest). Measured on ours <= 0.19" (the osculating apogee from Mars,
# 2026-09-18).
MOON_RETARD_BAND = 1.0


def _moon_anchor(v, earth, v0, earth0):
    """Why a Moon point v (mask m) is inconsistent with the Earth answered
    beside it, or "" when it is not. v0/earth0: the same at mask 0, when the
    row is mask 1 and both are at hand (the light-time check)."""
    lim = math.degrees(math.asin(min(1.0, MOON_REACH_AU / earth[2]))) * 3600.0
    s = sep_arcsec((v[0], v[1]), (earth[0], earth[1]))
    if s > lim or abs(v[2] - earth[2]) > MOON_REACH_AU:
        return f"{s:.0f}\" and {abs(v[2] - earth[2]):.4f} AU from the Earth (reach {lim:.0f}\")"
    if v0 is not None and earth0 is not None:
        moved = sep_arcsec((v[0], v[1]), (v0[0], v0[1]))
        e_moved = sep_arcsec((earth[0], earth[1]), (earth0[0], earth0[1]))
        if abs(moved - e_moved) > MOON_RETARD_BAND:
            return (f"light time moves it {moved:.2f}\", the Earth {e_moved:.2f}\" "
                    f"(band {MOON_RETARD_BAND}\")")
    return ""


def within_but_lighttime(sep, dkm):
    """The two servers differ by about one light time's worth of the Earth's
    motion from the Sun (20.5", 15,000 km at 1 AU), and no more."""
    return sep <= 25.0 and dkm <= 20000.0


def points_from_elsewhere(client, ours, theirs, table, verbose, epochs):
    """The same points seen from the Sun, the barycentre and Mars's centre, at
    mask 0 and mask 1. A point is a place in space (ORBIT-POINTS.md), so from
    elsewhere it is re-centred like a body, and mask 1 adds light time and
    nothing else: 3.5a honours the bits as sent. A server may refuse an
    observer per object (errCode); that is recorded, not graded. The Moon's
    points are also held to the Earth answered in the same request
    (_moon_anchor), which says whose a difference is."""
    print("\n  from elsewhere: Sun, barycentre, Mars; masks 0 and 1")
    specs = [s for s in POINT_SPECS if point_class(s) != "natural"]
    replies = {}
    for obs, oargs in POINT_OBSERVERS:
        for mask in (0, 1):
            for jd in epochs:
                args = ["--jd", repr(jd), "--corrections", str(mask), "--deltat", str(DELTA_T)]
                args += oargs
                for spec in specs:
                    args += ["--node", spec]
                args += ["--obj", "399"]
                ra, rb = ask(client, ours, args, verbose), ask(client, theirs, args, verbose)
                replies[obs, mask, jd] = (ra, rb)

    def ok(v):
        return v is not None and not any(math.isnan(x) for x in v[:3])

    worst = {}
    for (obs, mask, jd), (ra, rb) in replies.items():
        table.asked(ra, rb)
        r0 = replies.get((obs, 0, jd)) if mask == 1 else None
        for k, spec in enumerate(specs):
            cls = point_class(spec)
            va, vb = ra.row(k), rb.row(k)
            base = dict(leg="points-observer", epoch_tt=jd, object=spec, observer=obs,
                        frame="true of date", plane="ecliptic", mask=mask,
                        deltat=DELTA_T, tier=2)
            if not ok(va) or not ok(vb):
                ea = ra.objects[k].err if k < len(ra.objects) else -1
                eb = rb.objects[k].err if k < len(rb.objects) else -1
                who = "ours" if ok(vb) else "theirs" if ok(va) else "both"
                table.add(**base, verdict=f"refused ({who})",
                          note=f"errCode ours {ea} theirs {eb}")
                continue
            s = sep_arcsec((va[0], va[1]), (vb[0], vb[1]))
            dkm = abs(va[2] - vb[2]) * AU_KM
            w = worst.setdefault((obs, mask, cls), [0.0, 0.0, ""])
            if s > w[0]:
                w[0], w[2] = s, spec
            w[1] = max(w[1], dkm)
            note = f"distance diff {dkm:.1f} km"
            if cls == "model":
                table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]),
                          sep_servers=s, verdict="expected-difference",
                          note=note + "; mean elements from two published sources")
                continue
            band_s, band_km = POINT_BANDS[cls]
            within = s <= band_s and dkm <= band_km
            if cls == "moon":
                why = {}
                held = {}  # was there an Earth beside it to be held to?
                r_of = {"ours": ra, "theirs": rb}
                for who, r, i in (("ours", ra, 0), ("theirs", rb, 1)):
                    n = len(specs)
                    v0 = e0 = None
                    if r0 is not None:
                        v0, e0 = r0[i].row(k), r0[i].row(n)
                        if not ok(v0) or not ok(e0):
                            v0 = e0 = None
                    held[who] = ok(r.row(n))
                    why[who] = _moon_anchor(r.row(k), r.row(n), v0, e0) if held[who] else ""
                bad = [w for w in ("ours", "theirs") if why[w]]
                # corrApplied states what applies to an object structurally
                # (SERVER.md): a server that says light time does not apply
                # to a point answers mask 1 as mask 0, and says so. That
                # statement stands on its own. It does NOT need the Earth
                # beside it: a server refuses the Earth at the edge of its
                # coverage, and requiring both pieces of evidence turned six
                # rows at 1800 into unattributed findings on 2026-09-20,
                # reading as ours when their own corrApplied explained them.
                nolt = [w for w in ("ours", "theirs")
                        if not (r_of[w].objects[k].corr & 1)] if mask == 1 else []
                declared = [w for w in bad if why[w].startswith("light time") and w in nolt]
                note += "".join(f"; {w}: {why[w]}" for w in bad)
                if bad and len(declared) == len(bad) and within_but_lighttime(s, dkm):
                    table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]),
                              sep_servers=s, verdict="expected-difference",
                              note=note + f"; {declared[0]}: corrApplied has no light time "
                                          "for this point, so mask 1 is its mask 0")
                    continue
                if not bad and not within and len(nolt) == 1 and within_but_lighttime(s, dkm):
                    table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]),
                              sep_servers=s, verdict="expected-difference",
                              note=note + f"; {nolt[0]}: corrApplied has no light time for "
                                          "this point, so mask 1 is its mask 0" +
                                   ("" if all(held.values()) else
                                    f"; {[w for w in held if not held[w]][0]} refused the "
                                    "Earth here, so corrApplied is the whole evidence"))
                    continue
                if not bad and not within and not all(held.values()):
                    # Nothing to attribute with: the anchor is missing and no
                    # corrApplied explains the gap. Say so rather than letting
                    # it read as the other server's defect.
                    table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]),
                              sep_servers=s, band=f"{band_s}\" {band_km} km",
                              verdict="unadjudicated",
                              note=note + f"; {[w for w in held if not held[w]][0]} refused "
                                          "the Earth here, so neither side can be held to it")
                    continue
                verdict = "agree" if within and not bad else \
                    f"finding ({bad[0]})" if len(bad) == 1 else "finding"
            else:
                verdict = "agree" if within else "finding"
            table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]), sep_servers=s,
                      band=f"{band_s}\" {band_km} km", verdict=verdict, note=note)
    for (obs, mask, cls), (s, d, spec) in sorted(worst.items()):
        print(f"    {obs:6s} mask {mask} {cls:10s} worst {s:10.3f}\" ({spec})  {d:11.1f} km")


def leg_points(client, ours, theirs, table, verbose):
    """Orbit points by direction AND distance, mask 0, true ecliptic of date;
    then 3.5a's rule, as amended on 2026-09-18, that a node lies on the mean
    ecliptic of date whatever the frame: the Moon's mean node asked in J2000
    must be the same point on both servers."""
    epochs = sorted({p[0] for _, _, pts in corpus() for p in pts})
    print("\n== points: orbit points, mask 0, true ecliptic of date, direction and distance")
    worst = {}
    for spec in POINT_SPECS:
        cls = point_class(spec)
        for jd in epochs:
            args = ["--jd", repr(jd), "--corrections", "0", "--deltat", str(DELTA_T), "--node", spec]
            ra, rb = ask(client, ours, args, verbose), ask(client, theirs, args, verbose)
            table.asked(ra, rb)
            va, vb = ra.row(0), rb.row(0)
            base = dict(leg="points", epoch_tt=jd, object=spec, observer="geo",
                        frame="true of date", plane="ecliptic", mask=0, deltat=DELTA_T)
            if va is None or vb is None or any(math.isnan(v) for v in va[:3] + vb[:3]):
                ea = ra.objects[0].err if ra.objects else -1
                eb = rb.objects[0].err if rb.objects else -1
                table.add(**base, verdict="unanswered", note=f"errCode ours {ea} theirs {eb}")
                continue
            s = sep_arcsec((va[0], va[1]), (vb[0], vb[1]))
            dkm = abs(va[2] - vb[2]) * AU_KM
            w = worst.setdefault(spec, [0.0, 0.0])
            w[0], w[1] = max(w[0], s), max(w[1], dkm)
            note = f"distance ours {va[2] * AU_KM:.1f} km theirs {vb[2] * AU_KM:.1f} km, diff {dkm:.1f} km"
            if cls == "natural":
                band_s = NATURAL_BANDS[spec.split(".")[1]]
                table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]), sep_servers=s,
                          band=f"{band_s}\"", tier=2,
                          verdict="expected-difference" if s <= band_s else "finding",
                          note=note + "; two published readings of the natural apse (ours "
                                      "interpolated over a DE440 model, Swiss's analytic)")
                continue
            if cls == "model":
                table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]), sep_servers=s,
                          tier=2, verdict="expected-difference",
                          note=note + "; mean elements from two published sources: ours "
                                      "fitted to DE440, Swiss's from VSOP87 (Swiss manual)")
                continue
            band_s, band_km = POINT_BANDS[cls]
            verdict = "agree" if s <= band_s and dkm <= band_km else "finding"
            table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]), sep_servers=s,
                      band=f"{band_s}\" {band_km} km", tier=2, verdict=verdict, note=note)
    for spec, (s, d) in worst.items():
        print(f"  {spec:8s} worst {s:9.3f}\"  {d:11.1f} km  ({point_class(spec)})")

    natural_at_passages(client, ours, theirs, table, verbose, epochs)
    points_from_elsewhere(client, ours, theirs, table, verbose, epochs)

    print("  node of date in the J2000 frame (3.5a as amended: the frame gives the coordinates)")
    band_s, _ = POINT_BANDS["moon"]
    for jd in epochs:
        args = ["--jd", repr(jd), "--corrections", "0", "--deltat", str(DELTA_T), "--j2000",
                "--node", "301.a.m"]
        ra, rb = ask(client, ours, args, verbose), ask(client, theirs, args, verbose)
        table.asked(ra, rb)
        va, vb = ra.row(0), rb.row(0)
        base = dict(leg="points-frame", epoch_tt=jd, object="301.a.m", observer="geo",
                    frame="J2000", plane="ecliptic", mask=0, deltat=DELTA_T, tier=2)
        if va is None or vb is None:
            table.add(**base, verdict="unanswered")
            continue
        s = sep_arcsec((va[0], va[1]), (vb[0], vb[1]))
        table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]), sep_servers=s,
                  band=band_s, verdict="agree" if s <= band_s else "finding",
                  note=f"J2000 latitude ours {va[1] * 3600.0:+.4f}\" theirs {vb[1] * 3600.0:+.4f}\"; "
                       "a node lies on the ecliptic of date in every frame (3.5a, amended "
                       "2026-09-18), so a server on the J2000 ecliptic misses by up to 680\"")


# Fixed stars by name, apparent place. Both sides read Hipparcos-derived
# catalogues with proper motion, so the band is small: measured <= 0.008"
# over 1900-2100 (2026-09-18) for the 29 stars below.
STARS = ["Aldebaran", "Regulus", "Spica", "Antares", "Fomalhaut", "Sirius", "Algol", "Vega",
         "Polaris", "Betelgeuse", "Rigel", "Arcturus", "Canopus", "Achernar", "Deneb", "Altair",
         "Pollux", "Castor", "Procyon", "Capella", "Alcyone", "Zubenelgenubi", "Zubeneschamali",
         "Bellatrix", "Acrux", "Hadar", "Mirach", "Alphecca", "Scheat"]
STAR_BAND = 0.02


def _star_records():
    """name -> catalog record, and hip -> record (src/star_catalog.inc)."""
    pat = re.compile(r'\s*\{\d+, \d+, (\d+), [^"]*"[^"]*", \d+, \d+, [^,]+, ([^,]+), ([^,]+), '
                     r'([^,]+), ([^,]+), ([^,]+), .*"([^"]*)"\},$')
    by_name, by_hip = {}, {}
    # The pattern above is read against the entries it walks past, not
    # trusted. A regex over a generated file rots toward green: change the
    # record's layout and it stops matching, the star list shrinks, and the
    # leg reports "40 of 40 agree" while testing a fifth of the catalogue.
    # Counting the candidate lines inside kStarRecords[] and requiring a
    # match for every one of them makes the same drift fail loudly instead.
    entries = matched = 0
    inside = False
    with open(os.path.join(REPO, "src", "star_catalog.inc")) as f:
        for line in f:
            if line.startswith("constexpr StarRecord kStarRecords[]"):
                inside = True
                continue
            if inside and line.startswith("};"):
                inside = False
                continue
            if not inside or not line.lstrip().startswith("{"):
                continue
            entries += 1
            m = pat.match(line)
            if not m:
                continue
            matched += 1
            g = m.groups()
            rec = {"hip": int(g[0]), "ra": float(g[1]), "dec": float(g[2]),
                   "epoch": float(g[3]), "pmra": float(g[4]), "pmdec": float(g[5])}
            by_hip.setdefault(rec["hip"], rec)
            for n in g[6].split("|"):
                if n:
                    by_name.setdefault(n, rec)
    if not entries:
        raise SystemExit("src/star_catalog.inc: found no kStarRecords entries at all; "
                         "the catalogue's layout has changed and this reader has not")
    if matched != entries:
        raise SystemExit(
            f"src/star_catalog.inc holds {entries} star records and this reader parsed "
            f"{matched} of them. The stars leg would run on the {matched} it understood and "
            f"report them as the whole catalogue. Fix the pattern in _star_records().")
    return by_name, by_hip


_ORBITS = None


def orbit_bend(name, jd):
    """How far our orbit model moves a binary from its catalog's straight line
    at jd (arcsec), or None for a star without one."""
    global _ORBITS
    if _ORBITS is None:
        by_name, by_hip = _star_records()
        orbits = binary_orbits.load(os.path.join(REPO, "stars-raw"))
        _ORBITS = {n: (o, rec, by_hip.get(o["partner"]) if o["partner"] else None)
                   for n, rec in by_name.items()
                   for h, o in orbits.items() if rec["hip"] == h}
    entry = _ORBITS.get(name)
    if entry is None:
        return None
    o, rec, partner = entry
    epoch = 2451545.0 + (rec["epoch"] - 2000.0) * 365.25
    if partner:  # a secondary, placed from its primary
        orbits = binary_orbits.load(os.path.join(REPO, "stars-raw"))
        e, n = binary_orbits.bend(o, jd, epoch, binary_orbits.line_delta(rec, partner, jd),
                                  orbits[partner["hip"]],
                                  2451545.0 + (partner["epoch"] - 2000.0) * 365.25)
    else:
        e, n = binary_orbits.bend(o, jd, epoch)
    return math.hypot(e, n)
# alpha Cen A and B: the two sides take them from different catalogues (ours
# Hipparcos, theirs SIMBAD), for the fastest-moving bright pair in the sky; an
# estimate, measured 0.007" (A) and 0.040" (B) at J2000, 2026-09-18.
ALCEN_BAND = 0.1
STAR_EPOCHS = [2415020.5, 2451545.0, 2488069.5]


def leg_stars(client, ours, theirs, table, verbose):
    """Fixed stars by name, apparent, tropical and Lahiri; then alpha Centauri,
    where the catalogues differ in what a name means: the IAU's names are
    Rigil Kentaurus for alpha Cen A and Toliman for alpha Cen B."""
    print("\n== stars: fixed stars by name, apparent, true ecliptic of date")
    for zodiac in ("", "lahiri"):
        worst = 0.0
        for jd in STAR_EPOCHS:
            args = ["--jd", repr(jd), "--corrections", "7", "--deltat", str(DELTA_T)]
            if zodiac:
                args += ["--sid", zodiac]
            for name in STARS:
                args += ["--star", name]
            ra, rb = ask(client, ours, args, verbose), ask(client, theirs, args, verbose)
            table.asked(ra, rb)
            for k, name in enumerate(STARS):
                va, vb = ra.row(k), rb.row(k)
                base = dict(leg="stars", epoch_tt=jd, object=name, observer="geo",
                            frame=zodiac or "tropical", plane="ecliptic", mask=7,
                            deltat=DELTA_T, tier=2)
                if va is None or vb is None or any(math.isnan(v) for v in va[:2] + vb[:2]):
                    ea = ra.objects[k].err if k < len(ra.objects) else -1
                    eb = rb.objects[k].err if k < len(rb.objects) else -1
                    table.add(**base, verdict="unanswered", note=f"errCode ours {ea} theirs {eb}")
                    continue
                sep = sep_arcsec((va[0], va[1]), (vb[0], vb[1]))
                bent = orbit_bend(name, jd)
                if bent is not None:
                    # Both sides move these on their orbits (ours STARS.md,
                    # "Binary stars"; theirs astrolog 13d3e5e), so they agree
                    # like any star, and the separation is a direction check.
                    # Before that, the servers differed by the bend: a row at
                    # the bend's size now means one side lost its orbit.
                    table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]), sep_servers=sep,
                              band=STAR_BAND, verdict="agree" if sep <= STAR_BAND else "finding",
                              note=f"a binary on its orbit on both sides; the orbit bends the "
                                   f"straight line by {bent:.3f}\" here")
                    worst = max(worst, sep)
                    continue
                worst = max(worst, sep)
                table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]), sep_servers=sep,
                          band=STAR_BAND, verdict="agree" if sep <= STAR_BAND else "finding")
        print(f"  {zodiac or 'tropical':9s} {len(STARS)} stars worst {worst:.4f}\"")

    # alpha Centauri: the same two names on both servers, astrometric J2000.
    args = ["--jd", "2451545.0", "--icrs", "--eq", "--corrections", "0", "--deltat",
            str(DELTA_T), "--star", "Rigil Kentaurus", "--star", "Toliman"]
    ra, rb = ask(client, ours, args, verbose), ask(client, theirs, args, verbose)
    table.asked(ra, rb)
    rows = [(ra.row(k), rb.row(k)) for k in range(2)]
    if any(v is None for pair in rows for v in pair):
        table.add(leg="stars-alcen", verdict="unanswered", note="alpha Centauri not answered")
        return
    (a_o, a_t), (b_o, b_t) = rows
    ours_split = sep_arcsec((a_o[0], a_o[1]), (b_o[0], b_o[1]))
    theirs_split = sep_arcsec((a_t[0], a_t[1]), (b_t[0], b_t[1]))
    table.add(leg="stars-alcen", epoch_tt=2451545.0, object="Rigil Kentaurus", frame="ICRF",
              plane="equator", mask=0, ours=(a_o[0], a_o[1]), theirs=(a_t[0], a_t[1]),
              sep_servers=sep_arcsec((a_o[0], a_o[1]), (a_t[0], a_t[1])), tier=3,
              verdict="agree" if sep_arcsec((a_o[0], a_o[1]), (a_t[0], a_t[1])) <= ALCEN_BAND
              else "finding",
              note="the IAU's Rigil Kentaurus is alpha Cen A itself (astrolog 554288b); both "
                   f"sides add the AB orbit's bend, {orbit_bend('Rigil Kentaurus', 2451545.0):.3f}\" "
                   "here")
    table.add(leg="stars-alcen", epoch_tt=2451545.0, object="Toliman", frame="ICRF",
              plane="equator", mask=0, ours=(b_o[0], b_o[1]), theirs=(b_t[0], b_t[1]),
              sep_servers=sep_arcsec((b_o[0], b_o[1]), (b_t[0], b_t[1])), tier=3,
              verdict="agree" if theirs_split > 1.0 and
              sep_arcsec((b_o[0], b_o[1]), (b_t[0], b_t[1])) <= ALCEN_BAND else "finding",
              note=f"the IAU's Toliman is alpha Cen B; A to B is {ours_split:.2f}\" here "
                   f"and {theirs_split:.2f}\" on theirs (0 = both names answer one star)")
    print(f"  alpha Cen: A-B {ours_split:.2f}\" ours, {theirs_split:.2f}\" theirs")


# Sidereal planes (A.8). Planes 0 and 1 are the same question on both sides
# and are judged by the refit band. Plane 2's origin is 3.5a Part A (approved
# 2026-09-18): the zodiac's zero point projected onto the plane. A constant
# offset across bodies with the planes agreeing in latitude is the Astrolog
# side's pending fix, and agrees once the offset closes.
SIDEREAL_BODIES = [10, 301, 4, 5, 6]
# What a sidereal rotation may add to the tropical gap: the two ayanamsa
# series differ by their precession models, 0.0026" over 1800-2200
# (docs/FRAMES.md, measured against swetest -ay).
SIDEREAL_EXTRA = 0.003
INVARIABLE_LAT_BAND = 0.05  # arcsec: the two sides' plane orientations
INVARIABLE_SPREAD_BAND = 0.02  # arcsec: a constant origin offset, across bodies


# Rates: a different oracle from every other leg, which grade positions
# only. Each server's reported rates are checked against central differences
# of its own positions at t -/+ h, so a rate with the wrong sign, unit or
# frame shows in either server alone. Graded per server, not against the
# other server: the two agree on positions to milliarcseconds, and a rate
# error is invisible to a position check.
RATE_BAND_DEG = 1e-6  # deg/day: 3.6 mas/day
# A topocentric Moon's diurnal parallax sets our three-point truncation:
# measured 2.9e-6 deg/day (ENGINE.md, "Rates"), so an estimate of twice that.
RATE_BAND_TOPO_DEG = 6e-6
RATE_BAND_AU = 1e-9  # AU/day per AU of distance (150 m/day at 1 AU)
# 1/1024 day (84.375 s): every row time is then exact in f64 (a JD near
# 2.45e6 resolves only ~40 us, and at h = 0.001 d that jitter alone showed as
# 7e-7 deg/day on the Moon). Truncation is ~1e-8 deg/day for the Moon.
RATE_H_DAYS = 1.0 / 1024.0
RATE_EPOCHS = [2451545.0, 2461300.5]
RATE_OBJECTS = [["--obj", "10"], ["--obj", "301"], ["--obj", "199"], ["--obj", "4"],
                ["--obj", "5"], ["--obj", "9"], ["--node", "301.a.m"], ["--node", "4.p.m"],
                ["--star", "Sirius"]]
RATE_LABELS = ["Sun", "Moon", "Mercury", "Mars", "Jupiter", "Pluto", "Moon mean node",
               "Mars mean perihelion", "Sirius"]
RATE_CONFIGS = [
    ("geo apparent, true of date, ecliptic", ["--corrections", "7"]),
    ("geo apparent, true of date, equatorial", ["--corrections", "7", "--eq"]),
    ("geo astrometric, ICRF, equatorial", ["--corrections", "1", "--icrs", "--eq"]),
    ("geo apparent, J2000, ecliptic", ["--corrections", "7", "--j2000"]),
    # mask 1: from the Sun, the advertised masks (0x0004, 0x0014) differ by
    # kind, and 3.5a makes any unlisted mask ERROR 11 for the whole request;
    # mask 1 is listed for every kind in this leg, by both servers
    ("helio astrometric, true of date, ecliptic", ["--helio", "--corrections", "1"]),
    ("topo Zurich, apparent", ["--corrections", "7", "--topo", "8.55,47.37,500"]),
    ("geo apparent, lahiri", ["--corrections", "7", "--sid", "lahiri"]),
    ("geo apparent, true-citra", ["--corrections", "7", "--sid", "true-citra"]),
]


def _rate_error(v):
    """Worst of |reported rate - central difference| for lon, lat (deg/day)
    and dist (AU/day), from rows t-2h .. t+2h; None if any row is missing.
    Five points (error ~h^4 f5/30): a topocentric Moon's diurnal parallax
    makes the three-point difference's h^2 f3/6 about 4e-5 deg/day here."""
    if any(r is None or any(math.isnan(x) for x in r) for r in v):
        return None
    mid = v[2]

    def d(i, wrap=False):
        f = [r[i] for r in v]
        if wrap:
            f = [f[2] + ((x - f[2] + 180.0) % 360.0 - 180.0) for x in f]
        return (f[0] - 8.0 * f[1] + 8.0 * f[3] - f[4]) / (12.0 * RATE_H_DAYS)
    # distance: relative to the distance, as a star's is f64 noise at ~5e5 AU
    return (abs(mid[3] - d(0, True)), abs(mid[4] - d(1)),
            abs(mid[5] - d(2)) / max(1.0, mid[2]))


def leg_rates(client, ours, theirs, table, verbose):
    print(f"\n== rates: each server's rates against its own positions at t -/+ {RATE_H_DAYS} d"
          " (lon/lat deg/day; dist AU/day divided by the distance in AU)")
    worst = {"ours": [0.0, 0.0, 0.0], "theirs": [0.0, 0.0, 0.0]}
    for label, cfg in RATE_CONFIGS:
        for jd in RATE_EPOCHS:
            args = ["--jd", repr(jd - 2 * RATE_H_DAYS), "--step", repr(RATE_H_DAYS * 86400.0),
                    "--count", "5", "--deltat", str(DELTA_T)] + cfg
            # Seen from the Sun, the Sun is the observer: astrolog-ephd then
            # refuses the whole request, so it is left out there.
            objs = [(o, n) for o, n in zip(RATE_OBJECTS, RATE_LABELS)
                    if not ("--helio" in cfg and n == "Sun")]
            for o, _ in objs:
                args += o
            ra, rb = ask(client, ours, args, verbose), ask(client, theirs, args, verbose)
            table.asked(ra, rb)
            for k, (_, name) in enumerate(objs):
                ea = _rate_error([ra.row(k, r) for r in range(5)])
                eb = _rate_error([rb.row(k, r) for r in range(5)])
                base = dict(leg="rates", epoch_tt=jd, object=name, frame=label, deltat=DELTA_T,
                            tier=2)
                if ea is None or eb is None:
                    table.add(**base, verdict="unanswered",
                              note="rows missing: ours %s theirs %s" % (ea is None, eb is None))
                    continue
                for who, e in (("ours", ea), ("theirs", eb)):
                    for i in range(3):
                        worst[who][i] = max(worst[who][i], e[i])
                bd = RATE_BAND_TOPO_DEG if "--topo" in cfg else RATE_BAND_DEG
                ok_a = ea[0] <= bd and ea[1] <= bd and ea[2] <= RATE_BAND_AU
                ok_b = eb[0] <= bd and eb[1] <= bd and eb[2] <= RATE_BAND_AU
                table.add(**base, ours="%.2e/%.2e/%.2e" % ea, theirs="%.2e/%.2e/%.2e" % eb,
                          band=f"{bd} deg/d, {RATE_BAND_AU} AU/d per AU",
                          verdict="agree" if ok_a and ok_b else
                          "finding" if not ok_a else "finding (theirs)",
                          note="|rate - central difference| lon/lat deg/day, dist AU/day per AU")
                if verbose or not (ok_a and ok_b):
                    print(f"  {label:40s} {jd:.1f} {name:15s} ours {ea[0]:.1e} {ea[1]:.1e} "
                          f"{ea[2]:.1e}  theirs {eb[0]:.1e} {eb[1]:.1e} {eb[2]:.1e}")
    for who in ("ours", "theirs"):
        w = worst[who]
        print(f"  {who:6s} worst: lon {w[0]:.2e} lat {w[1]:.2e} deg/day, dist {w[2]:.2e} AU/day")


def leg_sidereal(client, ours, theirs, table, verbose):
    """Planes 0 and 1 are judged by what the sidereal rotation adds: each row
    against the same body's tropical gap at that instant (the ephemerides'
    own difference, the Moon's ~5 mas included) plus SIDEREAL_EXTRA."""
    epochs = sorted({p[0] for _, _, pts in corpus() for p in pts})
    print("\n== sidereal: the three A.8 planes, Fagan-Bradley and Lahiri, apparent")
    tropical = {}
    for jd in epochs:
        args = ["--jd", repr(jd), "--corrections", "7", "--deltat", str(DELTA_T)]
        for b in SIDEREAL_BODIES:
            args += ["--obj", str(b)]
        ra, rb = ask(client, ours, args, verbose), ask(client, theirs, args, verbose)
        for k, b in enumerate(SIDEREAL_BODIES):
            va, vb = ra.row(k), rb.row(k)
            if va is not None and vb is not None and not math.isnan(va[0] + vb[0]):
                tropical[(jd, b)] = sep_arcsec((va[0], va[1]), (vb[0], vb[1]))
    for zodiac in ("fagan-bradley", "lahiri"):
        for plane in ("date", "anchor", "invariable"):
            worst = 0.0
            offsets = []
            for jd in epochs:
                args = ["--jd", repr(jd), "--corrections", "7", "--deltat", str(DELTA_T),
                        "--sid", zodiac, "--sid-plane", plane]
                for b in SIDEREAL_BODIES:
                    args += ["--obj", str(b)]
                ra, rb = ask(client, ours, args, verbose), ask(client, theirs, args, verbose)
                table.asked(ra, rb)
                rows = []
                for k, b in enumerate(SIDEREAL_BODIES):
                    va, vb = ra.row(k), rb.row(k)
                    base = dict(leg="sidereal", epoch_tt=jd, object=b, observer="geo",
                                frame=f"{zodiac} {plane}", plane="ecliptic", mask=7,
                                deltat=DELTA_T, tier=2)
                    if va is None or vb is None or any(math.isnan(v) for v in va[:2] + vb[:2]):
                        ea = ra.objects[k].err if k < len(ra.objects) else -1
                        eb = rb.objects[k].err if k < len(rb.objects) else -1
                        table.add(**base, verdict="unanswered", note=f"errCode ours {ea} theirs {eb}")
                        continue
                    rows.append((b, va, vb, base))
                if plane != "invariable":
                    for b, va, vb, base in rows:
                        s = sep_arcsec((va[0], va[1]), (vb[0], vb[1]))
                        trop = tropical.get((jd, b))
                        band = (trop if trop is not None else refit_band(b, va[2])) + SIDEREAL_EXTRA
                        worst = max(worst, s - (trop or 0.0))
                        table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]),
                                  sep_servers=s, band=band,
                                  verdict="agree" if s <= band else "finding",
                                  note=f"tropical gap here {trop:.4f}\"" if trop is not None
                                  else "no tropical answer here")
                    continue
                dlons = [((va[0] - vb[0] + 180.0) % 360.0 - 180.0) * 3600.0 for _, va, vb, _ in rows]
                dlats = [abs(va[1] - vb[1]) * 3600.0 for _, va, vb, _ in rows]
                spread = max(dlons) - min(dlons) if dlons else 0.0
                offsets += dlons
                for (b, va, vb, base), dl, dt in zip(rows, dlons, dlats):
                    same_plane = dt <= INVARIABLE_LAT_BAND and spread <= INVARIABLE_SPREAD_BAND
                    # 3.5a Part A, approved by both maintainers 2026-09-18: the
                    # origin is the zodiac's zero point projected onto the plane.
                    # astrolog-ephd implements it since their 4f9c2a1, so any
                    # offset beyond the frames' own disagreement is a finding.
                    verdict = "agree" if same_plane and abs(dl) <= INVARIABLE_LAT_BAND else "finding"
                    table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]),
                              sep_servers=sep_arcsec((va[0], va[1]), (vb[0], vb[1])),
                              band=f"lat {INVARIABLE_LAT_BAND} spread {INVARIABLE_SPREAD_BAND}",
                              verdict=verdict,
                              note=f"longitude offset {dl:+.3f}\" (spread {spread:.3f}\" across "
                                   f"bodies), latitude {dt:.3f}\"; 3.5a Part A (2026-09-18): "
                                   "the origin is the projected zero point (theirs since 4f9c2a1)")
            if plane == "invariable":
                print(f"  {zodiac:13s} {plane:10s} origin offset "
                      f"{min(offsets):+.3f}..{max(offsets):+.3f}\"")
            else:
                print(f"  {zodiac:13s} {plane:10s} worst beyond the tropical gap {worst:+.4f}\"")


# Zodiacs defined at the instant (docs/FRAMES.md): plane 0, both servers, for
# the tokens both list. Until their 789f3c2, astrolog-ephd took the anchor's
# apparent position, and its rows differed by the anchor's aberration (up to
# ~20", record l); both now take the true position, as published.
INSTANT_TOKENS = ["true-citra", "true-revati", "true-pushya", "true-mula", "galcent-0sag",
                  "galcent-cochrane", "galcent-rgilbrand", "galcent-mula-wilhelm",
                  "galequ-iau1958", "galequ-true", "galequ-mula"]
INSTANT_EPOCHS = [2415020.5, 2451545.0, 2488069.5]
# The anchors come from different data on the two sides: the stars from two
# catalogues (the stars leg holds those within STAR_BAND), Sgr A*'s place and
# motion from different sources. An estimate; the definitions agree once
# astrolog-ephd took the true anchor (their 789f3c2, 2026-09-18).
INSTANT_ANCHOR_BAND = 0.1


def leg_sidinstant(client, ours, theirs, table, verbose):
    print("\n== sidinstant: zodiacs defined at the instant, plane 0, apparent, true of date")
    wel_a = ask(client, ours, ["--obj", "10", "--corrections", "0"], verbose)
    wel_b = ask(client, theirs, ["--obj", "10", "--corrections", "0"], verbose)
    both = [t for t in INSTANT_TOKENS
            if t in wel_a.caps.get("zodiacs", []) and t in wel_b.caps.get("zodiacs", [])]
    if not both:
        print("  no token listed by both servers")
        return
    tropical = {}
    for jd in INSTANT_EPOCHS:
        args = ["--jd", repr(jd), "--corrections", "7", "--deltat", str(DELTA_T)]
        for b in SIDEREAL_BODIES:
            args += ["--obj", str(b)]
        ra, rb = ask(client, ours, args, verbose), ask(client, theirs, args, verbose)
        for k, b in enumerate(SIDEREAL_BODIES):
            va, vb = ra.row(k), rb.row(k)
            if va is not None and vb is not None and not math.isnan(va[0] + vb[0]):
                tropical[(jd, b)] = sep_arcsec((va[0], va[1]), (vb[0], vb[1]))
    for token in both:
        offsets = []
        for jd in INSTANT_EPOCHS:
            args = ["--jd", repr(jd), "--corrections", "7", "--deltat", str(DELTA_T), "--sid", token]
            for b in SIDEREAL_BODIES:
                args += ["--obj", str(b)]
            ra, rb = ask(client, ours, args, verbose), ask(client, theirs, args, verbose)
            table.asked(ra, rb)
            for k, b in enumerate(SIDEREAL_BODIES):
                va, vb = ra.row(k), rb.row(k)
                base = dict(leg="sidinstant", epoch_tt=jd, object=b, observer="geo",
                            frame=f"{token} date", plane="ecliptic", mask=7, deltat=DELTA_T,
                            tier=2)
                if va is None or vb is None or any(math.isnan(v) for v in va[:2] + vb[:2]):
                    table.add(**base, verdict="unanswered", note="no row from one side")
                    continue
                s = sep_arcsec((va[0], va[1]), (vb[0], vb[1]))
                trop = tropical.get((jd, b), 0.0)
                band = trop + SIDEREAL_EXTRA + INSTANT_ANCHOR_BAND
                dl = ((va[0] - vb[0] + 180.0) % 360.0 - 180.0) * 3600.0
                offsets.append(dl)
                table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]), sep_servers=s,
                          band=band, verdict="agree" if s <= band else "finding (theirs)",
                          note=f"longitude offset {dl:+.4f}\"; the anchor at its true position "
                               "(published definition, 3.5a); the band allows the anchors' "
                               "different catalogues")
        print(f"  {token:22s} longitude offset {min(offsets):+.4f}..{max(offsets):+.4f}\"")


SWEEP_BODIES = [10, 301, 4, 5]
SWEEP_EPOCHS = [2433463.5, 2478938.5]  # 1950-07-01, 2075-01-01: off every token's anchor
SWEEP_USER_ANCHOR = "2415020.5,22.46"  # any anchor will do: the leg asks whether a plane moves
SWEEP_PLANES = ("date", "anchor", "invariable")
# astrolog-ephd's star- and frame-anchored tokens, which answer plane 0 for
# planes 1 and 2: known, and their fix is decided and pending (their registry
# 4.1, 2026-09-18). Still findings; the note says they are not news.
SWEEP_KNOWN_IGNORED = {"b1950", "j1900", "j2000", "true-citra", "true-revati", "true-pushya",
                       "true-mula", "true-sheoran", "galcent-0sag", "galcent-cochrane",
                       "galcent-mula-wilhelm", "galcent-rgilbrand", "galequ-iau1958",
                       "galequ-mula", "galequ-true", "galalign-mardyks"}


def sweep_tokens():
    with open(os.path.join(REPO, "third_party", "ephproto", "v4", "registries.json")) as f:
        entries = json.load(f)["registries"]["zodiac_tokens"]["entries"]
    return [e["token"] for e in entries]


def leg_sidsweep(client, ours, theirs, table, verbose):
    """Every A.11 zodiac token on every A.8 plane, graded per server, on what
    a plane request does rather than on agreement:
    - a token the server's WELCOME does not list must draw ERROR 11 (3.5a);
    - a listed token's planes 1 and 2 must each MOVE the answer from plane 0,
      or be refused explicitly (ERROR 11, or errCode 2 on every object);
      A row bit-identical to plane 0 is a plane accepted and ignored, which no
      comparison with another server can see when that server was never asked
      (the Astrolog side's 16 star- and frame-anchored tokens, 2026-09-18).
    The epochs sit off every anchor epoch, where plane 1 could legitimately
    coincide with plane 0."""
    print("\n== sidsweep: every zodiac token on every sidereal plane, per server")
    tokens = sweep_tokens()
    for who, srv in (("ours", ours), ("theirs", theirs)):
        listed = set(ask(client, srv, ["--obj", "10", "--corrections", "0"], verbose).caps.get("zodiacs", []))
        tally = {}
        for token in tokens:
            zod = ["--sid", token] + (["--sidu", SWEEP_USER_ANCHOR] if token == "user" else [])
            for jd in SWEEP_EPOCHS:
                replies = {}
                for plane in SWEEP_PLANES:
                    args = ["--jd", repr(jd), "--corrections", "7", "--deltat", str(DELTA_T),
                            "--sid-plane", plane] + zod
                    for b in SWEEP_BODIES:
                        args += ["--obj", str(b)]
                    r = ask(client, srv, args, verbose)
                    table.asked(r if who == "ours" else None, r if who == "theirs" else None)
                    replies[plane] = r
                for plane, r in replies.items():
                    base = dict(leg="sidsweep", epoch_tt=jd, object=token, observer="geo",
                                frame=f"{token} {plane}", plane="ecliptic", mask=7,
                                deltat=DELTA_T, tier=2)
                    refused = "ERROR 11" in r.stderr
                    if token not in listed:
                        verdict = "agree" if refused else f"finding ({who})"
                        note = "unlisted in WELCOME; 3.5a requires ERROR 11"
                        what = "ERROR 11" if refused else "answered"
                    elif refused and plane == "date":
                        verdict, what = f"finding ({who})", "ERROR 11"
                        note = "token listed in WELCOME, so plane 0 must be served"
                    elif refused:
                        # An explicit refusal of one (token, fixed plane) pair is
                        # the protocol's answer for a zero point the server cannot
                        # construct; only a silent plane-0 answer is a finding.
                        verdict, what = "refused", "ERROR 11"
                        note = "this token on a fixed plane refused outright, not answered silently"
                    elif plane == "date":
                        verdict, what, note = "agree", "answered", "plane 0, the reference"
                    else:
                        p0 = replies["date"]
                        moved, same = [], 0
                        for k in range(len(SWEEP_BODIES)):
                            v, v0 = r.row(k), p0.row(k)
                            # A refused object's rows are NaN: no answer, and
                            # never counted as movement (a NaN separation is
                            # neither equal nor small).
                            if v is None or v0 is None or any(math.isnan(x) for x in v[:2] + v0[:2]):
                                continue
                            if v[0] == v0[0] and v[1] == v0[1]:
                                same += 1
                            moved.append(sep_arcsec((v[0], v[1]), (v0[0], v0[1])))
                        errs = {o.err for o in r.objects}
                        if not moved and errs == {2}:
                            verdict, what = "refused", "errCode 2"
                            note = "every object unsupported on this plane: refused, not answered silently"
                        elif not moved:
                            verdict, what, note = f"finding ({who})", "no rows", "answered without rows"
                        elif same == len(moved):
                            verdict, what = f"finding ({who})", "identical to plane 0"
                            note = "bit-identical to plane 0 for every body: the plane was accepted and ignored"
                            if who == "theirs" and token in SWEEP_KNOWN_IGNORED:
                                note += "; known, their registry 4.1, fix pending"
                        else:
                            verdict, what = "agree", f"moved {min(moved):.3f}..{max(moved):.3f}\""
                            note = "moves from plane 0"
                    tally[verdict] = tally.get(verdict, 0) + 1
                    table.add(**base, **{who: what}, verdict=verdict, note=note)
                    if verdict != "agree" and jd == SWEEP_EPOCHS[0]:
                        print(f"  {who:6s} {token:22s} {plane:10s} {what}")
        print(f"  {who}: {len(listed & set(tokens))} of {len(tokens)} tokens listed; "
              + ", ".join(f"{k} {v}" for k, v in sorted(tally.items())))


def delta_t_from_sidereal_time(ut1_tool, lines):
    """TT - UT1 (s) per (jd_tt, tdb_minus_ut, last_hours, lon_deg), solved by
    prometheia-ut1 so Horizons' own Earth rotation is what both servers get."""
    text = "".join(f"{a!r} {b!r} {c!r} {d!r}\n" for a, b, c, d in lines)
    done = subprocess.run([ut1_tool], input=text, capture_output=True, text=True, check=True)
    out = [float(v) for v in done.stdout.split()]
    if len(out) != len(lines):
        sys.exit(f"prometheia-ut1 answered {len(out)} of {len(lines)} rows")
    return out


# Swiss builds a topocentric site about the MEAN pole and uses it against a
# true-of-date geocentric vector (confirmed in its source by the Astrolog
# side; measured here as the nutation pole offset to 1-3 m). The site moves
# at most ~9.3" of arc on the Earth's surface, under 300 m, which is 0.2" on
# the Moon at perigee and far less on anything else.
MEAN_POLE_BAND = {"moon": 0.2, "planets": 0.001}
MEAN_POLE_NOTE = ("Swiss's topocentric site about the mean pole (no nutation), kept upstream; "
                  "geocentric agrees at this instant")


def mean_pole_site(client, ours, theirs, jd, body, st, verbose):
    """Whether a topocentric gap of astrolog-ephd's can be the mean-pole site
    and nothing else: the two servers' GEOCENTRIC answers at the same instant
    agree within the same-question band, and the gap is no larger than the
    nutation offset can make it. Anything else stays a finding."""
    cls = "moon" if body == 301 else "planets"
    args = ["--jd", repr(jd), "--corrections", "1", "--icrs", "--eq", "--obj", str(body),
            "--deltat", str(DELTA_T)]
    va, vb = ask(client, ours, args, verbose).row(0), ask(client, theirs, args, verbose).row(0)
    if va is None or vb is None:
        return False
    if st > MEAN_POLE_BAND[cls] + refit_band(body, va[2], anchor=True):
        return False
    return sep_arcsec((va[0], va[1]), (vb[0], vb[1])) <= refit_band(body, va[2])


def leg_topo(client, ut1_tool, ours, theirs, wel_a, wel_b, table, verbose):
    """Each server against Horizons from a site on the Earth: mask 1, ICRF,
    equatorial, with the Delta T that reproduces Horizons' sidereal time.
    Then apparent place of date at the same rows, server against server, so
    a topocentric apparent gap can be read with the anchor's Earth rotation."""
    corpus_ = topo_corpus()
    rows = [(name, body, site, p) for name, body, site, pts in corpus_ for p in pts]
    dts = delta_t_from_sidereal_time(ut1_tool, [(p[0], p[3], p[4], site[0])
                                                for _, _, site, p in rows])
    print(f"\n== horizons-topo: mask 1, ICRF, equatorial, {len(corpus_)} site/body series, "
          f"{len(rows)} rows; Delta T from Horizons' LAST {min(dts):.3f}..{max(dts):.3f} s")
    worst = {}
    for (name, body, site, p), dt in zip(rows, dts):
        jd, anc = p[0], (p[1], p[2])
        where = ["--topo", f"{site[0]},{site[1]},{site[2] * 1000.0}", "--deltat", repr(dt)]
        cls = "moon" if body == 301 else "planets"
        obs = "topo " + name.split("-")[1]
        for mask, leg in ((1, "horizons-topo"), (common_mask(wel_a, wel_b, 1), "apparent-topo")):
            args = ["--jd", repr(jd), "--corrections", str(mask), "--obj", str(body)] + where
            if mask == 1:
                args += ["--icrs", "--eq"]
            ra = ask(client, ours, args, verbose)
            rb = ask(client, theirs, args, verbose)
            table.asked(ra, rb)
            va, vb = ra.row(0), rb.row(0)
            base = dict(leg=leg, epoch_tt=jd, object=body, observer=obs,
                        frame="ICRF" if mask == 1 else "true of date",
                        plane="equator" if mask == 1 else "ecliptic", mask=mask, deltat=dt)
            if va is None or vb is None or any(math.isnan(x) for x in va[:2] + vb[:2]):
                ea = ra.objects[0].err if ra.objects else -1
                eb = rb.objects[0].err if rb.objects else -1
                table.add(**base, verdict="unanswered", note=f"errCode ours {ea} theirs {eb}")
                continue
            pa, pb = (va[0], va[1]), (vb[0], vb[1])
            s = sep_arcsec(pa, pb)
            band = refit_band(body, va[2])
            if leg == "apparent-topo":
                verdict = "agree" if s <= band else "finding"
                note = "Earth rotation from Horizons' LAST; see this row's horizons-topo"
                if verdict == "finding" and mean_pole_site(client, ours, theirs, jd, body, s,
                                                           verbose):
                    verdict, note = "expected-difference", note + "; " + MEAN_POLE_NOTE
                table.add(**base, ours=pa, theirs=pb, sep_servers=s, band=band, tier=2,
                          verdict=verdict, note=note)
                worst[("apparent", body)] = max(worst.get(("apparent", body), 0.0), s)
                continue
            so, st = sep_arcsec(pa, anc), sep_arcsec(pb, anc)
            band_t = refit_band(body, va[2], anchor=True)
            ok_o, ok_t = so <= OURS_HORIZONS[cls], st <= band_t
            note = ""
            if ok_o and ok_t:
                verdict = "agree"
            elif s <= band:
                verdict = "finding"
            else:
                verdict = "finding (ours)" if not ok_o else "finding (theirs)"
            if verdict == "finding (theirs)" and mean_pole_site(client, ours, theirs, jd, body,
                                                                 st, verbose):
                verdict = "expected-difference"
                note = MEAN_POLE_NOTE
            table.add(**base, ours=pa, theirs=pb, anchor=anc, anchor_source=f"Horizons {name} q1",
                      sep_servers=s, sep_ours_anchor=so, sep_theirs_anchor=st,
                      band=f"ours {OURS_HORIZONS[cls]} theirs {band_t:.6f}", tier=2,
                      verdict=verdict, note=note)
            worst[("ours", body)] = max(worst.get(("ours", body), 0.0), so)
            worst[("theirs", body)] = max(worst.get(("theirs", body), 0.0), st)
    for (who, body), w in sorted(worst.items()):
        label = {"ours": "ours vs Horizons", "theirs": "theirs vs Horizons",
                 "apparent": "apparent, ours vs theirs"}[who]
        print(f"  body {body:4d}  worst {label}: {w:.6f}\"")


def adjudicate_helio_light_time(table):
    """A heliocentric astrometric row where only astrolog-ephd leaves the
    Horizons band, while the geometric (mask 0) answers agree within the
    same-question band, is Swiss's heliocentric light time: kept by the
    Astrolog side on purpose, so the difference is expected, not a defect.
    A geometric disagreement at that row keeps it a finding."""
    geometric = {(r["epoch_tt"], r["object"]): r for r in table.rows if r["leg"] == "same-helio"}
    for r in table.rows:
        astrometric_apparent = (r["leg"] == "apparent" and r["observer"] == "helio"
                                and r["mask"] == 1 and r["verdict"] == "finding")
        if not astrometric_apparent and (r["leg"] != "horizons-helio"
                                         or r["verdict"] != "finding (theirs)"):
            continue
        g = geometric.get((r["epoch_tt"], r["object"]))
        if g is not None and g["verdict"] == "agree":
            r["verdict"] = "expected-difference"
            r["note"] = (r.get("note", "") + "; geometric agrees, light time differs: Swiss's "
                         "heliocentric light time, kept by design (docs/CROSS-TEST.md)").lstrip("; ")


def adjudicate_coverage(table):
    """A row one server answers and the other refuses with errCode 3 is a
    difference of coverage (A.17: outside the data's span), a legitimate
    answer, not a missing one. Any other refusal stays unanswered."""
    for r in table.rows:
        if r["verdict"] != "unanswered" or not r["note"].startswith("errCode"):
            continue
        codes = r["note"].split()
        ours, theirs = int(codes[2]), int(codes[4])
        if sorted((ours, theirs)) == [0, 3]:
            r["verdict"] = "expected-difference"
            r["note"] += "; outside one server's coverage (errCode 3)"


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
    deflected = {(r["epoch_tt"], r["object"]): r for r in table.rows if r["leg"] == "deflection"}
    geo_apparent = {(r["epoch_tt"], r["object"]): r for r in table.rows
                    if r["leg"] == "apparent" and r["observer"] == "geo"}
    anchored_bary = {r["epoch_tt"]: r for r in table.rows if r["leg"] == "horizons-bary"}
    anchored_helio = {(r["epoch_tt"], r["object"]): r for r in table.rows
                      if r["leg"] == "horizons-helio"}
    for r in table.rows:
        if r["leg"] not in ("same", "same-helio", "apparent") or r["verdict"] != "finding":
            continue
        # An anchor adjudicates only a row from the same observer: the
        # geocentric Moon agreeing says nothing about a topocentric or a
        # barycentric gap, and borrowing it would wave real findings through.
        if r["leg"] == "apparent" and r["observer"] == "jupiter":
            d = deflected.get((r["epoch_tt"], r["object"]))
            if d is None or d["verdict"] == "unanswered":
                r["verdict"] = "unadjudicated"
                r["note"] += "; no deflection anchor at this row"
            else:
                r["verdict"] = {"agree": "unadjudicated"}.get(d["verdict"], d["verdict"])
                r["note"] += f"; deflection against the textbook: {d['verdict']}"
            continue
        if r["leg"] == "apparent" and r["observer"] == "bary" and r["object"] == 10:
            h = anchored_bary.get(r["epoch_tt"])
            if h is not None and h["verdict"] == "agree":
                r["verdict"] = "expected-difference"
                r["note"] += ("; the Sun's barycentric position, each server inside its km "
                              "band at the anchor (horizons-bary): the .se1 refit, seen close up")
                continue
        if r["leg"] == "apparent" and r["observer"] == "topo":
            g = geo_apparent.get((r["epoch_tt"], r["object"]))
            cls = "moon" if r["object"] == 301 else "planets"
            if (g is not None and g["verdict"] in ("agree", "expected-difference")
                    and r["sep_servers"] <= MEAN_POLE_BAND[cls]):
                r["verdict"] = "expected-difference"
                r["note"] += "; " + MEAN_POLE_NOTE
                continue
        if r["leg"] == "apparent" and r["observer"] != "geo":
            r["note"] += "; no anchor from this observer"
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


def git_head(path, ignore=None):
    try:
        head = subprocess.run(["git", "-C", path, "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True, timeout=10).stdout.strip() or "?"
        # The table being written does not make the tree dirty.
        spec = ["--", ".", f":!{os.path.relpath(os.path.abspath(ignore), path)}"] if ignore else []
        dirty = subprocess.run(["git", "-C", path, "status", "--porcelain",
                                "--untracked-files=no"] + spec,
                               capture_output=True, text=True, timeout=10).stdout.strip()
        return head + ("+dirty" if dirty else "")
    except Exception:  # noqa: BLE001
        return "?"


def pids_listening(port):
    """The pids holding a listening TCP socket on this port, from /proc alone
    (no ss, no lsof). Empty when none is found or /proc cannot be read."""
    inodes = set()
    for name in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            with open(name) as f:
                next(f, None)
                for line in f:
                    p = line.split()
                    if len(p) < 10 or p[3] != "0A":  # 0A = TCP_LISTEN
                        continue
                    if int(p[1].rsplit(":", 1)[1], 16) == port:
                        inodes.add(p[9])
        except OSError:
            continue
    pids = set()
    if not inodes:
        return pids
    for pid in os.listdir("/proc") if os.path.isdir("/proc") else []:
        if not pid.isdigit():
            continue
        try:
            for fd in os.listdir(f"/proc/{pid}/fd"):
                try:
                    link = os.readlink(f"/proc/{pid}/fd/{fd}")
                except OSError:
                    continue
                if link.startswith("socket:[") and link[8:-1] in inodes:
                    pids.add(int(pid))
                    break
        except OSError:
            continue
    return pids


def binary_time(path, port=None):
    """The daemon file's build time, marked STALE when the daemon that
    ANSWERED still executes a file since replaced (a rebuild under a live
    daemon), since then the time describes a binary that did not answer.

    Only the process listening on `port` counts. Matching any process of the
    same name instead reported a stale binary on 2026-09-20 because a second,
    older `astrolog-ephd` was up on another port; the daemon under test was
    current, and the record carried a warning that was true of nothing it
    measured. A check that cannot say which process it means is not a check.
    """
    try:
        t = os.path.getmtime(path)
    except OSError:
        return "unknown"
    out = datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    if port is None:
        return out
    pids = pids_listening(port)
    if not pids:
        return out + f" (no listener on port {port}: staleness unchecked)"
    for pid in sorted(pids):
        try:
            exe = os.readlink(f"/proc/{pid}/exe")
        except OSError:
            return out + f" (pid {pid} unreadable: staleness unchecked)"
        if exe.endswith(" (deleted)"):
            return out + (f" STALE: the daemon on port {port} (pid {pid}) runs a "
                          "binary since replaced, so this time is not its build")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ours", default="127.0.0.1:47190")
    ap.add_argument("--theirs", default="127.0.0.1:47391")
    ap.add_argument("--client", default=os.path.join(REPO, "build", "prometheia-wire-client"))
    ap.add_argument("--ut1", default=os.path.join(REPO, "build", "prometheia-ut1"))
    ap.add_argument("--legs",
                    default="surfaces,same,horizons,hamburg,helio,apparent,topo,bary,"
                            "deflection,deflection-geo,deflection-topo,arrival,points,rates,"
                            "sidereal,sidsweep,sidinstant,stars")
    ap.add_argument("--out", help="write the leg table (TSV) here")
    ap.add_argument("--astrolog", default="/nvmraid/shares/Astrolog", help="the Astrolog tree, for its commit")
    ap.add_argument("--astrolog-bin", default="/nvmraid/shares/Astrolog/astrolog-ephd",
                    help="the daemon that ran, for its build time (a commit can postdate it)")
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
    if a.server == b.server and a.dataset == b.dataset:
        # Two harnesses on one machine have met: a port taken by someone
        # else's daemon compares a server with itself, and every verdict
        # after that is meaningless.
        sys.exit(f"both endpoints answered as {a.server} with the same dataset: "
                 "one of them is not the server it should be")
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
        leg_apparent(client, ours, theirs, a, b, table, args.verbose)
    if "points" in legs:
        leg_points(client, ours, theirs, table, args.verbose)
    if "stars" in legs:
        leg_stars(client, ours, theirs, table, args.verbose)
    if "rates" in legs:
        leg_rates(client, ours, theirs, table, args.verbose)
    if "sidereal" in legs:
        leg_sidereal(client, ours, theirs, table, args.verbose)
    if "sidsweep" in legs:
        leg_sidsweep(client, ours, theirs, table, args.verbose)
    if "sidinstant" in legs:
        leg_sidinstant(client, ours, theirs, table, args.verbose)
    if "deflection" in legs:
        leg_deflection(client, ours, theirs, a, b, table, args.verbose)
    if "deflection-geo" in legs:
        leg_deflection_geo(client, ours, theirs, a, b, table, args.verbose)
    if "deflection-topo" in legs:
        leg_deflection_topo(client, ours, theirs, a, b, table, args.verbose)
    if "arrival" in legs:
        leg_arrival(client, ours, theirs, a, b, table, args.verbose)
    if "bary" in legs:
        leg_bary(client, ours, theirs, table, args.verbose)
    if "topo" in legs:
        leg_topo(client, args.ut1, ours, theirs, a, b, table, args.verbose)
    # Always, whatever ran. Both adjudicators already leave a row
    # "unadjudicated" when the anchor it needs is missing, but they used to
    # be called only when the anchor's leg was in --legs, so `--legs same`
    # alone reported four findings the harness had never asked an anchor
    # about. A verdict a run cannot support is the mis-attribution this
    # harness exists to avoid.
    adjudicate_helio_light_time(table)
    adjudicate_same(table)
    adjudicate_coverage(table)  # last: an anchor refused for coverage still adjudicates nothing

    counts = {}
    for r in table.rows:
        if r["leg"] != "surfaces":
            counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    print("\nverdicts: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    if args.out:
        header = [
            f"crosstest {datetime.datetime.now(datetime.timezone.utc):%Y-%m-%dT%H:%M:%SZ}",
            f"ours   {a.server} dataset {a.dataset} prometheia {git_head(REPO, args.out)}",
            f"theirs {b.server} dataset {b.dataset} astrolog {git_head(args.astrolog)}"
            f" binary built {binary_time(args.astrolog_bin, theirs[1])}",
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

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
  points     orbit points (Moon and planets, mean and osculating) by
             direction and distance, mask 0, and the node-on-its-frame's-
             ecliptic rule checked where it is exact
  sidereal   the three A.8 sidereal planes for two zodiacs
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
               [--legs surfaces,same,...] [--out table.tsv]
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
    raw = os.path.join(REPO, "horizons-raw")
    out = []
    for name, _, params in hf.requests():
        if not name.startswith("topo-"):
            continue
        site = tuple(float(x) for x in params["SITE_COORD"].strip("'").split(","))
        with open(os.path.join(raw, name + ".json")) as f:
            result = json.load(f)["result"]
        cols, rows = gen.table(result)
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
    with open(os.path.join(REPO, "horizons-raw", "bary-sun.json")) as f:
        result = json.load(f)["result"]
    cols, rows = gen.table(result)
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


# Orbit points (kind 1), geometric (mask 0) in the frame of date: the portable
# comparison, where light-time conventions for a point stay out. Bands from
# the first measurement (2026-09-18, the corpus epochs), each with its reason.
POINT_SPECS = ["301.a.m", "301.d.m", "301.p.m", "301.A.m", "301.a.o", "301.d.o", "301.p.o",
               "301.A.o", "199.a.m", "199.p.m", "199.a.o", "199.p.o", "4.a.m", "4.p.m",
               "4.a.o", "4.p.o", "5.a.m", "5.p.m", "5.a.o", "5.p.o", "6.a.m", "6.p.m", "6.a.o",
               "6.p.o"]
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


def point_class(spec):
    naif, _, method = spec.split(".")
    if naif == "301":
        return "moon"
    if method == "o":
        return "osculating"
    # Mean elements are model-defined; the giant planets' are ours fitted to
    # DE440 and differ from Swiss's by up to 3300" (Saturn's perihelion).
    return "mean" if naif in ("199", "299", "4") else "model"


def leg_points(client, ours, theirs, table, verbose):
    """Orbit points by direction AND distance, mask 0, true ecliptic of date;
    then the rule that a node lies on the ecliptic of the profile's frame
    (3.5a), checked where it is exact: the Moon's geocentric node has zero
    latitude on its ecliptic, which for frames 2 and 3 is the J2000 one."""
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
            if cls == "model":
                table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]), sep_servers=s,
                          tier=2, verdict="unadjudicated",
                          note=note + "; mean elements are model-defined (ours fitted to DE440)")
                continue
            band_s, band_km = POINT_BANDS[cls]
            verdict = "agree" if s <= band_s and dkm <= band_km else "finding"
            table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]), sep_servers=s,
                      band=f"{band_s}\" {band_km} km", tier=2, verdict=verdict, note=note)
    for spec, (s, d) in worst.items():
        print(f"  {spec:8s} worst {s:9.3f}\"  {d:11.1f} km  ({point_class(spec)})")

    print("  node on the frame's ecliptic (Moon's mean node, J2000 frame: latitude must be 0)")
    for jd in epochs:
        args = ["--jd", repr(jd), "--corrections", "0", "--deltat", str(DELTA_T), "--j2000",
                "--node", "301.a.m"]
        ra, rb = ask(client, ours, args, verbose), ask(client, theirs, args, verbose)
        table.asked(ra, rb)
        va, vb = ra.row(0), rb.row(0)
        base = dict(leg="points-frame", epoch_tt=jd, object="301.a.m", observer="geo",
                    frame="J2000", plane="ecliptic", mask=0, deltat=DELTA_T, tier=1)
        if va is None or vb is None:
            table.add(**base, verdict="unanswered")
            continue
        lo, lt = abs(va[1]) * 3600.0, abs(vb[1]) * 3600.0
        band = 0.001  # rounding: the node is on its plane by construction
        verdict = ("agree" if lo <= band and lt <= band else
                   "finding (ours)" if lo > band else "finding (theirs)")
        table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]), band=band,
                  verdict=verdict,
                  note=f"latitude on the J2000 ecliptic: ours {lo:.4f}\" theirs {lt:.4f}\"; "
                       "3.5a puts a node on the frame's ecliptic, the J2000 one for frames 2-3")


# Sidereal planes (A.8). Planes 0 and 1 are the same question on both sides
# and are judged by the refit band. Plane 2's longitude origin is open (the
# protocol's "carried onto"): a row there is unadjudicated when the planes
# themselves agree (latitude) and the longitudes differ by one constant
# across bodies, which is a difference of origin and nothing else.
SIDEREAL_BODIES = [10, 301, 4, 5, 6]
# What a sidereal rotation may add to the tropical gap: the two ayanamsa
# series differ by their precession models, 0.0026" over 1800-2200
# (docs/FRAMES.md, measured against swetest -ay).
SIDEREAL_EXTRA = 0.003
INVARIABLE_LAT_BAND = 0.05  # arcsec: the two sides' plane orientations
INVARIABLE_SPREAD_BAND = 0.02  # arcsec: a constant origin offset, across bodies


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
                    table.add(**base, ours=(va[0], va[1]), theirs=(vb[0], vb[1]),
                              sep_servers=sep_arcsec((va[0], va[1]), (vb[0], vb[1])),
                              band=f"lat {INVARIABLE_LAT_BAND} spread {INVARIABLE_SPREAD_BAND}",
                              verdict="unadjudicated" if same_plane else "finding",
                              note=f"longitude offset {dl:+.3f}\" (spread {spread:.3f}\" across "
                                   f"bodies), latitude {dt:.3f}\"; the plane agrees and the "
                                   "origin is open: the protocol's 'carried onto'")
            if plane == "invariable":
                print(f"  {zodiac:13s} {plane:10s} origin offset "
                      f"{min(offsets):+.3f}..{max(offsets):+.3f}\"")
            else:
                print(f"  {zodiac:13s} {plane:10s} worst beyond the tropical gap {worst:+.4f}\"")


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


def binary_time(path):
    """The daemon file's build time, marked STALE when a running process of
    that name still executes a file since replaced (a rebuild under a live
    daemon), since then the time describes a binary that did not answer."""
    try:
        t = os.path.getmtime(path)
    except OSError:
        return "unknown"
    out = datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    name = os.path.basename(path)
    for pid in os.listdir("/proc") if os.path.isdir("/proc") else []:
        try:
            exe = os.readlink(f"/proc/{pid}/exe")
        except OSError:
            continue
        if os.path.basename(exe).startswith(name) and exe.endswith(" (deleted)"):
            return out + " STALE: a running daemon predates this file"
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ours", default="127.0.0.1:47190")
    ap.add_argument("--theirs", default="127.0.0.1:47391")
    ap.add_argument("--client", default=os.path.join(REPO, "build", "prometheia-wire-client"))
    ap.add_argument("--ut1", default=os.path.join(REPO, "build", "prometheia-ut1"))
    ap.add_argument("--legs", default="surfaces,same,horizons,hamburg,helio,apparent,topo,bary,deflection,points,sidereal")
    ap.add_argument("--out", help="write the leg table (TSV) here")
    ap.add_argument("--astrolog", default="/nvm/work/ephv4", help="the Astrolog tree, for its commit")
    ap.add_argument("--astrolog-bin", default="/nvm/work/ephv4/astrolog-ephd",
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
    if "sidereal" in legs:
        leg_sidereal(client, ours, theirs, table, args.verbose)
    if "deflection" in legs:
        leg_deflection(client, ours, theirs, a, b, table, args.verbose)
    if "bary" in legs:
        leg_bary(client, ours, theirs, table, args.verbose)
    if "topo" in legs:
        leg_topo(client, args.ut1, ours, theirs, a, b, table, args.verbose)
    if "helio" in legs:
        adjudicate_helio_light_time(table)
    if "horizons" in legs or "helio" in legs:
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
            f" binary built {binary_time(args.astrolog_bin)}",
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

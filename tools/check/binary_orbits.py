# SPDX-License-Identifier: GPL-2.0-or-later
"""The binary stars whose orbits the engine applies (docs/STARS.md, "Binary
stars"), computed independently of the engine for its checks:
tools/check/stars_fk5.py and tools/gen/gen_star_fixtures.py.

The orbital elements are read from the Sixth Catalog of Orbits of Visual
Binary Stars as fetched (stars-raw/orb6orbits.txt, tools/fetch/stars_fetch.py),
not copied from the engine's table, so a transcription error on either side
shows. The masses are the orbits' own papers' (the same tool's arxiv-*
sources).
"""
import math
import os

# HIP -> (ORB6 WDS id, ORB6 discoverer designation, share of the relative
# orbit, what the catalog's straight line is). The share is -M_B/M for a
# primary and +M_A/M for the secondary; the line is from the Hipparcos
# solution type (I/311): "barycentre" where the published catalog fitted the
# orbit; "component" for an ordinary 5-parameter solution of one star;
# "secondary" for a star placed from its primary's model plus the relative
# orbit (the primary's HIP last), its own line giving only its distance.
BINARIES = {
    32349: ("06451-1643", "AGC   1AB", -1.018 / (2.063 + 1.018), "barycentre"),  # Sirius A
    37279: ("07393+0514", "SHB   1AB", -0.592 / (1.478 + 0.592), "barycentre"),  # Procyon A
    71683: ("14396-6050", "RHD   1AB", -0.97 / (1.13 + 0.97), "component"),  # alpha Cen A
    71681: ("14396-6050", "RHD   1AB", 1.13 / (1.13 + 0.97), "secondary", 71683),  # alpha Cen B
}


def _field(line, start, width):
    return line[start - 1:start - 1 + width].strip()


def load(raw_dir):
    """HIP -> orbit dict, from ORB6's fixed columns (orb6format.txt)."""
    by_key = {}
    with open(os.path.join(raw_dir, "orb6orbits.txt"), encoding="latin-1") as f:
        for line in f:
            if len(line) < 230 or not line[19:29].strip():
                continue
            by_key[(line[19:29], line[30:44].rstrip())] = line
    out = {}
    for hip, (wds, disc, share, kind, *partner) in BINARIES.items():
        line = by_key[(wds, disc.rstrip())]
        assert _field(line, 93, 1) == "y" and _field(line, 115, 1) == "a"
        assert _field(line, 175, 1) == "y"
        out[hip] = dict(
            share=share, line=kind, partner=partner[0] if partner else 0,
            P=float(_field(line, 82, 11)), a=float(_field(line, 106, 9)),
            i=float(_field(line, 126, 8)), node=float(_field(line, 144, 8)),
            T=float(_field(line, 163, 12)), e=float(_field(line, 188, 8)),
            omega=float(_field(line, 206, 8)))
    return out


def offset(o, jd):
    """The star's offset from its system's barycentre at jd: arcsec east
    (RA x cos Dec) and north."""
    r = math.radians
    tp = 2415020.31352 + (o["T"] - 1900.0) * 365.242198781  # Besselian year
    m = 2.0 * math.pi * (jd - tp) / (o["P"] * 365.25)
    ecc = m
    for _ in range(60):
        ecc -= (ecc - o["e"] * math.sin(ecc) - m) / (1.0 - o["e"] * math.cos(ecc))
    x = math.cos(ecc) - o["e"]
    y = math.sqrt(1.0 - o["e"] ** 2) * math.sin(ecc)
    cw, sw = math.cos(r(o["omega"])), math.sin(r(o["omega"]))
    cn, sn = math.cos(r(o["node"])), math.sin(r(o["node"]))
    ci = math.cos(r(o["i"]))
    a = o["a"]
    A, B = a * (cw * cn - sw * sn * ci), a * (cw * sn + sw * cn * ci)
    F, G = a * (-sw * cn - cw * sn * ci), a * (-sw * sn + cw * cn * ci)
    return o["share"] * (B * x + G * y), o["share"] * (A * x + F * y)


def line_delta(own, partner, jd):
    """The partner's straight line less this star's at jd, arcsec east and north
    in this star's tangent plane. `own` and `partner` are catalog records
    (ra, dec, pmra, pmdec in mas/yr, epoch in Julian years); over a few decades
    the linear tangent-plane motion is exact to microarcseconds here."""
    def at(rec):
        dt = (jd - (2451545.0 + (rec["epoch"] - 2000.0) * 365.25)) / 365.25
        return rec["ra"], rec["dec"], rec["pmra"] * dt / 1000.0, rec["pmdec"] * dt / 1000.0
    ra0, de0, e0, n0 = at(own)
    ra1, de1, e1, n1 = at(partner)
    de_e = (ra1 - ra0) * 3600.0 * math.cos(math.radians(de0)) + e1 - e0
    de_n = (de1 - de0) * 3600.0 + n1 - n0
    return de_e, de_n


def bend(o, jd, epoch_jd, delta=None, primary=None, primary_epoch_jd=None):
    """What the orbit adds to the catalog's straight line (arcsec east, north):
    all of the offset for a barycentre line; for a component's, the offset less
    its value and rate at the catalog epoch, which the line already holds; for
    a secondary, (the primary's line + its bend + the relative orbit) - this
    star's line, with `delta` = the primary's line - this one's (line_delta)."""
    e1, n1 = offset(o, jd)
    if o["line"] == "barycentre":
        return e1, n1
    if o["line"] == "secondary":
        pe, pn = bend(primary, jd, primary_epoch_jd)
        qe, qn = offset(primary, jd)
        return delta[0] + pe + e1 - qe, delta[1] + pn + n1 - qn
    e0, n0 = offset(o, epoch_jd)
    ep, np_ = offset(o, epoch_jd + 1.0)
    em, nm = offset(o, epoch_jd - 1.0)
    dt = jd - epoch_jd
    return e1 - e0 - (ep - em) / 2.0 * dt, n1 - n0 - (np_ - nm) / 2.0 * dt

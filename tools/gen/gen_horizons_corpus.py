#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate tests/horizons_corpus.inc from cached JPL Horizons responses.

Input: the raw cache written by tools/fetch/horizons_fetch.py (gitignored,
default horizons-raw/). Output: C++ fixture tables for tests/test_horizons.cpp,
with provenance (API version, Horizons' planetary/small-body sources, EOP file,
retrieval time and SHA-256 per request) in the header.

Horizons output is a US-government work (JPL/Caltech for NASA).

Usage: gen_horizons_corpus.py [--raw-dir horizons-raw] [--check]
"""
import argparse
import json
import math
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(REPO, "tests", "horizons_corpus.inc")
sys.path.insert(0, os.path.join(REPO, "tools", "fetch"))
import horizons_fetch as hf  # noqa: E402  (the request declarations)

CENTER_GEO, CENTER_HELIO, CENTER_TOPO = 0, 1, 2
SMALL_BODY_SPKID_BASE = 20000000


def table(result):
    """(header columns, rows of stripped fields) of a CSV Horizons table."""
    lines = result.splitlines()
    soe = lines.index("$$SOE")
    eoe = lines.index("$$EOE")
    header = None
    for k in range(soe - 1, -1, -1):
        if "," in lines[k] and not lines[k].startswith("*"):
            header = [c.strip() for c in lines[k].split(",")]
            break
    rows = [[c.strip() for c in l.split(",")] for l in lines[soe + 1:eoe]]
    return header, rows


def header_value(result, key):
    m = re.search(r"^" + re.escape(key) + r"\s*:\s*(.*)$", result, re.M)
    return m.group(1).strip() if m else ""


def num(s):
    s = s.strip()
    if s in ("", "n.a."):
        return None
    return float(s)


def fmt(v):
    if v is None:
        return "kNa"
    r = repr(float(v))
    return r if ("." in r or "e" in r or "inf" in r or "nan" in r) else r + ".0"


def body_of(name, command):
    if command.endswith(";"):
        return SMALL_BODY_SPKID_BASE + int(command[:-1])
    return int(command)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--raw-dir", default="horizons-raw")
    ap.add_argument("--check", action="store_true", help="exit 1 if the .inc is stale")
    args = ap.parse_args()

    with open(os.path.join(args.raw_dir, "manifest.json")) as f:
        manifest = json.load(f)

    obs_rows, vec_rows, prov = [], [], []
    for name, purpose, params in hf.requests():
        path = os.path.join(args.raw_dir, name + ".json")
        if name not in manifest or not os.path.exists(path):
            sys.exit(f"{name}: not in the cache (run tools/fetch/horizons_fetch.py)")
        with open(path) as f:
            doc = json.load(f)
        result = doc["result"]
        entry = manifest[name]
        target = header_value(result, "Target body name")
        center = header_value(result, "Center body name")
        eop = header_value(result, "EOP file")
        perts = header_value(result, "Small-body perts") or header_value(result, "Small perturbers")
        target = " ".join(target.split())
        center = " ".join(center.split())
        perts = " ".join(perts.split())
        prov.append(f"//   {name}: {target} | from {center}"
                    + (f" | perturbers {perts}" if perts else "")
                    + (f" | EOP {eop}" if eop and params["EPHEM_TYPE"] == "'OBSERVER'" else ""))
        prov.append(f"//     retrieved {entry['retrieved_utc']} sha256 {entry['sha256']}")

        command = params["COMMAND"].strip("'")
        body = body_of(name, command)
        cols, rows = table(result)
        idx = {c: i for i, c in enumerate(cols)}

        if params["EPHEM_TYPE"] == "'VECTORS'":
            for r in rows:
                vals = [num(r[idx[c]]) for c in ("JDTDB", "X", "Y", "Z", "VX", "VY", "VZ")]
                vec_rows.append(f'    {{"{name}", {body}, ' + ", ".join(fmt(v) for v in vals)
                                + "},")
            continue

        ctr = params["CENTER"].strip("'")
        if ctr == "500@399":
            kind, site = CENTER_GEO, (0.0, 0.0, 0.0)
        elif ctr == "500@10":
            kind, site = CENTER_HELIO, (0.0, 0.0, 0.0)
        else:
            kind = CENTER_TOPO
            site = tuple(float(x) for x in params["SITE_COORD"].strip("'").split(","))

        def col(r, c):
            return num(r[idx[c]]) if c in idx else None

        for r in rows:
            vals = [
                col(r, "Date_________JDTT"),
                col(r, "R.A.___(ICRF)"), col(r, "DEC____(ICRF)"),
                col(r, "R.A.__(a-app)"), col(r, "DEC___(a-app)"),
                col(r, "delta"),
                col(r, "ObsEcLon"), col(r, "ObsEcLat"),
                col(r, "TDB-UT"),
                col(r, "L_Ap_Sid_Time"),
                col(r, "RA_3sigma"), col(r, "DEC_3sigma"), col(r, "POS_3sigma"),
            ]
            obs_rows.append(
                f'    {{"{name}", {body}, {kind}, {fmt(site[0])}, {fmt(site[1])}, '
                f'{fmt(site[2])}, ' + ", ".join(fmt(v) for v in vals) + "},")

    first = manifest[min(manifest)]
    lines = [
        "// Generated by tools/gen/gen_horizons_corpus.py -- do not edit.",
        "// JPL Horizons verification corpus (M5): responses of the requests declared",
        "// in tools/fetch/horizons_fetch.py. Source: NASA/JPL Horizons API",
        f"// (https://ssd.jpl.nasa.gov/api/horizons.api, API version {first['api_version']}),",
        "// a US-government work. Per request: target | center | sources.",
    ] + prov + [
        "",
        "// Observer tables (TIME_TYPE TT). Columns: request, body (NAIF/SPK-ID),",
        "// center (0 geo, 1 helio, 2 topo), site E-lon deg, lat deg, height km,",
        "// JD(TT), astrometric RA/Dec ICRF deg, apparent RA/Dec of date deg (EOP-",
        "// corrected IAU76/80), light-time range AU, apparent ecliptic lon/lat of",
        "// date deg (IAU76/80), TDB-UT s (UT1 before 1962, UTC after), local apparent",
        "// sidereal time h, RA/Dec/plane-of-sky 3-sigma arcsec. kNa: not requested.",
        f"const HorizonsObs kHorizonsObs[] = {{",
    ] + obs_rows + [
        "};",
        "",
        "// Vector tables: heliocentric geometric state, ICRF, AU and AU/day, at JD(TDB).",
        "const HorizonsVec kHorizonsVec[] = {",
    ] + vec_rows + ["};", ""]
    text = "\n".join(lines)

    def data(t):
        return [l for l in t.splitlines() if "retrieved" not in l]

    current = open(OUT).read() if os.path.exists(OUT) else ""
    if args.check:
        stale = data(current) != data(text)
        print(f"tests/horizons_corpus.inc: {'STALE' if stale else 'up to date'}")
        return 1 if stale else 0
    with open(OUT, "w") as f:
        f.write(text)
    print(f"tests/horizons_corpus.inc: {len(obs_rows)} observer rows, {len(vec_rows)} vector rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())

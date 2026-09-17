#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate src/star_catalog.inc, the fixed-star and Messier catalog (docs/STARS.md).

Inputs: the cache of tools/fetch/stars_fetch.py (verified against its pinned
checksums first) and data/star_names.txt (the curated traditional names).

- Stars: every entry of the Yale Bright Star Catalogue (V/50) that has a
  position.
  - Astrometry comes from the Hipparcos new reduction (I/311) through
    SIMBAD's HR-HIP cross-identifications: ICRS position at J1991.25,
    parallax, proper motions.
  - A star without a Hipparcos match keeps the Bright Star Catalogue's
    J2000 position, proper motion and parallax (flagged).
  - Radial velocities come from SIMBAD (quality A-C) where it has one, else
    from the Bright Star Catalogue; the two are compared.
- IAU names: each name of the IAU Catalog of Star Names attaches to its star
  by HIP number, or by HR number for the few without one. A named star
  outside the Bright Star Catalogue but in Hipparcos is added.
- Traditional names from data/star_names.txt: each must be found in Allen
  (1899); the passage matched is printed.
- Messier objects: SIMBAD positions (ICRS, J2000), object types and sizes;
  constellations from NASA HEASARC's Messier table.
- Constellation boundaries (Roman 1987, CDS VI/42) for finding the
  constellation of any position.

Cross-checks (fatal where identity is at stake, reported otherwise):
- The Bright Star Catalogue's own J2000 positions against Hipparcos.
- Each IAU name's HR, HIP and Bayer designations against the star it
  attaches to; its printed coordinates (reported: the page has errata).
- Each curated name against SIMBAD's NAME identifiers: a name SIMBAD gives
  to another star is refused. No name may sit on two objects.
- SIMBAD's Messier positions against HEASARC's.

Usage:
  gen_star_catalog.py --raw-dir stars-raw            # write src/star_catalog.inc
  gen_star_catalog.py --raw-dir stars-raw --check    # exit 1 if src/ is stale
"""
import argparse
import collections
import csv
import gzip
import html
import math
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(REPO, "src", "star_catalog.inc")
NAMES = os.path.join(REPO, "data", "star_names.txt")

GREEK = ["alp", "bet", "gam", "del", "eps", "zet", "eta", "the", "iot", "kap", "lam", "mu",
         "nu", "xi", "omi", "pi", "rho", "sig", "tau", "ups", "phi", "chi", "psi", "ome"]
GREEK_UNICODE = "αβγδεζηθικλμνξοπρστυφχψω"
SUPERSCRIPTS = {"¹": "1", "²": "2", "³": "3", "⁴": "4", "⁵": "5", "⁶": "6", "⁷": "7", "⁸": "8",
                "⁹": "9"}

# SIMBAD object types -> the catalog's deep-sky kinds (docs/STARS.md).
KIND_STAR, KIND_GALAXY, KIND_GLOBULAR, KIND_OPEN, KIND_NEBULA, KIND_PLANETARY, KIND_SNR, \
    KIND_ASTERISM, KIND_DOUBLE = range(9)
OTYPES = {
    "G": KIND_GALAXY, "GiG": KIND_GALAXY, "GiC": KIND_GALAXY, "BiC": KIND_GALAXY,
    "Sy2": KIND_GALAXY, "Sy1": KIND_GALAXY, "SyG": KIND_GALAXY, "LIN": KIND_GALAXY,
    "AGN": KIND_GALAXY, "rG": KIND_GALAXY, "H2G": KIND_GALAXY, "SBG": KIND_GALAXY,
    "EmG": KIND_GALAXY, "GiP": KIND_GALAXY, "IG": KIND_GALAXY, "PaG": KIND_GALAXY,
    "SFR": KIND_NEBULA, "HII": KIND_NEBULA, "RNe": KIND_NEBULA, "ISM": KIND_NEBULA,
    "MoC": KIND_NEBULA, "Cld": KIND_NEBULA,
    "GlC": KIND_GLOBULAR, "OpC": KIND_OPEN, "Cl*": KIND_OPEN, "As*": KIND_ASTERISM,
    "St*": KIND_ASTERISM, "PN": KIND_PLANETARY, "SNR": KIND_SNR, "**": KIND_DOUBLE,
}

HIP_EPOCH = 1991.25
J2000_EPOCH = 2000.0

ALLEN_MAX_EDITS = lambda n: 0 if n <= 5 else (1 if n <= 10 else 2)  # noqa: E731


def fail(msg):
    sys.exit("gen_star_catalog: " + msg)


def verify_cache(raw_dir):
    fetch = os.path.join(REPO, "tools", "fetch", "stars_fetch.py")
    r = subprocess.run([sys.executable, fetch, "--raw-dir", raw_dir, "--verify"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        fail("the cache does not match the pinned checksums:\n" + r.stdout)


def parse_bsc(raw_dir):
    stars = {}
    with gzip.open(os.path.join(raw_dir, "V_50_catalog.gz"), "rt", encoding="latin-1") as f:
        for line in f:
            line = line.rstrip("\n").ljust(197)
            if not line[75:77].strip():
                continue  # removed from the catalog: no position
            hr = int(line[0:4])
            name = line[4:14]
            flam = name[0:3].strip()
            bayer = name[3:6].strip().lower()
            sup = name[6:7].strip()
            con = name[7:10].strip()
            ra = (int(line[75:77]) + int(line[77:79]) / 60 + float(line[79:83]) / 3600) * 15
            dec = int(line[84:86]) + int(line[86:88]) / 60 + int(line[88:90]) / 3600
            if line[83] == "-":
                dec = -dec
            num = lambda s: float(s) if s.strip() else None  # noqa: E731
            stars[hr] = {
                "hr": hr, "hd": int(line[25:31]) if line[25:31].strip() else 0,
                "flamsteed": int(flam) if flam else 0,
                "bayer": GREEK.index(bayer) + 1 if bayer in GREEK else 0,
                "bayer_index": int(sup) if sup.isdigit() else 0,
                "con": con, "vmag": num(line[102:107]), "sp": line[127:147].strip(),
                "ra": ra, "dec": dec, "epoch": J2000_EPOCH,
                "pmra": (num(line[148:154]) or 0.0) * 1000.0,
                "pmdec": (num(line[154:160]) or 0.0) * 1000.0,
                "plx": (num(line[161:166]) or 0.0) * 1000.0 if line[160] != "D" else 0.0,
                "rv": num(line[166:170]) or 0.0,
                "hip": 0, "source": 1, "names": [], "kind": KIND_STAR, "messier": 0,
                "size": 0.0,
            }
    return stars


def parse_hip2(raw_dir):
    hip = {}
    with gzip.open(os.path.join(raw_dir, "I_311_hip2.dat.gz"), "rt") as f:
        for line in f:
            hip[int(line[0:6])] = {
                "ra": math.degrees(float(line[15:28])), "dec": math.degrees(float(line[29:42])),
                "plx": float(line[43:50]), "pmra": float(line[51:59]),
                "pmdec": float(line[60:68]), "hp": float(line[129:136]),
            }
    return hip


def parse_hr_hip(raw_dir):
    pairs = collections.defaultdict(set)
    with open(os.path.join(raw_dir, "simbad-hr-hip.csv"), newline="") as f:
        for row in csv.DictReader(f):
            hr = row["hr"].split()[1]
            hp = row["hip"].split()[1]
            # Component-suffixed HIP identifiers (13847A, 13847B) share one
            # I/311 row, which is the primary's: keep the A (or unsuffixed) one.
            m = re.fullmatch(r"(\d+)([A-Z]?)", hp)
            if hr.isdigit() and m and m.group(2) in ("", "A"):
                pairs[int(hr)].add(int(m.group(1)))
    return pairs


def parse_iau(raw_dir):
    text = open(os.path.join(raw_dir, "iau-csn.html"), encoding="utf-8").read()
    rows = []
    for tr in re.findall(r"<tr[^>]*>(.*?)</tr>", text, re.S):
        cells = [html.unescape(re.sub(r"<[^>]+>", "", c)).strip()
                 for c in re.findall(r"<td[^>]*>(.*?)</td>", tr, re.S)]
        if len(cells) >= 17:
            rows.append({"name": cells[0], "designation": cells[2],
                         "hip": int(cells[3]) if cells[3].isdigit() else 0,
                         "bayer": cells[4], "con": cells[6],
                         "ra": cells[13], "dec": cells[14], "mag": cells[15]})
    if len(rows) < 500:
        fail(f"the IAU page parsed to only {len(rows)} names")
    return rows


def parse_messier(raw_dir):
    out = {}
    with open(os.path.join(raw_dir, "simbad-messier.csv"), newline="") as f:
        for row in csv.DictReader(f):
            m = row["messier"].split()
            if len(m) != 2 or not m[1].isdigit():
                continue
            n = int(m[1])
            # SIMBAD's types for the two non-clusters: M 40 is the double star
            # Winnecke 4 ("?"), M 73 a four-star asterism ("err").
            row["otype"] = {40: "**", 73: "As*"}.get(n, row["otype"])
            if row["otype"] not in OTYPES:
                fail(f"M {n}: unmapped SIMBAD object type {row['otype']}")
            out[n] = {
                "messier": n, "ra": float(row["ra"]), "dec": float(row["dec"]),
                "kind": OTYPES[row["otype"]],
                "size": float(row["galdim_majaxis"]) if row["galdim_majaxis"] else 0.0,
                "main_id": row["main_id"],
            }
    if sorted(out) != list(range(1, 111)):
        fail(f"SIMBAD returned {len(out)} Messier objects, not M 1..M 110")
    return out


def separation_arcsec(ra1, dec1, ra2, dec2):
    r1, d1, r2, d2 = map(math.radians, (ra1, dec1, ra2, dec2))
    c = math.sin(d1) * math.sin(d2) + math.cos(d1) * math.cos(d2) * math.cos(r1 - r2)
    x = math.cos(d1) * math.sin(r1 - r2)
    y = math.cos(d2) * math.sin(d1) - math.sin(d2) * math.cos(d1) * math.cos(r1 - r2)
    return math.degrees(math.atan2(math.hypot(x, y), c)) * 3600.0


def at_j2000(star):
    """Catalog position carried linearly to J2000 by its proper motion (degrees)."""
    dt = J2000_EPOCH - star["epoch"]
    dec = star["dec"] + star["pmdec"] * dt / 3.6e6
    ra = star["ra"] + star["pmra"] * dt / 3.6e6 / max(1e-9, math.cos(math.radians(star["dec"])))
    return ra, dec


def name_key(name):
    return re.sub(r"[^a-z0-9]", "", name.lower())


def parse_simbad_rv(raw_dir):
    out = {}
    with open(os.path.join(raw_dir, "simbad-rv.csv"), newline="") as f:
        for row in csv.DictReader(f):
            h = row["hip"].split()[1]
            if h.isdigit() and row["rvz_radvel"]:
                out[int(h)] = float(row["rvz_radvel"])
    if len(out) < 80000:
        fail(f"SIMBAD radial velocities: only {len(out)} rows (a truncated query?)")
    return out


def parse_simbad_names(raw_dir):
    out = collections.defaultdict(set)  # name key -> HR numbers
    with open(os.path.join(raw_dir, "simbad-names.csv"), newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            hr = row["hr"].split()[1]
            if hr.isdigit():
                out[name_key(row["name"][5:])].add(int(hr))
    return out


def parse_heasarc_messier(raw_dir):
    out = {}
    with open(os.path.join(raw_dir, "heasarc-messier.txt"), encoding="utf-8") as f:
        for line in f:
            cells = [c.strip() for c in line.split("|")]
            m = re.fullmatch(r"M (\d+)", cells[0]) if cells else None
            if not m or len(cells) < 4:
                continue
            h, mi, se = (float(x) for x in cells[1].split())
            sign = -1.0 if cells[2].startswith("-") else 1.0
            d, dm, ds = (abs(float(x)) for x in cells[2].split())
            out[int(m.group(1))] = {"ra": (h + mi / 60 + se / 3600) * 15,
                                    "dec": sign * (d + dm / 60 + ds / 3600),
                                    "con": cells[3].capitalize()}
    # HEASARC leaves out M 102, whose identification (a duplicate of M 101,
    # or NGC 5866) has long been disputed; SIMBAD takes NGC 5866.
    if set(range(1, 111)) - set(out) - {102}:
        fail(f"HEASARC returned {len(out)} Messier objects")
    return out


class Allen:
    """Finds a name in the OCR text of Allen (1899), tolerating OCR damage."""

    def __init__(self, raw_dir):
        text = open(os.path.join(raw_dir, "allen-1899.txt"), encoding="utf-8",
                    errors="replace").read()
        self.words = [w for w in (re.sub(r"[^a-z]", "", t.lower()) for t in text.split()) if w]
        self.by_len = collections.defaultdict(list)
        for n in (1, 2, 3):
            for i in range(len(self.words) - n + 1):
                s = "".join(self.words[i:i + n])
                if len(s) <= 40:
                    self.by_len[len(s)].append((s, i))
        self.exact = {s for bucket in self.by_len.values() for s, _ in bucket}

    @staticmethod
    def distance(a, b, limit):
        prev = list(range(len(b) + 1))
        for i, ca in enumerate(a, 1):
            cur = [i] + [0] * len(b)
            for j, cb in enumerate(b, 1):
                cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb))
            if min(cur) > limit:
                return limit + 1
            prev = cur
        return prev[-1]

    def find(self, name):
        key = re.sub(r"[^a-z]", "", name.lower())
        if key in self.exact:
            return 0, key
        # The closest passage within the allowance: one edit before two.
        for limit in range(1, ALLEN_MAX_EDITS(len(key)) + 1):
            for length in range(len(key) - limit, len(key) + limit + 1):
                for s, i in self.by_len.get(length, ()):
                    if self.distance(key, s, limit) <= limit:
                        return limit, " ".join(self.words[i:i + 4])
        return None, None


def parse_key(key, stars):
    """A star_names.txt key -> the matching catalog key."""
    m = re.fullmatch(r"(HR|HIP|M) (\d+)", key)
    if m:
        return (m.group(1), int(m.group(2)))
    m = re.fullmatch(r"([a-z]{2,3})(\d?) ([A-Za-z]{3})", key)
    if m and m.group(1) in GREEK:
        return ("bayer", GREEK.index(m.group(1)) + 1, int(m.group(2) or 0), m.group(3))
    m = re.fullmatch(r"(\d+) ([A-Za-z]{3})", key)
    if m:
        return ("flamsteed", int(m.group(1)), m.group(2))
    fail(f"star_names.txt: cannot read key '{key}'")


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--raw-dir", required=True)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    verify_cache(args.raw_dir)

    stars = parse_bsc(args.raw_dir)
    hip = parse_hip2(args.raw_dir)
    hr_hip = parse_hr_hip(args.raw_dir)
    iau = parse_iau(args.raw_dir)
    messier = parse_messier(args.raw_dir)

    # Hipparcos stars on a 1-degree declination grid, at J2000, for the
    # positional fallback of stars SIMBAD gives no HIP number.
    grid = collections.defaultdict(list)
    for h, a in hip.items():
        ra, dec = at_j2000({"ra": a["ra"], "dec": a["dec"], "epoch": HIP_EPOCH,
                            "pmra": a["pmra"], "pmdec": a["pmdec"]})
        grid[int(math.floor(dec))].append((h, ra, dec, a["hp"]))

    def nearest_hip(ra, dec, vmag, radius=30.0):
        best = None
        for band in (int(math.floor(dec)) - 1, int(math.floor(dec)), int(math.floor(dec)) + 1):
            for h, hra, hdec, hp in grid.get(band, ()):
                if abs(hdec - dec) * 3600.0 > radius:
                    continue
                sep = separation_arcsec(ra, dec, hra, hdec)
                if sep <= radius and (vmag is None or abs(hp - vmag) < 1.0):
                    if best is None or sep < best[1]:
                        best = (h, sep)
        return best

    # Hipparcos astrometry for the Bright Star Catalogue.
    by_hip = {}
    bsc_vs_hip = []
    positional = 0
    for hr, s in stars.items():
        candidates = [h for h in hr_hip.get(hr, ()) if h in hip]
        if not candidates:
            near = nearest_hip(s["ra"], s["dec"], s["vmag"])
            if near is not None and near[0] not in by_hip:
                candidates = [near[0]]
                positional += 1
        if len(candidates) != 1:
            continue
        h = candidates[0]
        if h in by_hip:
            continue  # two HR entries on one HIP (a close pair): keep the first
        a = hip[h]
        # Cross-check: the Bright Star Catalogue's J2000 position (0.1 s, 1")
        # against Hipparcos carried to J2000. A wrong identification would be
        # arcminutes off.
        hra, hdec = at_j2000({"ra": a["ra"], "dec": a["dec"], "epoch": HIP_EPOCH,
                              "pmra": a["pmra"], "pmdec": a["pmdec"]})
        sep = separation_arcsec(s["ra"], s["dec"], hra, hdec)
        bsc_vs_hip.append(sep)
        if sep > 60.0:
            print(f"  HR {hr}: HIP {h} is {sep:.0f}\" from the BSC position; not used",
                  file=sys.stderr)
            continue
        s.update(hip=h, ra=a["ra"], dec=a["dec"], plx=a["plx"], pmra=a["pmra"],
                 pmdec=a["pmdec"], epoch=HIP_EPOCH, source=0)
        by_hip[h] = s
    by_hr = stars

    # IAU names.
    hip_to_hr = {}
    for hr, hips in hr_hip.items():
        for h in hips:
            if hr in stars and (h not in hip_to_hr or
                                (stars[hr]["vmag"] or 99) < (stars[hip_to_hr[h]]["vmag"] or 99)):
                hip_to_hr[h] = hr
    extras = []
    attached = unplaced = 0
    iau_sep = []
    iau_position_disagrees = []
    iau_bayer_checked = 0
    for row in iau:
        # An HR designation names the component; a HIP number may be shared
        # by a double's components.
        target = None
        if re.fullmatch(r"HR \d+", row["designation"]):
            target = by_hr.get(int(row["designation"][3:]))
        if target is None:
            target = by_hip.get(row["hip"])
        if target is None and row["hip"] in hip_to_hr:
            # A Bright Star Catalogue star whose Hipparcos entry was not used
            # for its astrometry (a close pair sharing one HIP number).
            target = by_hr.get(hip_to_hr[row["hip"]])
        if target is None and row["designation"].startswith("HR "):
            target = by_hr.get(int(row["designation"][3:]))
        if target is None and row["hip"] in hip:
            # A Bright Star Catalogue star at the Hipparcos position, which
            # SIMBAD did not cross-identify.
            a = hip[row["hip"]]
            ra, dec = at_j2000({"ra": a["ra"], "dec": a["dec"], "epoch": HIP_EPOCH,
                                "pmra": a["pmra"], "pmdec": a["pmdec"]})
            near = [st for st in stars.values()
                    if abs(st["dec"] - dec) < 0.02 and separation_arcsec(*at_j2000(st), ra, dec) < 30.0]
            if near:
                target = min(near, key=lambda st: st["vmag"] if st["vmag"] is not None else 99.0)
        if target is None and row["hip"] in hip:
            a = hip[row["hip"]]
            target = {
                "hr": 0, "hd": int(row["designation"][3:]) if row["designation"].startswith("HD ")
                and row["designation"][3:].isdigit() else 0,
                "flamsteed": 0, "bayer": 0, "bayer_index": 0, "con": row["con"],
                "vmag": float(row["mag"]) if re.fullmatch(r"-?\d+(\.\d+)?", row["mag"]) else a["hp"],
                "sp": "", "ra": a["ra"], "dec": a["dec"], "epoch": HIP_EPOCH, "pmra": a["pmra"],
                "pmdec": a["pmdec"], "plx": a["plx"], "rv": 0.0, "hip": row["hip"], "source": 0,
                "names": [], "kind": KIND_STAR, "messier": 0, "size": 0.0,
            }
            by_hip[row["hip"]] = target
            extras.append(target)
        if target is None:
            unplaced += 1
            continue
        # Cross-check the IAU row against the star it attached to: position
        # and Bayer designation.
        if re.fullmatch(r"-?\d+(\.\d+)?", row["ra"]) and re.fullmatch(r"-?\d+(\.\d+)?", row["dec"]):
            # The page prints Hipparcos-epoch (J1991.25) coordinates for some
            # stars and J2000 ones for others: compare with both.
            ra, dec = at_j2000(target)
            sep = min(separation_arcsec(ra, dec, float(row["ra"]), float(row["dec"])),
                      separation_arcsec(target["ra"], target["dec"], float(row["ra"]),
                                        float(row["dec"])))
            if sep > 60.0:
                # The IAU row's own HIP/HR number chose this star; its printed
                # coordinates disagree. Reported: an upstream transcription error.
                iau_position_disagrees.append(
                    f"{row['name']} (HIP {row['hip']}, {row['designation']}): IAU RA/Dec "
                    f"{row['ra']} {row['dec']} is {sep / 3600:.2f} deg from the star")
            else:
                iau_sep.append(sep)
        if target["bayer"] and row["bayer"]:
            letter = GREEK_UNICODE.find(row["bayer"][0]) + 1
            if letter and letter != target["bayer"]:
                fail(f"IAU {row['name']}: Bayer {row['bayer']} but the star is "
                     f"{GREEK[target['bayer'] - 1]} {target['con']}")
            iau_bayer_checked += 1
        if row["name"] not in target["names"]:
            target["names"].insert(len([n for n in target["names"] if n.startswith("\x00")]),
                                   row["name"])
        attached += 1

    # Radial velocities: SIMBAD's (quality A-C, mostly modern surveys) where it
    # has one, else the Bright Star Catalogue's. Cross-check the two.
    simbad_rv = parse_simbad_rv(args.raw_dir)
    rv_diffs = []
    rv_from_simbad = 0
    for obj in list(stars.values()) + extras:
        if obj["hip"] and obj["hip"] in simbad_rv:
            v = simbad_rv[obj["hip"]]
            if obj["hr"] and obj["rv"]:
                rv_diffs.append(abs(v - obj["rv"]))
            obj["rv"] = v
            rv_from_simbad += 1

    # Messier objects.
    deep = []
    for n in range(1, 111):
        m = messier[n]
        deep.append({
            "hr": 0, "hd": 0, "hip": 0, "flamsteed": 0, "bayer": 0, "bayer_index": 0, "con": "",
            "vmag": None, "sp": "", "ra": m["ra"], "dec": m["dec"], "epoch": J2000_EPOCH,
            "pmra": 0.0, "pmdec": 0.0, "plx": 0.0, "rv": 0.0, "source": 2, "names": [],
            "kind": m["kind"], "messier": n, "size": m["size"],
        })
    by_messier = {d["messier"]: d for d in deep}
    heasarc = parse_heasarc_messier(args.raw_dir)
    messier_sep = []
    messier_disagrees = []
    for n, d in by_messier.items():
        h = heasarc.get(n)
        if h is None:
            continue
        sep = separation_arcsec(d["ra"], d["dec"], h["ra"], h["dec"]) / 60.0
        messier_sep.append(sep)
        # HEASARC's positions are rounded to 0.1 min and 1'. An extended
        # object's centre is a matter of definition: allow half its size, and
        # 10' for the loose groups M 40 and M 73.
        allowed = max(5.0, d["size"] / 2.0, 10.0 if n in (40, 73) else 0.0)
        if sep > allowed:
            messier_disagrees.append(f"M {n}: SIMBAD and HEASARC positions {sep:.1f}' apart")
        d["con"] = h["con"]

    # Curated traditional names, each checked against Allen.
    allen = Allen(args.raw_dir)
    by_bayer = {}
    by_flamsteed = {}
    # A designation shared by a double's components means the brighter one.
    def brighter(a, b):
        if a is None:
            return b
        va = a["vmag"] if a["vmag"] is not None else 99.0
        vb = b["vmag"] if b["vmag"] is not None else 99.0
        return b if vb < va else a

    for s in list(stars.values()):
        if s["bayer"]:
            k = (s["bayer"], s["bayer_index"], s["con"].lower())
            by_bayer[k] = brighter(by_bayer.get(k), s)
        if s["flamsteed"]:
            k = (s["flamsteed"], s["con"].lower())
            by_flamsteed[k] = brighter(by_flamsteed.get(k), s)
    curated = 0
    simbad_names = parse_simbad_names(args.raw_dir)
    simbad_confirmed = 0
    for line_no, line in enumerate(open(NAMES, encoding="utf-8"), 1):
        line = line.split("#")[0].strip()
        if not line:
            continue
        if ":" not in line:
            fail(f"star_names.txt line {line_no}: expected KEY: NAMES")
        key, names = line.split(":", 1)
        k = parse_key(key.strip(), stars)
        if k[0] == "HR":
            target = by_hr.get(k[1])
        elif k[0] == "HIP":
            target = by_hip.get(k[1])
        elif k[0] == "M":
            target = by_messier.get(k[1])
        elif k[0] == "bayer":
            target = by_bayer.get((k[1], k[2], k[3].lower()))
        else:
            target = by_flamsteed.get((k[1], k[2].lower()))
        if target is None:
            fail(f"star_names.txt line {line_no}: no catalog object for '{key.strip()}'")
        for name in (n.strip() for n in names.split(",")):
            if not name:
                continue
            edits, passage = allen.find(name)
            if edits is None:
                fail(f"star_names.txt line {line_no}: '{name}' is not in Allen (1899)")
            print(f"  {key.strip():10s} {name:20s} Allen: {passage}"
                  + (f"  ({edits} edit{'s' if edits > 1 else ''})" if edits else ""),
                  file=sys.stderr)
            owners = simbad_names.get(name_key(name), set())
            if target["hr"] and owners and target["hr"] not in owners:
                fail(f"star_names.txt line {line_no}: SIMBAD gives '{name}' to HR "
                     f"{sorted(owners)}, not HR {target['hr']}")
            if target["hr"] and target["hr"] in owners:
                simbad_confirmed += 1
            if name.lower() not in (n.lower() for n in target["names"]):
                target["names"].append(name)
                curated += 1

    objects = [stars[hr] for hr in sorted(stars)] + sorted(extras, key=lambda s: s["hip"]) + deep

    # Every name identifies one object.
    seen = {}
    for o in objects:
        for n in o["names"]:
            k = name_key(n)
            if k in seen and seen[k] is not o:
                fail(f"the name '{n}' is on two objects (HR {seen[k]['hr']} M {seen[k]['messier']}, "
                     f"HR {o['hr']} M {o['messier']})")
            seen[k] = o

    def f(x, digits=12):
        return "NAN" if x is None else repr(round(x, digits))

    out = [
        "// Generated by tools/gen/gen_star_catalog.py -- do not edit. docs/STARS.md.",
        "// Sources (tools/fetch/stars_fetch.py, checksums pinned there):",
        "//   Yale Bright Star Catalogue, 5th rev. ed. (Hoffleit & Warren 1991), CDS V/50",
        "//   Hipparcos, the New Reduction (van Leeuwen 2007), CDS I/311",
        "//   SIMBAD (Wenger et al. 2000), CDS: HR-HIP identifications, names (a check),",
        "//   radial velocities, Messier objects; NASA HEASARC Messier table",
        "//   IAU Catalog of Star Names, IAU WGSN (CC BY 4.0)",
        "//   data/star_names.txt, checked against Allen, Star-Names and Their Meanings (1899)",
        "// This research has made use of the SIMBAD database and the VizieR catalogue",
        "// access tool, CDS, Strasbourg, France.",
        f"// {len(stars)} Bright Star Catalogue stars ({sum(1 for s in stars.values() if s['source'] == 0)}"
        f" with Hipparcos astrometry), {len(extras)} further IAU-named Hipparcos stars,",
        f"// {len(deep)} Messier objects; {attached} IAU names, {curated} curated names.",
        "// Fields: HR, HD, HIP, Flamsteed, Messier, Bayer letter (1 = alpha), Bayer index,",
        "// constellation, kind, astrometry source (0 Hipparcos, 1 BSC, 2 SIMBAD), V mag,",
        "// RA and Dec (deg, ICRS) at epoch (Julian year), pm RA*cos(Dec) and pm Dec (mas/yr),",
        "// parallax (mas), radial velocity (km/s), size (arcmin), spectral type, names ('|').",
        "constexpr StarRecord kStarRecords[] = {",
    ]
    for s in objects:
        out.append(
            "    {%d, %d, %d, %d, %d, %d, %d, %s, %d, %d, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s},"
            % (s["hr"], s["hd"], s["hip"], s["flamsteed"], s["messier"], s["bayer"],
               s["bayer_index"], c_str(s["con"]), s["kind"], s["source"],
               f(s["vmag"], 3), f(s["ra"], 10), f(s["dec"], 10), f(s["epoch"], 2),
               f(s["pmra"], 3), f(s["pmdec"], 3), f(s["plx"], 3), f(s["rv"], 2),
               f(s["size"], 2), c_str(s["sp"]), c_str("|".join(s["names"]))))
    out.append("};")

    # Constellation boundaries (Delporte 1930 as arranged by Roman 1987):
    # the southern edge of each region, in order of declination.
    bounds = []
    with open(os.path.join(args.raw_dir, "VI_42_data.dat")) as fb:
        for line in fb:
            parts = line.split()
            if len(parts) == 4:
                bounds.append((float(parts[0]), float(parts[1]), float(parts[2]), parts[3]))
    if len(bounds) != 357 or len({b[3] for b in bounds}) != 88:
        fail(f"constellation boundaries: {len(bounds)} rows, {len({b[3] for b in bounds})} names")
    out += [
        "",
        "// Constellation boundaries, CDS VI/42 (Roman 1987, PASP 99, 695, from Delporte",
        "// 1930): RA from, RA to (hours), southern Dec (deg), equinox B1875.0.",
        "// A position belongs to the first row with Dec >= its Dec and RA in [from, to).",
        "constexpr ConstellationBoundary kConstellationBoundaries[] = {",
    ]
    for lo, hi, dec, con in bounds:
        out.append(f"    {{{lo!r}, {hi!r}, {dec!r}, {c_str(con)}}},")
    out.append("};")
    text = "\n".join(out) + "\n"

    def pct(v, q):
        v = sorted(v)
        return v[min(len(v) - 1, int(q * len(v)))]
    for note in messier_disagrees:
        print(f"  warning: {note}", file=sys.stderr)
    if len(messier_disagrees) > 3:
        fail(f"{len(messier_disagrees)} Messier positions disagree between SIMBAD and HEASARC")
    for note in iau_position_disagrees:
        print(f"  warning: {note}", file=sys.stderr)
    # The page's own HIP, HR and Bayer designations decide identity (checked
    # above, fatal on disagreement); its printed coordinates contain errors.
    print(f"cross-checks: BSC vs Hipparcos at J2000 median {pct(bsc_vs_hip, 0.5):.1f}\" "
          f"99% {pct(bsc_vs_hip, 0.99):.1f}\" max {max(bsc_vs_hip):.1f}\" ({len(bsc_vs_hip)} stars); "
          f"IAU positions median {pct(iau_sep, 0.5):.2f}\" max {max(iau_sep):.1f}\", "
          f"{iau_bayer_checked} Bayer letters agree; curated names confirmed by SIMBAD "
          f"{simbad_confirmed}; {positional} Hipparcos matches by position; radial velocities from "
          f"SIMBAD {rv_from_simbad}, BSC vs SIMBAD median {pct(rv_diffs, 0.5):.1f} km/s, "
          f"{sum(1 for d in rv_diffs if d > 10)} of {len(rv_diffs)} over 10 km/s; Messier SIMBAD vs HEASARC median {pct(messier_sep, 0.5):.2f}' "
          f"max {max(messier_sep):.1f}'", file=sys.stderr)
    print(f"{len(stars)} BSC stars, {len(extras)} extra IAU stars, {len(deep)} Messier; "
          f"{attached} IAU names attached, {unplaced} without a Hipparcos star; "
          f"{curated} curated names", file=sys.stderr)
    if args.check:
        stale = not os.path.exists(OUT) or open(OUT).read() != text
        print(f"{OUT}: {'stale' if stale else 'up to date'}")
        return 1 if stale else 0
    with open(OUT, "w") as fo:
        fo.write(text)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

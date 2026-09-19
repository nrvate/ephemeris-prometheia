#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetch the source data of the fixed-star catalog (docs/STARS.md).

Every source is declared below with its terms. The tool fetches strictly
sequentially, with a pause between requests, an identifying User-Agent and
backoff-only retries. Files are cached verbatim under --raw-dir (gitignored),
with a manifest of URL, retrieval time, size and SHA-256. A cached file is
not fetched again; --force refetches.

Pinned checksums: each source carries the SHA-256 of the copy the committed
catalog was generated from. --verify checks the cache against them. A source
that has changed upstream (a new IAU name, a SIMBAD update) shows up as a
mismatch; refresh deliberately: refetch, regenerate, review the diff, pin the
new checksum.

tools/gen/gen_star_catalog.py turns the cache into src/star_catalog.inc.

Usage:
  stars_fetch.py --list                         # print the sources, fetch nothing
  stars_fetch.py --raw-dir stars-raw            # fetch what is missing
  stars_fetch.py --raw-dir stars-raw --verify   # check the cache against the pins
  stars_fetch.py --raw-dir stars-raw --only iau-csn --force
"""
import argparse
import datetime
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fetchlog  # noqa: E402  (one timestamped line per GET)

USER_AGENT = ("prometheia-fetch/0.1.0 "
              "(Ephemeris Prometheia fixed-star catalog; sequential, 20 requests)")
PAUSE_S = 5.0

CDS = "https://cdsarc.cds.unistra.fr/ftp/"
SIMBAD_TAP = "https://simbad.cds.unistra.fr/simbad/sim-tap/sync"


def tap(query):
    # MAXREC above SIMBAD's default of 50,000 rows, which truncates silently.
    return SIMBAD_TAP + "?" + urllib.parse.urlencode(
        {"request": "doQuery", "lang": "adql", "format": "csv", "maxrec": "500000", "query": query})


# name -> (file, url, pinned sha256 or None, terms)
SOURCES = [
    ("bsc5-readme", "V_50_ReadMe", CDS + "V/50/ReadMe", "44fd9c73e2eecad0beb47bdfa3f01c60fd43f93d6964198e31fcd48732de5b33",
     "Yale Bright Star Catalogue, 5th ed. (Hoffleit & Warren 1991), CDS V/50; "
     "CDS: free use with acknowledgement"),
    ("bsc5", "V_50_catalog.gz", CDS + "V/50/catalog.gz", "3dc44b1e90be8fbe5bcc7656032560f51275f985c7e3f783c9028e1838ec7bed",
     "as bsc5-readme"),
    ("hip2-readme", "I_311_ReadMe", CDS + "I/311/ReadMe", "6c017925d658447d40983da3b459ba16bf9789c8f7573e263952c75692622499",
     "Hipparcos, the New Reduction (van Leeuwen 2007), CDS I/311; "
     "CDS: free use with acknowledgement"),
    ("hip2", "I_311_hip2.dat.gz", CDS + "I/311/hip2.dat.gz", "8e624f843d4254a9b7c2e8dda8e3158dbe825bf98f6bf3dbbae7f0d0b73d6858",
     "as hip2-readme"),
    ("iau-csn", "iau-csn.html", "https://exopla.net/star-names/modern-iau-star-names/", "280cf66d26f87876cb4bb5b479436aeb70ce28f52ea70edbad80065e54c024d1",
     "IAU Catalog of Star Names, IAU Division C WGSN; IAU products are CC BY 4.0"),
    ("allen-1899", "allen-1899.txt",
     "https://archive.org/download/starnamesandthe00allegoog/starnamesandthe00allegoog_djvu.txt",
     "6c8f916a27a8a342ded394d409157021758db04e4f155d0b8218482eb20d3f91", "R. H. Allen, Star-Names and Their Meanings (G. E. Stechert, 1899), OCR text of the "
     "Internet Archive scan; public domain. Checks the curated traditional names"),
    ("simbad-hr-hip", "simbad-hr-hip.csv",
     tap("SELECT a.id AS hr, b.id AS hip FROM ident AS a JOIN ident AS b "
         "ON a.oidref = b.oidref WHERE a.id LIKE 'HR %' AND b.id LIKE 'HIP %'"),
     "4efb245a74e52a7bba1619fc5b98c52dee38aa801d1e770c510021e865c70b91", "SIMBAD (Wenger et al. 2000), CDS: free use with acknowledgement"),
    ("simbad-names", "simbad-names.csv",
     tap("SELECT a.id AS hr, b.id AS name FROM ident AS a JOIN ident AS b "
         "ON a.oidref = b.oidref WHERE a.id LIKE 'HR %' AND b.id LIKE 'NAME %'"),
     "85bcb9f0cfff72dd96b1f33fe697bd1ff878ea2477539d60efb5977421e58509", "as simbad-hr-hip; cross-checks which star each curated name belongs to"),
    ("heasarc-messier", "heasarc-messier.txt",
     "https://heasarc.gsfc.nasa.gov/xamin/query?table=messier"
     "&fields=name,ra,dec,constell,object_type&format=text&resultmax=0&sortvar=name",
     "1ca05beaece80b825d55c00074980a23920a0f97c7b684faffe3daf4c098ed68", "NASA HEASARC Messier Nebulae table (from Sky Catalogue 2000.0 vol. 2), "
     "US Government service; cross-checks the Messier positions, gives constellations"),
    ("roman-1987-readme", "VI_42_ReadMe", CDS + "VI/42/ReadMe", "b6a3e9ec21f902df084406e97d71754671c5787bee1cf49c109b3bc8cfc50ab9",
     "Identification of a Constellation from Position (Roman 1987, PASP 99, 695), CDS VI/42; "
     "CDS: free use with acknowledgement"),
    ("roman-1987", "VI_42_data.dat", CDS + "VI/42/data.dat", "daf9e2b39ec57446d862a445276ae2ea50490ee455540972906098f5f9187957", "as roman-1987-readme"),
    ("fk5-readme", "I_149A_ReadMe", CDS + "I/149A/ReadMe", "9b35679eb2bc623538baecbb1a4f94d1e7522c58928feaf8509a34b6ce59f7d8",
     "Fifth Fundamental Catalogue, part I (Fricke et al. 1988), CDS I/149A; "
     "CDS: free use with acknowledgement. Ground-based and pre-Hipparcos: the "
     "outside check on the Hipparcos-derived positions (docs/STARS.md)"),
    ("fk5", "I_149A_catalog.gz", CDS + "I/149A/catalog.gz", "67a228e2adacdb559ea5ec8305ca84d1d81f278af70483aac46beacd8e308a13", "as fk5-readme"),
    ("swisseph-doc", "swisseph.htm", "https://www.astro.com/swisseph/swisseph.htm", "7ca3f8863d31001f3a1411139d4759813cc8c3d3c51312fbc45aa84382f05aa2",
     "Swiss Ephemeris general documentation (Astrodienst), published; read for the "
     "published definitions of the star- and frame-anchored zodiacs (docs/FRAMES.md), "
     "never copied. Its code is never read (CLAUDE.md, Cleanroom)"),
    ("simbad-sgra", "simbad-sgra.csv",
     tap("SELECT b.main_id, b.ra, b.dec, b.coo_err_maj, b.coo_bibcode, b.pmra, b.pmdec, "
         "b.pm_bibcode FROM ident AS i JOIN basic AS b ON i.oidref = b.oid "
         "WHERE i.id = 'NAME Sgr A*'"),
     "cfaacc0dbb56c7a4f1eac72f76cfe545ed54f7ed42888bf56c5fd56a691f6690", "as simbad-hr-hip; the Galactic Centre (Sgr A*) for the galactic-centre zodiacs"),
    ("liu-2011", "liu-2011-abs.html", "https://arxiv.org/abs/1010.3773", "d16888d88e5cd1a24b939aee8655395b275c6f51cae8f4b20ae0dd9c501bd3fd",
     "Liu, Zhu & Zhang, Reconsidering the galactic coordinate system, A&A 526, A16 "
     "(2011), arXiv abstract page (arXiv's own terms: abstracts free to read); the "
     "modern galactic pole of galequ-true and galequ-mula. Facts only"),
    ("liu-2011-pdf", "liu-2011.pdf", "https://arxiv.org/pdf/1010.3773", "f5dc2c8bc5a2f421a42e3d88b4e9858364177fa51e99fd332eea7a5fd992cc9e", "as liu-2011; the paper, for the pole's coordinates"),
    ("reid-2020", "reid-2020-abs.html", "https://arxiv.org/abs/2001.04386", "611f288a2e49c3533bd922c67bb0846e4e87ec0afd601d1a92d398a43407f813",
     "Reid & Brunthaler, The Proper Motion of Sagittarius A*. III (ApJ 892, 39, 2020), "
     "arXiv abstract page; Sgr A*'s apparent proper motion. Facts only"),
    ("simbad-rv", "simbad-rv.csv",
     tap("SELECT i.id AS hip, b.rvz_radvel, b.rvz_err, b.rvz_qual FROM ident AS i "
         "JOIN basic AS b ON i.oidref = b.oid WHERE i.id LIKE 'HIP %' "
         "AND b.rvz_type = 'v' AND b.rvz_radvel IS NOT NULL AND b.rvz_qual IN ('A', 'B', 'C')"),
     "bc7cff05d5dbac59f53aae8fa5b8ecf4c6686ce26981f8fc57d4c5363a2887a8", "as simbad-hr-hip; radial velocities (quality A-C) of Hipparcos stars"),
    ("simbad-messier", "simbad-messier.csv",
     tap("SELECT i.id AS messier, b.main_id, b.ra, b.dec, b.otype, b.galdim_majaxis, "
         "b.galdim_minaxis FROM ident AS i JOIN basic AS b ON i.oidref = b.oid "
         "WHERE i.id LIKE 'M %'"),
     "12e77ad54ff8047c2fea163567bf1065c39931c5240c6c86e5153c6bec425633", "as simbad-hr-hip"),
]


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch(url):
    delay = PAUSE_S
    for attempt in range(4):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            fetchlog.log(f"GET {url}")
            with fetchlog.Timer() as t, urllib.request.urlopen(req, timeout=300) as resp:
                body = resp.read()
            fetchlog.log(f"  {resp.status} {len(body)} bytes {t.seconds:.1f} s")
            return body
        except urllib.error.HTTPError as exc:
            # A client error (a rejected query) will not succeed on retry.
            if exc.code < 500 or attempt == 3:
                fetchlog.log(f"  failed: {exc}")
                raise
            fetchlog.log(f"  retry after {delay:.0f} s: {exc}")
            time.sleep(delay)
            delay *= 2
        except Exception as exc:  # noqa: BLE001 - retry any transport error
            if attempt == 3:
                fetchlog.log(f"  failed: {exc}")
                raise
            fetchlog.log(f"  retry after {delay:.0f} s: {exc}")
            time.sleep(delay)
            delay *= 2
    raise AssertionError("unreachable")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--raw-dir", default="stars-raw")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--verify", action="store_true")
    args = ap.parse_args()

    chosen = [s for s in SOURCES if not args.only or s[0] in args.only]
    if args.list:
        for name, file, url, pin, terms in chosen:
            print(f"{name}\n  {url}\n  -> {file}  pinned {pin or '(not pinned)'}\n  {terms}")
        return 0

    os.makedirs(args.raw_dir, exist_ok=True)
    manifest_path = os.path.join(args.raw_dir, "manifest.json")
    manifest = {}
    if os.path.exists(manifest_path):
        with open(manifest_path) as f:
            manifest = json.load(f)

    if args.verify:
        bad = 0
        for name, file, url, pin, _ in chosen:
            path = os.path.join(args.raw_dir, file)
            if not os.path.exists(path):
                print(f"{name}: missing")
                bad += 1
                continue
            digest = sha256_file(path)
            state = "ok" if digest == pin else ("not pinned" if pin is None else "CHANGED")
            if state != "ok":
                bad += 1
            print(f"{name}: {state} {digest}")
        return 1 if bad else 0

    first = True
    for name, file, url, pin, _ in chosen:
        path = os.path.join(args.raw_dir, file)
        if os.path.exists(path) and not args.force:
            print(f"{name}: cached")
            continue
        if not first:
            time.sleep(PAUSE_S)
        first = False
        print(f"{name}: GET {url}")
        body = fetch(url)
        with open(path + ".part", "wb") as f:
            f.write(body)
        os.replace(path + ".part", path)
        digest = hashlib.sha256(body).hexdigest()
        manifest[name] = {
            "file": file, "url": url, "bytes": len(body), "sha256": digest,
            "retrieved": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        }
        with open(manifest_path, "w") as f:
            json.dump(manifest, f, indent=2, sort_keys=True)
        note = "" if pin is None else (" (matches pin)" if digest == pin else " (DIFFERS from pin)")
        print(f"  {len(body)} bytes sha256 {digest}{note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

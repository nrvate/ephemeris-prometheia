#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetch NASA's solar-eclipse reference tables, with checksums.

The solar-eclipse engine (docs/SOLAR_ECLIPSES.md) is validated against two
NASA/GSFC tables (US-government work, no strings attached). Neither is
committed; both are acquired here and verified against pinned SHA-256s:

  * besselian: the Besselian elements of every solar eclipse in the Five
    Millennium Canon (11,898 rows; x, y, d, mu, l1, l2, tan f1, tan f2 at
    t0, plus the polynomial coefficients) -- the oracle for x and y;
  * canon: the Five Millennium Canon of Solar Eclipses catalog
    (5MCSEcatalog.txt): date, delta T, type, saros, gamma, magnitude,
    greatest-eclipse position.

Politeness (CLAUDE.md): strictly sequential, a pause between requests, a
generic User-Agent, streamed to `<file>.part` and renamed only when whole,
retries only on transport errors and 5xx, `--probe` sends HEAD only.

Tests read them through PROMETHEIA_SOLAR_BESSELIAN and PROMETHEIA_SOLAR_CANON
(the file paths); they skip when unset.

Usage:
    tools/fetch/solar_eclipse_fetch.py --list
    tools/fetch/solar_eclipse_fetch.py --dir ephe/eclipse --probe
    tools/fetch/solar_eclipse_fetch.py --dir ephe/eclipse
    tools/fetch/solar_eclipse_fetch.py --dir ephe/eclipse --verify
"""

import argparse
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fetchlog  # noqa: E402

USER_AGENT = "curl/8.5.0"  # generic: nothing that identifies us (CLAUDE.md)
BASE = "https://eclipse.gsfc.nasa.gov/"
PAUSE_S = 5.0
CHUNK = 1 << 20

# name -> (path under BASE, pinned sha256 or None). The server dated the
# Besselian file 2024-03-25; a changed file shows as CHANGED, never silently.
FILES = {
    "eclipse_besselian_from_mysqldump2.csv": (
        "eclipse_besselian_from_mysqldump2.csv",
        "44460be3ed5a5c69a7627af6ffa875c82c70872f067e2907d20b49068e792b44"),
    "5MCSEcatalog.txt": ("5MCSE/5MCSEcatalog.txt",
                         "9967c3ae0e8bd761659c14bf18dce044927f114a71b96e722cea31bfcd97a344"),
}


def request(url, method="GET"):
    """Open url, retrying transport errors and 5xx with backoff; never 4xx."""
    for wait in (0, 5, 10, 20, 40):
        if wait:
            fetchlog.log(f"  retry in {wait} s")
            time.sleep(wait)
        fetchlog.log(f"{method} {url}")
        try:
            req = urllib.request.Request(url, method=method, headers={"User-Agent": USER_AGENT})
            return urllib.request.urlopen(req, timeout=120)
        except urllib.error.HTTPError as e:
            fetchlog.log(f"  HTTP {e.code}")
            if e.code < 500:
                raise
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
            fetchlog.log(f"  {e}")
    raise SystemExit(f"giving up on {url}")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(CHUNK), b""):
            h.update(block)
    return h.hexdigest()


def download(url, path):
    """Stream url to path via path.part; return (bytes, sha256)."""
    part = path + ".part"
    h = hashlib.sha256()
    n = 0
    with fetchlog.Timer() as t, request(url) as resp, open(part, "wb") as out:
        expected = int(resp.headers.get("Content-Length") or 0)
        for block in iter(lambda: resp.read(CHUNK), b""):
            out.write(block)
            h.update(block)
            n += len(block)
    if expected and n != expected:
        raise SystemExit(f"{url}: {n} bytes arrived of {expected}; {part} kept, not renamed")
    os.replace(part, path)
    fetchlog.log(f"  200 {n} bytes {t.seconds:.1f} s")
    return n, h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=None, help="where the files go (required unless --list)")
    ap.add_argument("--list", action="store_true", help="print the declared files and pins")
    ap.add_argument("--probe", action="store_true", help="HEAD each file: name and size only")
    ap.add_argument("--verify", action="store_true", help="check files already in --dir")
    ap.add_argument("--force", action="store_true", help="fetch even if present")
    args = ap.parse_args()
    if args.list:
        for name, (rel, pin) in FILES.items():
            print(f"{BASE}{rel}\n  pinned {pin or '(not pinned)'}")
        return 0
    if not args.dir:
        ap.error("--dir is required")
    os.makedirs(args.dir, exist_ok=True)
    manifest_path = os.path.join(args.dir, "solar_eclipse_manifest.json")
    manifest = json.load(open(manifest_path)) if os.path.exists(manifest_path) else {}
    bad = 0
    first = True
    for name, (rel, pin) in FILES.items():
        path = os.path.join(args.dir, name)
        url = BASE + rel
        if args.verify:
            if not os.path.exists(path):
                print(f"MISSING  {name}")
                bad += 1
                continue
            digest = sha256_file(path)
            state = "ok" if digest == pin else ("not pinned" if pin is None else "CHANGED")
            bad += state == "CHANGED"
            print(f"{state:10s} {name} {digest}")
            continue
        if not first:
            time.sleep(PAUSE_S)
        first = False
        if args.probe:
            with request(url, "HEAD") as resp:
                size = int(resp.headers.get("Content-Length") or 0)
            print(f"{name}: {size} bytes")
            continue
        if os.path.exists(path) and not args.force:
            print(f"present  {name} (use --force to refetch; --verify to check)")
            continue
        n, digest = download(url, path)
        note = "" if pin is None else (" (matches pin)" if digest == pin else " (DIFFERS from pin)")
        bad += pin is not None and digest != pin
        print(f"{name}: {n} bytes sha256 {digest}{note}")
        manifest[name] = {"url": url, "bytes": n, "sha256": digest,
                          "fetched_utc": fetchlog.utc()}
        with open(manifest_path, "w") as f:
            json.dump(manifest, f, indent=2, sort_keys=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

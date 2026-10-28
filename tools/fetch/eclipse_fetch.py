#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetch NASA's Five Millennium Catalog of Lunar Eclipses, with a checksum.

The lunar-eclipse comparison (tests/test_lunar_eclipse.cpp, docs/ECLIPSES.md)
is graded against the catalog of the Five Millennium Canon of Lunar Eclipses,
-1999 to +3000. It is one text file of ~1.4 MB, not committed; it is acquired
here and verified against a pinned SHA-256.

Credit, as NASA asks: "Eclipse Predictions by Fred Espenak, NASA's GSFC".
The catalog is a US-government work, public domain.

  * one file, one request; a generic User-Agent, nothing that identifies us;
  * streamed to `<file>.part` and renamed only when whole;
  * retries only on transport errors and 5xx, with backoff.

Usage:
    tools/fetch/eclipse_fetch.py --list
    tools/fetch/eclipse_fetch.py --dir /nvm/work/ephe/eclipse --probe
    tools/fetch/eclipse_fetch.py --dir /nvm/work/ephe/eclipse
    tools/fetch/eclipse_fetch.py --dir /nvm/work/ephe/eclipse --verify
Then PROMETHEIA_LUNAR_CANON=<dir>/5MKLEcatalog.txt build/test_lunar_eclipse.
"""

import argparse
import hashlib
import json
import os
import sys
import urllib.error
import urllib.request
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fetchlog  # noqa: E402

USER_AGENT = "curl/8.5.0"  # generic: nothing that identifies us (CLAUDE.md)
BASE = "https://eclipse.gsfc.nasa.gov/5MCLE/"
CHUNK = 1 << 20

# file -> pinned sha256 (None until the first fetch has been checked).
FILES = [("5MKLEcatalog.txt",
          "d47586fc9c1c59338f234b3c6634c31744739887169f58d411ac766f3861fcf2")]


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
    ap.add_argument("--dir", help="where the file goes (required unless --list)")
    ap.add_argument("--list", action="store_true", help="print the declared file and pin")
    ap.add_argument("--probe", action="store_true", help="HEAD only: name and size")
    ap.add_argument("--verify", action="store_true", help="check the file already in --dir")
    ap.add_argument("--force", action="store_true", help="fetch even if present")
    args = ap.parse_args()
    if args.list:
        for name, pin in FILES:
            print(f"{BASE}{name}\n  pinned {pin or '(not pinned)'}")
        return 0
    if not args.dir:
        ap.error("--dir is required")
    os.makedirs(args.dir, exist_ok=True)
    manifest_path = os.path.join(args.dir, "eclipse_manifest.json")
    manifest = json.load(open(manifest_path)) if os.path.exists(manifest_path) else {}
    bad = 0
    for name, pin in FILES:
        path, url = os.path.join(args.dir, name), BASE + name
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
        if args.probe:
            with request(url, "HEAD") as resp:
                print(f"{name}: {int(resp.headers.get('Content-Length') or 0)} bytes")
            continue
        if os.path.exists(path) and not args.force:
            print(f"present  {name} (use --force to refetch; --verify to check)")
            continue
        n, digest = download(url, path)
        note = "" if pin is None else (" (matches pin)" if digest == pin else " (DIFFERS from pin)")
        bad += pin is not None and digest != pin
        print(f"{name}: {n} bytes sha256 {digest}{note}")
        manifest[name] = {"url": url, "bytes": n, "sha256": digest, "fetched_utc": fetchlog.utc()}
        with open(manifest_path, "w") as f:
            json.dump(manifest, f, indent=2, sort_keys=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

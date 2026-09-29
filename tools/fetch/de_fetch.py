#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetch JPL planetary ephemeris binaries (DE440, DE441) with checksums.

The engine reads JPL's DE binaries directly (docs/DE.md). They are not
committed -- DE441 is ~2.6 GB -- so they are acquired here, by committed
machinery, and verified against pinned SHA-256s:

  * strictly sequential, one file at a time, a pause between requests;
  * an identifying User-Agent;
  * streamed to `<file>.part` and renamed only when whole, hashing as it
    goes, so an interrupted pull never leaves a plausible-looking file;
  * retries only on transport errors and 5xx, with backoff (a 4xx means the
    request itself was refused, and repeating it helps nobody);
  * `--probe` sends HEAD requests only, to confirm names and sizes before a
    large download is started.

JPL's planetary ephemerides are US-government work. Source:
https://ssd.jpl.nasa.gov/ftp/eph/planets/Linux/ (docs/DE.md).

Usage:
    tools/fetch/de_fetch.py --list
    tools/fetch/de_fetch.py --dir /nvm/work/ephe --only de441 --probe
    tools/fetch/de_fetch.py --dir /nvm/work/ephe --only de441
    tools/fetch/de_fetch.py --dir /nvm/work/ephe --verify
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

USER_AGENT = ("prometheia-fetch/0.1.0 (Ephemeris Prometheia; JPL DE binaries; "
              "strictly sequential, one file at a time)")
BASE = "https://ssd.jpl.nasa.gov/ftp/eph/planets/Linux/"
PAUSE_S = 5.0
CHUNK = 1 << 20

# set -> [(file, pinned sha256 or None)]. DE440's pins are the ones in ephe/SHA256SUMS
# since 2026-09-16; DE441's from its first fetch, 2026-09-29.
SETS = {
    "de440": [
        ("linux_p1550p2650.440",
         "29915576d0a6555766b99485ac3056ee415e86df4fce282611c31afb329ad062"),
        ("header.440", "0f4636b663e6f00af9efa2b605ef23635489f56b23dd1a5e3821d3acc6ac3359"),
        ("testpo.440", "45084a7f406b8e41032e7d99dd8042d4df97a9f752a68c06d1a27af16693429f"),
    ],
    "de441": [
        ("linux_m13000p17000.441",
         "476096486def4e41bfceb29aa27f50784da0bce318902bcf7b88caad058cd4da"),
        ("header.441",
         "376cd6f6766356ba0f4c25dc5a7ff8130ea2826f3c34149e33c1e6ac970d6e7c"),
        ("testpo.441",
         "e60c08ced7741a07d4dfe89b124afb4b1648bd1e3d0eb68dd9a79bf6990bb9c2"),
    ],
}


def url_of(de, name):
    return f"{BASE}{de}/{name}"


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
    ap.add_argument("--only", action="append", choices=sorted(SETS), help="set(s) to act on")
    ap.add_argument("--list", action="store_true", help="print the declared files and pins")
    ap.add_argument("--probe", action="store_true", help="HEAD each file: name and size only")
    ap.add_argument("--verify", action="store_true", help="check files already in --dir")
    ap.add_argument("--force", action="store_true", help="fetch even if present")
    args = ap.parse_args()
    sets = args.only or sorted(SETS)
    todo = [(de, name, pin) for de in sets for name, pin in SETS[de]]
    if args.list:
        for de, name, pin in todo:
            print(f"{url_of(de, name)}\n  pinned {pin or '(not pinned)'}")
        return 0
    if not args.dir:
        ap.error("--dir is required")
    os.makedirs(args.dir, exist_ok=True)
    manifest_path = os.path.join(args.dir, "de_manifest.json")
    manifest = json.load(open(manifest_path)) if os.path.exists(manifest_path) else {}
    bad = 0
    first = True
    for de, name, pin in todo:
        path = os.path.join(args.dir, name)
        url = url_of(de, name)
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
            print(f"{name}: {size} bytes ({size / 1e9:.2f} GB)")
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

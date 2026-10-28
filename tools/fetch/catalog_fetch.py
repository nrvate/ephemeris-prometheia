#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetch a published small-body catalog (.epm) from this repository's GitHub
releases, verified against its pinned SHA-256.

Catalogs are release assets, not tree files (docs/INGESTION.md,
"Distribution"). Each one was made by the committed sbdb_fetch.py and
prometheia-convert of its release's tag; this fetches the result instead of
pulling SBDB again. It downloads the way de_fetch.py does: one file, a
generic User-Agent, streamed to `<file>.part` and renamed only when whole
and matching its pin.

Usage:
    tools/fetch/catalog_fetch.py --list
    tools/fetch/catalog_fetch.py --dir .                  # the newest catalog
    tools/fetch/catalog_fetch.py --dir . --only sbdb-20261027.epm
    tools/fetch/catalog_fetch.py --dir . --verify
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from de_fetch import download, sha256_file  # noqa: E402

BASE = "https://github.com/nrvate/ephemeris-prometheia/releases/download/"

# Newest first: (asset, release tag, bytes, pinned sha256).
CATALOGS = [
    ("sbdb-20261027.epm", "catalog-20261027", 147254154,
     "1dd348aa203f341e0c499911262ea1265cf9a47ea9b0305f65eea85c23959919"),
    ("sbdb-full-20260916.epm", "v0.1.0", 136692088,
     "35ae8b889a5646ad2481a8357ca09d818eff426a30cedee6f366dc2128d23ba4"),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=None, help="where the file goes (required unless --list)")
    ap.add_argument("--only", choices=[c[0] for c in CATALOGS], help="catalog (default: newest)")
    ap.add_argument("--list", action="store_true", help="print the published catalogs and pins")
    ap.add_argument("--verify", action="store_true", help="check the file already in --dir")
    ap.add_argument("--force", action="store_true", help="fetch even if present")
    args = ap.parse_args()
    if args.list:
        for name, tag, size, pin in CATALOGS:
            print(f"{BASE}{tag}/{name}\n  {size} bytes, pinned {pin}")
        return 0
    if not args.dir:
        ap.error("--dir is required")
    name, tag, size, pin = next(c for c in CATALOGS if c[0] == (args.only or CATALOGS[0][0]))
    path = os.path.join(args.dir, name)
    if args.verify or (os.path.exists(path) and not args.force):
        if not os.path.exists(path):
            print(f"MISSING  {name}")
            return 1
        digest = sha256_file(path)
        state = "ok" if digest == pin else "CHANGED"
        print(f"{state:10s} {name} {digest}"
              + ("" if args.verify else " (present; use --force to refetch)"))
        return 0 if digest == pin else 1
    os.makedirs(args.dir, exist_ok=True)
    n, digest = download(f"{BASE}{tag}/{name}", path + ".fetched")
    if digest != pin or n != size:
        print(f"{name}: {n} bytes sha256 {digest} DIFFERS from pin; {path}.fetched kept")
        return 1
    os.replace(path + ".fetched", path)
    with open(path + ".sha256", "w") as f:
        f.write(f"{digest}  {name}\n")
    print(f"{name}: {n} bytes sha256 {digest} (matches pin)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

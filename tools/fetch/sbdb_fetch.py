#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""prometheia-fetch — bulk-pull small-body orbital elements from the public
JPL SBDB Query API into resumable TSV shards ("sbdb-raw v1").

JPL SBDB data is US-government work (public domain). This script only
downloads and tabulates it; all EPM1 encoding happens in the C++ converter,
so the container format has exactly one implementation.

Server etiquette (deliberate policy — do not "optimize" away):
  - Requests are strictly SEQUENTIAL. One page in flight, ever. No threads,
    no connection pools, no pipelining.
  - --delay (default 2 s) sleeps between page requests.
  - The User-Agent identifies this tool so JPL's operators can see what is
    polling them and at what rate.
  - Retries back off exponentially (5, 10, 20, 40 s) and never speed up.

Usage:
  sbdb_fetch.py --out-dir sbdb-raw [--kinds a,c] [--page-size 50000]
                [--full-prec] [--fields F1,F2,...] [--delay SECONDS]

Resume: completed pages are recorded in manifest.json and skipped on re-run.
Provenance: provenance.txt (key=value) is written for the C++ converter, and
manifest.json carries the full per-page record for auditing.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

BASE_URL = "https://ssd-api.jpl.nasa.gov/sbdb_query.api"

DEFAULT_FIELDS = [
    "spkid", "pdes", "name", "class", "epoch",
    "e", "a", "i", "om", "w", "ma",
    "sigma_e", "sigma_a", "sigma_i", "sigma_om", "sigma_w", "sigma_ma",
    "H", "G", "diameter",
]


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


USER_AGENT = ("prometheia-fetch/0.1.0 "
              "(Ephemeris Prometheia catalog build; strictly sequential, "
              "rate-limited bulk pull)")


def http_get_json(url: str, tries: int = 4) -> dict:
    last_err = None
    for attempt in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=180) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except Exception as exc:  # noqa: BLE001 - retry any transport error
            last_err = exc
            wait = 2 ** attempt * 5
            print(f"  attempt {attempt + 1}/{tries} failed ({exc}); "
                  f"retrying in {wait}s", file=sys.stderr)
            time.sleep(wait)
    raise RuntimeError(f"GET failed after {tries} tries: {last_err}")


def fetch_page(kind: str, fields: list[str], limit_from: int, page_size: int,
               full_prec: bool) -> dict:
    params = [
        ("fields", ",".join(fields)),
        ("sb-kind", kind),
        ("limit", str(page_size)),
        ("limit-from", str(limit_from)),
    ]
    if full_prec:
        params.append(("full-prec", "1"))
    url = f"{BASE_URL}?{urllib.parse.urlencode(params)}"
    doc = http_get_json(url)
    if "code" in doc and doc.get("code") not in (200, None):
        raise RuntimeError(f"SBDB error: {doc.get('message', doc)}")
    return doc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out-dir", default="sbdb-raw")
    ap.add_argument("--kinds", default="a,c",
                    help="comma list of SBDB kinds: a=asteroid, c=comet")
    ap.add_argument("--page-size", type=int, default=50000)
    ap.add_argument("--max-pages", type=int, default=0,
                    help="stop after N pages per kind (0 = until exhausted)")
    ap.add_argument("--delay", type=float, default=2.0,
                    help="seconds to sleep between page requests "
                         "(server etiquette; never set to 0 for bulk pulls)")
    ap.add_argument("--full-prec", action="store_true", default=True)
    ap.add_argument("--fields", default=",".join(DEFAULT_FIELDS))
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    fields = [f.strip() for f in args.fields.split(",") if f.strip()]
    kinds = [k.strip() for k in args.kinds.split(",") if k.strip()]

    manifest_path = out_dir / "manifest.json"
    manifest = {
        "schema": "sbdb-raw v1",
        "base_url": BASE_URL,
        "fields": fields,
        "kinds": kinds,
        "page_size": args.page_size,
        "pages": [],
    }
    if manifest_path.exists():
        old = json.loads(manifest_path.read_text())
        if old.get("schema") == manifest["schema"] and old.get("fields") == fields \
                and old.get("page_size") == args.page_size:
            manifest = old
        else:
            print("manifest mismatch; starting fresh", file=sys.stderr)

    def done_from(kind: str) -> int:
        return sum(p["count"] for p in manifest["pages"]
                   if p["kind"] == kind and p["status"] == "ok")

    started = utc_now()
    for kind in kinds:
        limit_from = done_from(kind)
        pages_this_kind = 0
        while True:
            if args.max_pages and pages_this_kind >= args.max_pages:
                break
            doc = fetch_page(kind, fields, limit_from, args.page_size, args.full_prec)
            rows = doc.get("data", [])
            total = int(doc.get("count", 0))
            print(f"kind {kind}: rows {limit_from}..{limit_from + len(rows)} "
                  f"of {total}")
            if not rows and limit_from == 0:
                print(f"kind {kind}: no data", file=sys.stderr)
                break
            if rows:
                shard = out_dir / f"part-{kind}-{limit_from:09d}.tsv"
                with shard.open("w", encoding="utf-8", newline="") as fh:
                    fh.write("# fields: kind\t" + "\t".join(fields) + "\n")
                    for row in rows:
                        fh.write(kind + "\t" + "\t".join(
                            "" if v is None else str(v) for v in row) + "\n")
                manifest["pages"].append({
                    "kind": kind,
                    "limit_from": limit_from,
                    "count": len(rows),
                    "total_reported": total,
                    "file": shard.name,
                    "fetched_at_utc": utc_now(),
                    "status": "ok",
                })
                manifest_path.write_text(json.dumps(manifest, indent=1))
                limit_from += len(rows)
            pages_this_kind += 1
            if len(rows) < args.page_size or limit_from >= total:
                break
            if args.delay > 0:
                time.sleep(args.delay)  # be a polite guest on a public science API
        print(f"kind {kind}: complete, {limit_from} rows")

    finished = utc_now()

    with (out_dir / "provenance.txt").open("w", encoding="utf-8") as fh:
        fh.write(f"schema=sbdb-raw v1\n")
        fh.write("source=JPL SBDB (Small-Body DataBase) Query API\n")
        fh.write(f"url={BASE_URL}\n")
        fh.write(f"started_utc={started}\n")
        fh.write(f"finished_utc={finished}\n")
        fh.write(f"fields={','.join(fields)}\n")
        fh.write(f"kinds={','.join(kinds)}\n")
        fh.write(f"page_size={args.page_size}\n")
        fh.write(f"pages={len(manifest['pages'])}\n")
        fh.write("count=" + str(max(
            (p["total_reported"] for p in manifest["pages"]), default=0)) + "\n")
        fh.write("tool=prometheia-fetch 0.1.0\n")

    print(f"done: {len(manifest['pages'])} pages, output in {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""prometheia-fetch — bulk-pull small-body orbital elements from the public
JPL SBDB Query API into resumable TSV shards ("sbdb-raw v1").

JPL SBDB data is US-government work (public domain). This script only
downloads and tabulates it; all EPM1 encoding happens in the C++ converter,
so the container format has exactly one implementation.

Modes:
  full pull (default)  — paged full-precision sweep of all bodies. This is
                         the base-catalog producer.
  --slim               — identity sweep only: kind,spkid,orbit_id per body.
                         Cheap (~2 MB/page); the input to freshness
                         tracking. Shards: slim-<kind>-<from>.tsv,
                         manifest: manifest-slim.json.
  --delta-prev DIR     — run a slim sweep, diff it against a previous slim
                         dir, and re-fetch only the bodies whose orbit
                         solution changed (orbit_id moved), one object per
                         request via sbdb.api. Output is overlay shards in
                         --delta-out with the standard 21-column layout, so
                         prometheia-convert turns them into a stackable
                         overlay .epm with zero converter changes.
                         See docs/INGESTION.md, "Freshness".
  --sb-class CEN,TJN   — hot-subset sweeps by SBDB orbit class (composable
                         with --fields / full-prec for tier-1 polling).

Server etiquette (deliberate policy — do not "optimize" away):
  - Requests are strictly SEQUENTIAL. One request in flight, ever. No
    threads, no connection pools, no pipelining.
  - --delay (default 2 s) sleeps between requests, including per-object
    delta fetches.
  - The User-Agent identifies this tool so JPL's operators can see what is
    polling them and at what rate.
  - Retries back off exponentially (5, 10, 20, 40 s) and never speed up.
  - Development pulls stay tiny (--page-size 100 --max-pages 1); full
    pulls are deliberate, announced runs. Never a build side effect.

Resume: completed pages are recorded in manifest.json (or manifest-slim.json)
and skipped on re-run. Provenance: provenance.txt (key=value) for the C++
converter; manifest.json carries the full per-page audit record.
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
SINGLE_URL = "https://ssd-api.jpl.nasa.gov/sbdb.api"

DEFAULT_FIELDS = [
    "spkid", "pdes", "name", "class", "epoch",
    "e", "a", "i", "om", "w", "ma",
    "sigma_e", "sigma_a", "sigma_i", "sigma_om", "sigma_w", "sigma_ma",
    "H", "G", "diameter",
]

SLIM_FIELDS = ["spkid", "orbit_id"]

# Delta rows reuse the sbdb-raw v1 21-column layout exactly, so the C++
# converter consumes overlay shards with zero changes.
DELTA_ROW_FIELDS = DEFAULT_FIELDS

USER_AGENT = ("prometheia-fetch/0.1.0 "
              "(Ephemeris Prometheia catalog build; strictly sequential, "
              "rate-limited bulk pull)")


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


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
               full_prec: bool, sb_class: str = "") -> dict:
    params = [
        ("fields", ",".join(fields)),
        ("sb-kind", kind),
        ("limit", str(page_size)),
        ("limit-from", str(limit_from)),
    ]
    if sb_class:
        params.append(("sb-class", sb_class))
    if full_prec:
        params.append(("full-prec", "1"))
    url = f"{BASE_URL}?{urllib.parse.urlencode(params)}"
    doc = http_get_json(url)
    if "code" in doc and doc.get("code") not in (200, None):
        raise RuntimeError(f"SBDB error: {doc.get('message', doc)}")
    return doc


def fetch_single(spkid: int) -> dict:
    """One body, full precision, via the single-object endpoint."""
    params = [("sstr", str(spkid)), ("full-prec", "1"), ("phys-par", "True")]
    doc = http_get_json(f"{SINGLE_URL}?{urllib.parse.urlencode(params)}")
    if "code" in doc and doc.get("code") not in (200, None):
        raise RuntimeError(f"SBDB error for {spkid}: {doc.get('message', doc)}")
    return doc


def sbdb_single_to_row(doc: dict) -> list[str]:
    """Flatten a sbdb.api response into one sbdb-raw v1 TSV row (21 cols,
    kind first). Missing values become empty cells, matching query-API
    null handling."""
    obj = doc.get("object", {})
    orb = doc.get("orbit", {})

    def clean(s) -> str:
        return str(s).strip().strip('"') if s is not None else ""

    elements = {}
    for el in orb.get("elements") or []:
        elements[el.get("name", "")] = el

    def elem(name: str, which: str) -> str:
        el = elements.get(name)
        if not el:
            return ""
        v = el.get(which)
        return "" if v is None else str(v)

    phys = {}
    for p in doc.get("phys_par") or []:
        phys[p.get("name", "").strip()] = p

    def phys_val(name: str) -> str:
        p = phys.get(name)
        if not p:
            return ""
        v = p.get("value")
        return "" if v is None else str(v)

    orbit_class = obj.get("orbit_class")
    if isinstance(orbit_class, dict):
        orbit_class = orbit_class.get("code", "")
    row = [
        clean(obj.get("kind")),
        clean(obj.get("spkid")),
        clean(obj.get("des")),
        clean(obj.get("shortname")),
        clean(orbit_class),
        clean(orb.get("epoch")),
    ]
    for name in ("e", "a", "i", "om", "w", "ma"):
        row.append(elem(name, "value"))
    for name in ("e", "a", "i", "om", "w", "ma"):
        row.append(elem(name, "sigma"))
    row += [phys_val("H"), phys_val("G"), phys_val("diameter")]
    return row


def load_slim_map(slim_dir: Path) -> dict[int, str]:
    """spkid -> orbit_id from a slim dir's shards."""
    slim = {}
    for shard in sorted(slim_dir.glob("slim-*.tsv")):
        lines = shard.read_text(encoding="utf-8").splitlines()
        if len(lines) < 2:
            continue
        for line in lines[1:]:
            if not line:
                continue
            cols = line.split("\t")
            if len(cols) >= 3 and cols[1].isdigit():
                slim[int(cols[1])] = cols[2]
    return slim


def write_provenance(out_dir: Path, kv: dict[str, str]) -> None:
    with (out_dir / "provenance.txt").open("w", encoding="utf-8") as fh:
        for k, v in kv.items():
            fh.write(f"{k}={v}\n")


def sweep(args, fields: list[str], manifest_name: str, shard_prefix: str,
          provenance: bool) -> None:
    """Paged sweep of the query API into resumable shards."""
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    kinds = [k.strip() for k in args.kinds.split(",") if k.strip()]

    manifest_path = out_dir / manifest_name
    manifest = {
        "schema": "sbdb-raw v1",
        "base_url": BASE_URL,
        "fields": fields,
        "kinds": kinds,
        "page_size": args.page_size,
        "sb_class": args.sb_class,
        "pages": [],
    }
    if manifest_path.exists():
        old = json.loads(manifest_path.read_text())
        if (old.get("schema") == manifest["schema"]
                and old.get("fields") == fields
                and old.get("page_size") == args.page_size
                and old.get("sb_class") == args.sb_class):
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
            doc = fetch_page(kind, fields, limit_from, args.page_size,
                             args.full_prec, args.sb_class)
            rows = doc.get("data", [])
            total = int(doc.get("count", 0))
            print(f"kind {kind}: rows {limit_from}..{limit_from + len(rows)} "
                  f"of {total}")
            if not rows and limit_from == 0:
                print(f"kind {kind}: no data", file=sys.stderr)
                break
            if rows:
                shard = out_dir / f"{shard_prefix}{kind}-{limit_from:09d}.tsv"
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

    if provenance:
        write_provenance(out_dir, {
            "schema": "sbdb-raw v1",
            "source": "JPL SBDB (Small-Body DataBase) Query API",
            "url": BASE_URL,
            "started_utc": started,
            "finished_utc": utc_now(),
            "fields": ",".join(fields),
            "kinds": ",".join(kinds),
            "page_size": str(args.page_size),
            "sb_class": args.sb_class,
            "pages": str(len(manifest["pages"])),
            "count": str(max((p["total_reported"] for p in manifest["pages"]),
                             default=0)),
            "tool": "prometheia-fetch 0.1.0",
        })


def delta(args) -> int:
    """Slim sweep + diff + per-object refetch of changed orbit solutions."""
    out_dir = Path(args.out_dir)
    delta_out = Path(args.delta_out)
    delta_out.mkdir(parents=True, exist_ok=True)

    if not args.delta_prev or not Path(args.delta_prev).exists():
        print("--delta-prev must point at a previous slim dir", file=sys.stderr)
        return 2

    # 1. Fresh slim sweep into out-dir (resume-aware).
    sweep(args, SLIM_FIELDS, "manifest-slim.json", "slim-", provenance=False)
    current = load_slim_map(out_dir)
    previous = load_slim_map(Path(args.delta_prev))

    changed = sorted(int(k) for k, oid in current.items()
                     if k not in previous or previous[k] != oid)
    vanished = sorted(int(k) for k in previous if k not in current)
    print(f"delta: {len(current)} bodies now, {len(previous)} before; "
          f"{len(changed)} changed, {len(vanished)} vanished")

    if len(changed) > args.max_delta:
        print(f"changed set ({len(changed)}) exceeds --max-delta "
              f"({args.max_delta}); do a full pull instead. Changed spkids "
              f"written to {delta_out / 'changed-spkids.txt'}", file=sys.stderr)
        (delta_out / "changed-spkids.txt").write_text(
            "".join(f"{k}\n" for k in changed))
        return 2
    (delta_out / "changed-spkids.txt").write_text(
        "".join(f"{k}\n" for k in changed))

    # 2. Refetch changed bodies, one request each, sequential, delayed.
    delta_manifest_path = delta_out / "manifest-delta.json"
    delta_manifest = {
        "schema": "prometheia-delta v1",
        "prev_slim": str(args.delta_prev),
        "fetched": {},  # spkid -> fetched_at_utc
    }
    if delta_manifest_path.exists():
        delta_manifest = json.loads(delta_manifest_path.read_text())

    shard = delta_out / "delta-changed.tsv"
    if not shard.exists():
        with shard.open("w", encoding="utf-8", newline="") as fh:
            fh.write("# fields: kind\t" + "\t".join(DELTA_ROW_FIELDS) + "\n")

    for spkid in changed:
        if str(spkid) in delta_manifest["fetched"]:
            continue
        doc = fetch_single(spkid)
        row = sbdb_single_to_row(doc)
        with shard.open("a", encoding="utf-8", newline="") as fh:
            fh.write("\t".join(row) + "\n")
        delta_manifest["fetched"][str(spkid)] = utc_now()
        delta_manifest_path.write_text(json.dumps(delta_manifest, indent=1))
        print(f"  refetched spkid {spkid} "
              f"(orbit_id {doc.get('orbit', {}).get('orbit_id')})")
        if args.delay > 0:
            time.sleep(args.delay)

    write_provenance(delta_out, {
        "schema": "prometheia-delta v1",
        "source": "JPL SBDB single-object API (sbdb.api), delta refetch",
        "url": SINGLE_URL,
        "started_utc": utc_now(),
        "finished_utc": utc_now(),
        "fields": ",".join(DELTA_ROW_FIELDS),
        "kinds": "a,c",
        "page_size": "1",
        "pages": str(len(delta_manifest["fetched"])),
        "count": str(len(changed)),
        "prev_slim": str(args.delta_prev),
        "vanished": ",".join(str(k) for k in vanished[:100]),
        "tool": "prometheia-fetch 0.1.0",
    })
    print(f"delta complete: {len(delta_manifest['fetched'])} bodies -> {shard}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out-dir", default="sbdb-raw")
    ap.add_argument("--kinds", default="a,c",
                    help="comma list of SBDB kinds: a=asteroid, c=comet")
    ap.add_argument("--page-size", type=int, default=50000)
    ap.add_argument("--max-pages", type=int, default=0,
                    help="stop after N pages per kind (0 = until exhausted)")
    ap.add_argument("--delay", type=float, default=2.0,
                    help="seconds to sleep between requests "
                         "(server etiquette; never set to 0 for bulk pulls)")
    ap.add_argument("--full-prec", action="store_true", default=True)
    ap.add_argument("--fields", default=",".join(DEFAULT_FIELDS))
    ap.add_argument("--sb-class", default="",
                    help="SBDB orbit-class filter, e.g. CEN,TJN (hot subsets)")
    ap.add_argument("--slim", action="store_true",
                    help="identity sweep only: kind,spkid,orbit_id")
    ap.add_argument("--delta-prev", default="",
                    help="previous slim dir; enables delta mode")
    ap.add_argument("--delta-out", default="",
                    help="overlay output dir for delta mode "
                         "(default: <out-dir>-overlay)")
    ap.add_argument("--max-delta", type=int, default=1000,
                    help="refetch at most this many changed bodies per delta "
                         "run; beyond it, insist on a full pull (default 1000)")
    args = ap.parse_args()

    if args.delta_prev:
        if not args.delta_out:
            args.delta_out = args.out_dir.rstrip("/") + "-overlay"
        return delta(args)

    if args.slim:
        sweep(args, SLIM_FIELDS, "manifest-slim.json", "slim-", provenance=False)
        return 0

    sweep(args, [f.strip() for f in args.fields.split(",") if f.strip()],
          "manifest.json", "part-", provenance=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

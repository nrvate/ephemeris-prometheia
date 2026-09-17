#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Refresh the Earth-orientation tables compiled into libprometheia.

Generates, from USNO's public-domain series (US Government work):

  src/delta_t_table.inc     observed Delta T = TT - UT1: half-yearly
                            1657-1972 (historic_deltat.data), monthly from
                            1973 (deltat.data)
  src/leap_second_table.inc TAI - UTC integer leap seconds from 1972
                            (tai-utc.dat)

This is a release-time ingestion step (docs/INGESTION.md): run it before
tagging, rebuild, run the tests, and commit the regenerated tables.

Network etiquette (docs/DESIGN.md): strictly sequential requests, a pause
between them, an identifying User-Agent, and backoff-only retries.

Usage:
  gen_earth_orientation.py --fetch --raw-dir eop-raw        # download + generate
  gen_earth_orientation.py --raw-dir eop-raw                # regenerate from cache
  gen_earth_orientation.py --raw-dir eop-raw --check        # exit 1 if src/ is stale
"""
import argparse
import datetime
import hashlib
import os
import re
import sys
import time
import urllib.request

BASE_URL = "https://maia.usno.navy.mil/ser7/"
SOURCES = ["historic_deltat.data", "deltat.data", "tai-utc.dat"]
USER_AGENT = ("prometheia-gen/0.1.0 "
              "(Ephemeris Prometheia release data refresh; sequential, 3 small files)")
REQUEST_PAUSE_S = 3.0

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DELTA_T_OUT = os.path.join(REPO, "src", "delta_t_table.inc")
LEAP_OUT = os.path.join(REPO, "src", "leap_second_table.inc")

MONTHS = ["JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"]


def fetch(raw_dir):
    os.makedirs(raw_dir, exist_ok=True)
    for i, name in enumerate(SOURCES):
        if i:
            time.sleep(REQUEST_PAUSE_S)
        url = BASE_URL + name
        for attempt in range(4):
            try:
                req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
                with urllib.request.urlopen(req, timeout=60) as resp:
                    body = resp.read()
                break
            except Exception as exc:  # noqa: BLE001 - retry any transport error
                if attempt == 3:
                    sys.exit(f"GET {url} failed: {exc}")
                wait = 5 * 2 ** attempt
                print(f"  {url}: {exc}; retrying in {wait}s", file=sys.stderr)
                time.sleep(wait)
        with open(os.path.join(raw_dir, name), "wb") as f:
            f.write(body)
        print(f"  fetched {name} ({len(body)} bytes)", file=sys.stderr)


def jd_from_civil(y, m, d):
    """JD at 00:00 of a proleptic Gregorian date."""
    return datetime.date(y, m, d).toordinal() + 1721424.5


def jd_from_decimal_year(yf):
    y = int(yf)
    start = jd_from_civil(y, 1, 1)
    length = jd_from_civil(y + 1, 1, 1) - start
    return start + (yf - y) * length


def parse_historic(text):
    rows = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 2 and re.fullmatch(r"\d{4}\.\d+", parts[0]):
            rows.append((jd_from_decimal_year(float(parts[0])), float(parts[1]), parts[0]))
    return rows


def parse_monthly(text):
    rows = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[0].isdigit():
            y, m, d = int(parts[0]), int(parts[1]), int(parts[2])
            rows.append((jd_from_civil(y, m, d), float(parts[3]), f"{y:04d}-{m:02d}-{d:02d}"))
    return rows


def parse_leaps(text):
    rows = []
    for line in text.splitlines():
        m = re.match(r"\s*(\d{4})\s+([A-Z]{3})\s+1\s+=JD\s+[\d.]+\s+TAI-UTC=\s*([\d.]+)\s+S"
                     r"\s+\+\s+\(MJD\s+-\s+[\d.]+\)\s+X\s+([\d.]+)", line)
        if not m:
            continue
        year, month = int(m.group(1)), MONTHS.index(m.group(2)) + 1
        offset, drift = float(m.group(3)), float(m.group(4))
        if year < 1972:
            continue  # the 1961-1971 drift era is not supported by the library
        if drift != 0.0 or offset != int(offset):
            sys.exit(f"tai-utc.dat: non-integer post-1972 entry: {line.strip()}")
        rows.append((year, month, int(offset)))
    return rows


def fail(msg):
    sys.exit("validation failed: " + msg)


def validate(historic, monthly, leaps):
    if len(historic) < 600 or len(monthly) < 600:
        fail("series unexpectedly short")
    for series, name in ((historic, "historic"), (monthly, "monthly")):
        for a, b in zip(series, series[1:]):
            if not b[0] > a[0]:
                fail(f"{name} dates not ascending at {b[2]}")
            if abs(b[1] - a[1]) > 2.0:
                fail(f"{name} jump of {b[1] - a[1]:.3f} s at {b[2]}")
    # Where the series overlap (1973-1984) they must agree.
    for jd, dt, label in historic:
        if monthly[0][0] <= jd <= monthly[-1][0]:
            k = max(i for i, r in enumerate(monthly) if r[0] <= jd)
            if k + 1 < len(monthly):
                (j0, v0, _), (j1, v1, _) = monthly[k], monthly[k + 1]
                v = v0 + (v1 - v0) * (jd - j0) / (j1 - j0)
            else:
                v = monthly[k][1]
            if abs(v - dt) > 0.05:
                fail(f"historic {label} = {dt} vs monthly {v:.4f}")
    if not leaps or leaps[0] != (1972, 1, 10):
        fail("leap table must start 1972-01-01 at 10 s")
    for a, b in zip(leaps, leaps[1:]):
        if b[2] != a[2] + 1 or (b[0], b[1]) <= (a[0], a[1]):
            fail(f"leap table not a +1 s ascending sequence at {b}")


def header(raw, names, what):
    lines = [
        "// Generated by tools/gen/gen_earth_orientation.py -- do not edit.",
        f"// {what}",
        "// Source: US Naval Observatory, public domain (US Government work):",
    ]
    for name in names:
        digest = hashlib.sha256(raw[name]).hexdigest()
        lines.append(f"//   {BASE_URL}{name}")
        lines.append(f"//     sha256 {digest}")
    lines.append(f"// Retrieved: {datetime.date.today().isoformat()}")
    return lines


def render_delta_t(raw, historic, monthly):
    first_monthly = monthly[0][0]
    rows = [r for r in historic if r[0] < first_monthly] + monthly
    out = header(raw, SOURCES[:2], "Observed Delta T = TT - UT1 (seconds) at JD (UTC date, 0h).")
    out.append(f"// {len(rows)} samples, {rows[0][2]} .. {rows[-1][2]}.")
    out.append("constexpr DeltaTSample kDeltaTTable[] = {")
    for jd, dt, label in rows:
        out.append(f"    {{{jd:.4f}, {dt!r}}}, // {label}")
    out.append("};")
    return "\n".join(out) + "\n"


def render_leaps(raw, leaps):
    out = header(raw, SOURCES[2:], "TAI - UTC integer leap seconds, effective 00:00 UTC.")
    out.append("constexpr LeapEntry kLeapTable[] = {")
    for y, m, off in leaps:
        out.append(f"    {{{y}, {m}, {off}}},")
    out.append("};")
    return "\n".join(out) + "\n"


def data_lines(text):
    """Content that matters for staleness: everything but the retrieval date."""
    return [l for l in text.splitlines() if not l.startswith("// Retrieved:")]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--raw-dir", default="eop-raw", help="download cache (gitignored)")
    ap.add_argument("--fetch", action="store_true", help="download fresh sources first")
    ap.add_argument("--check", action="store_true",
                    help="do not write; exit 1 if the committed tables differ")
    args = ap.parse_args()

    if args.fetch:
        fetch(args.raw_dir)
    raw = {}
    for name in SOURCES:
        path = os.path.join(args.raw_dir, name)
        if not os.path.exists(path):
            sys.exit(f"{path} missing (run with --fetch)")
        with open(path, "rb") as f:
            raw[name] = f.read()

    historic = parse_historic(raw["historic_deltat.data"].decode("ascii", "replace"))
    monthly = parse_monthly(raw["deltat.data"].decode("ascii", "replace"))
    leaps = parse_leaps(raw["tai-utc.dat"].decode("ascii", "replace"))
    validate(historic, monthly, leaps)

    outputs = {DELTA_T_OUT: render_delta_t(raw, historic, monthly),
               LEAP_OUT: render_leaps(raw, leaps)}
    stale = False
    for path, text in outputs.items():
        current = open(path).read() if os.path.exists(path) else ""
        changed = data_lines(current) != data_lines(text)
        rel = os.path.relpath(path, REPO)
        if args.check:
            print(f"{rel}: {'STALE' if changed else 'up to date'}")
            stale |= changed
        elif changed:
            with open(path, "w") as f:
                f.write(text)
            print(f"{rel}: updated")
        else:
            print(f"{rel}: unchanged")
    print(f"Delta T observed through {monthly[-1][2]}; "
          f"latest leap second {leaps[-1][0]}-{leaps[-1][1]:02d} ({leaps[-1][2]} s)")
    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main())

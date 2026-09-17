#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetch the M5 JPL Horizons verification corpus (raw responses).

Every request the corpus needs is declared below. The tool runs them strictly
sequentially, with a pause between requests, an identifying User-Agent,
and backoff-only retries (docs/DESIGN.md, server etiquette). Responses are
cached verbatim under --raw-dir (gitignored), one JSON file per request,
together with a manifest of URL, retrieval time and SHA-256. Re-running
skips requests already cached, so an interrupted run resumes.
tools/gen/gen_horizons_corpus.py turns the cache into tests/horizons_corpus.inc.

Horizons output is a US-government work (JPL/Caltech for NASA).

Usage:
  horizons_fetch.py --list                      # print every request, fetch nothing
  horizons_fetch.py --raw-dir horizons-raw --only geo-sun   # one request
  horizons_fetch.py --raw-dir horizons-raw      # the whole corpus
"""
import argparse
import datetime
import hashlib
import json
import os
import sys
import time
import urllib.parse
import urllib.request

API = "https://ssd.jpl.nasa.gov/api/horizons.api"
USER_AGENT = ("prometheia-fetch/0.1.0 "
              "(Ephemeris Prometheia M5 verification corpus; sequential, ~40 requests)")
PAUSE_S = 5.0

# --- Epochs (JD) --------------------------------------------------------------

# Planets: span the DE440 era; 1975-2026 lie inside the EOP-corrected era of
# Horizons' of-date frames, the others test the precession/nutation models.
PLANET_EPOCHS_TT = [
    2378496.5,  # 1800-01-01
    2415020.5,  # 1900-01-01
    2442413.5,  # 1975-01-01
    2451545.0,  # 2000-01-01 12h
    2459001.5,  # 2020-06-01
    2461299.5,  # 2026-09-16
    2469807.5,  # 2050-01-01
    2488069.5,  # 2100-01-01
]
# Topocentric: observed-Delta-T era only (so both sides use measured Earth
# rotation), at assorted hours to sample the diurnal geometry.
TOPO_EPOCHS_TT = [
    2444664.6,   # 1981-03-01 02:24
    2449900.85,  # 1995-07-02 08:24
    2455197.35,  # 2009-12-31 20:24
    2459001.1,   # 2020-05-31 14:24
    2461100.95,  # 2026-03-01 10:48
]
# Small bodies: the sample catalog's element epoch (2461200.5, 2026-06-08 TDB)
# and 10, 25, 50 and 100 Julian years either side.
SMALL_EPOCH = 2461200.5
SMALL_OFFSETS_YR = [-100, -50, -25, -10, 0, 10, 25, 50, 100]
SMALL_EPOCHS_TT = [SMALL_EPOCH + y * 365.25 for y in SMALL_OFFSETS_YR]

# --- Targets -------------------------------------------------------------------

PLANETS = [("sun", "10"), ("moon", "301"), ("mercury", "199"), ("venus", "299"),
           ("mars", "4"), ("jupiter", "5"), ("saturn", "6"), ("uranus", "7"),
           ("neptune", "8"), ("pluto", "9")]
FROM_SUN = [("mercury", "199"), ("venus", "299"), ("earth", "399"), ("mars", "4"),
            ("jupiter", "5"), ("saturn", "6"), ("uranus", "7"), ("neptune", "8"),
            ("pluto", "9")]
# Numbered asteroids from tests/data/sample-100.epm: the three largest, a
# high-inclination one, an inner-belt S type and an outer-belt body.
SMALL_BODIES = [("ceres", "1"), ("pallas", "2"), ("vesta", "4"), ("iris", "7"),
                ("hygiea", "10"), ("cybele", "65"),
                # A scattered-disk TNO (a = 92 AU): uncertainty, not dynamics,
                # is its limit. Not in the sample catalog; its elements and
                # covariance come from tests/data/covariance-7.epm.
                ("rumina", "145451")]
# Topocentric sites: (name, east lon deg, lat deg, height km)
SITES = {
    "zurich": (8.55, 47.37, 0.5),
    "longyearbyen": (15.63, 78.22, 0.02),  # high latitude: pole-orientation test
    "paranal": (-70.40, -24.63, 2.635),    # southern hemisphere, high altitude
}


def tlist(jds):
    return " ".join(f"'{jd:.6f}'" for jd in jds)


def observer(command, center, jds, quantities, extra=None):
    p = {
        "format": "json",
        "COMMAND": f"'{command}'",
        "OBJ_DATA": "'NO'",
        "MAKE_EPHEM": "'YES'",
        "EPHEM_TYPE": "'OBSERVER'",
        "CENTER": f"'{center}'",
        "TLIST": tlist(jds),
        "TLIST_TYPE": "'JD'",
        "TIME_TYPE": "'TT'",
        "QUANTITIES": f"'{quantities}'",
        "REF_SYSTEM": "'ICRF'",
        "CAL_FORMAT": "'JD'",
        "ANG_FORMAT": "'DEG'",
        "APPARENT": "'AIRLESS'",
        "RANGE_UNITS": "'AU'",
        "EXTRA_PREC": "'YES'",
        "CSV_FORMAT": "'YES'",
    }
    p.update(extra or {})
    return p


def vectors(command, center, jds_tdb):
    return {
        "format": "json",
        "COMMAND": f"'{command}'",
        "OBJ_DATA": "'NO'",
        "MAKE_EPHEM": "'YES'",
        "EPHEM_TYPE": "'VECTORS'",
        "CENTER": f"'{center}'",
        "TLIST": tlist(jds_tdb),
        "TLIST_TYPE": "'JD'",
        "REF_PLANE": "'FRAME'",
        "REF_SYSTEM": "'ICRF'",
        "VEC_TABLE": "'2'",
        "VEC_CORR": "'NONE'",
        "OUT_UNITS": "'AU-D'",
        "CSV_FORMAT": "'YES'",
    }


def requests():
    """(name, purpose, params) for every request of the corpus, in fetch order."""
    out = []
    # 1 astrometric RA/Dec (ICRF), 2 apparent RA/Dec (true equator of date),
    # 20 range and range rate, 30 TDB-UT, 31 apparent ecliptic lon/lat of date.
    # Topocentric adds 7, local apparent sidereal time: Horizons' TDB-UT is
    # TDB-UTC after 1962, so UT1 is recovered from the sidereal time instead.
    for name, cmd in PLANETS:
        out.append((f"geo-{name}", "geocentric astrometric/apparent, ecliptic of date",
                    observer(cmd, "500@399", PLANET_EPOCHS_TT, "1,2,20,30,31")))
    for name, cmd in FROM_SUN:
        out.append((f"helio-{name}", "heliocentric astrometric/apparent",
                    observer(cmd, "500@10", PLANET_EPOCHS_TT, "1,2,20,31")))
    for name, cmd in [("sun", "10"), ("moon", "301"), ("venus", "299"), ("mars", "4")]:
        lon, lat, h = SITES["zurich"]
        out.append((f"topo-zurich-{name}", "topocentric apparent",
                    observer(cmd, "coord@399", TOPO_EPOCHS_TT, "1,2,7,20,30,31",
                             {"COORD_TYPE": "'GEODETIC'",
                              "SITE_COORD": f"'{lon},{lat},{h}'"})))
    for site in ("longyearbyen", "paranal"):
        lon, lat, h = SITES[site]
        out.append((f"topo-{site}-moon", "topocentric apparent Moon",
                    observer("301", "coord@399", TOPO_EPOCHS_TT, "1,2,7,20,30,31",
                             {"COORD_TYPE": "'GEODETIC'",
                              "SITE_COORD": f"'{lon},{lat},{h}'"})))
    # Small bodies: 36 RA/Dec 3-sigma uncertainty, 38 plane-of-sky RSS
    # uncertainty (Horizons' own covariance propagation, for comparison
    # with sigma_arcsec); heliocentric geometric vectors isolate the
    # integration from the observation pipeline.
    for name, number in SMALL_BODIES:
        out.append((f"sb-geo-{name}", "small body geocentric astrometric/apparent + sigmas",
                    observer(f"{number};", "500@399", SMALL_EPOCHS_TT, "1,2,20,31,36,38")))
        out.append((f"sb-helio-{name}", "small body heliocentric geometric vectors (TDB)",
                    vectors(f"{number};", "500@10", SMALL_EPOCHS_TT)))
    return out


def url_of(params):
    return API + "?" + urllib.parse.urlencode(params, quote_via=urllib.parse.quote)


def fetch_one(url):
    for attempt in range(4):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=120) as resp:
                return resp.read()
        except Exception as exc:  # noqa: BLE001 - backoff on any transport error
            if attempt == 3:
                raise
            wait = 10 * 2 ** attempt
            print(f"    {exc}; retrying in {wait}s", file=sys.stderr)
            time.sleep(wait)
    raise AssertionError("unreachable")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--raw-dir", default="horizons-raw", help="response cache (gitignored)")
    ap.add_argument("--list", action="store_true", help="print the requests and exit")
    ap.add_argument("--only", action="append", default=[], help="fetch only these names")
    args = ap.parse_args()

    reqs = requests()
    if args.only:
        known = {n for n, _, _ in reqs}
        for n in args.only:
            if n not in known:
                sys.exit(f"unknown request name: {n}")
        reqs = [r for r in reqs if r[0] in args.only]

    if args.list:
        for name, purpose, params in reqs:
            print(f"{name:26s} {purpose}")
            print(f"    {url_of(params)}")
        print(f"{len(reqs)} requests; ~{len(reqs) * PAUSE_S / 60:.1f} min of pauses")
        return 0

    os.makedirs(args.raw_dir, exist_ok=True)
    manifest_path = os.path.join(args.raw_dir, "manifest.json")
    manifest = {}
    if os.path.exists(manifest_path):
        with open(manifest_path) as f:
            manifest = json.load(f)

    fetched = 0
    for name, purpose, params in reqs:
        path = os.path.join(args.raw_dir, name + ".json")
        if os.path.exists(path) and name in manifest:
            print(f"  cached  {name}", file=sys.stderr)
            continue
        if fetched:
            time.sleep(PAUSE_S)
        url = url_of(params)
        body = fetch_one(url)
        fetched += 1
        try:
            doc = json.loads(body)
        except ValueError:
            sys.exit(f"{name}: response is not JSON: {body[:200]!r}")
        if "error" in doc:
            sys.exit(f"{name}: Horizons error: {doc['error']}")
        with open(path, "wb") as f:
            f.write(body)
        manifest[name] = {
            "purpose": purpose,
            "url": url,
            "retrieved_utc": datetime.datetime.now(datetime.timezone.utc)
                             .strftime("%Y-%m-%dT%H:%M:%SZ"),
            "sha256": hashlib.sha256(body).hexdigest(),
            "api_version": doc.get("signature", {}).get("version"),
        }
        with open(manifest_path, "w") as f:
            json.dump(manifest, f, indent=1, sort_keys=True)
        print(f"  fetched {name} ({len(body)} bytes)", file=sys.stderr)
    print(f"{fetched} fetched, {len(reqs) - fetched} cached", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

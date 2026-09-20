#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A stand-in for prometheia-wire-client that answers from a script.

corrapplied.py reaches a server only through the reference client and reads
only what wirelib.py parses from its stdout, so a client is the narrowest
place to inject a fault: no daemon, no ephemeris, no socket, and a server
that can be made to lie in one specific way at a time.  That is what
corrtest.py drives.

This is deliberately not a v4 server.  It never parses a frame and knows
nothing about the wire; it reproduces the client's *output*, which is the
only surface the checker has.  A change to that format therefore has to be
made here too -- and wirelib.py's regexes will stop matching if it is not,
which fails corrtest.py rather than quietly reducing it to nothing.

The scenario is a JSON file named by $FAKEWIRE_SCENARIO:

    corrmasks      [[observer bitmask, exact mask], ...]  -- A.3 0x0004
    corrkinds      [[observers, kinds, mask], ...]        -- A.3 0x0014
    unavailable    correction bits the engine cannot actually apply here,
                   so an honest corrApplied has them clear and asking for
                   them moves nothing
    mode           honest | varies | overclaim | false-denial
    claim_extra    bits added to corrApplied under `overclaim`
    answer         observers this server answers at all; any other draws
                   errCode 2, which corrapplied.py counts as skipped

Under `mode: "rates"` it instead answers a series -- `--jd`, `--step`,
`--count` -- from a smooth analytic ephemeris whose RATE columns are the
exact derivative, plus whatever error the scenario asks for.  That is what
ratestest.py drives, and the extra fields are:

    ratesbound     [deg/day, AU/day] for A.3 0x0013, or absent for no such
                   record at all, which is not zero: the registry default
                   then applies and ratesweep.py has to know the difference
    ratesbound_alt   a second bound, sent instead for objects matching
    ratesbound_alt_when   ... this substring (one server, two answers)
    rate_error_deg   added to the reported longitude rate
    rate_error_au    added to the reported distance rate
    rate_error_deg_alt  added on top of those, and
    rate_error_au_alt   ... only when
    rate_error_alt_under ... only when --deltat is BELOW this value. This is
                     the shape the Astrolog server showed on 2026-09-20: a
                     cell inside the advertised bound at one delta T and far
                     outside it at another, which a grid that fixes delta T
                     cannot see. Mirrored deliberately -- theirs was dirty
                     ABOVE a threshold, and ratesweep.py fixes delta T at
                     69.2, so a case that must be clean at the grid's value
                     and dirty at a widened one has to inject below it
    answer_objects   name substrings this server answers; anything else
                     draws errCode 2
    no_welcome     omit the WELCOME line entirely, which is what a drift in
                   wirelib's regex would look like from here
"""

import json
import math
import os
import sys

LIGHT_TIME, DEFLECTION, ABERRATION = 1, 2, 4
OBS_BIT = {"geo": 0, "topo": 1, "helio": 2, "bary": 3, "body": 4}
# Each correction shifts the answer by its own amount, all far above
# corrapplied.py's 1e-3" floor, so "moved" and "did not move" are decided by
# the scenario and never by arithmetic noise.
SHIFT_ARCSEC = {LIGHT_TIME: 20.0, DEFLECTION: 5.0, ABERRATION: 12.0}


def parse_argv(argv):
    """The subset of the client's command line the checkers ever pass."""
    out = {"observer": "geo", "kind": 0, "name": "?", "mask": 0,
           "jd": 2451545.0, "step": 0.0, "count": 1, "deltat": 0.0}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--corrections":
            out["mask"] = int(argv[i + 1])
            i += 2
        elif a == "--jd":
            out["jd"] = float(argv[i + 1])
            i += 2
        elif a == "--step":           # seconds on the wire, days here
            out["step"] = float(argv[i + 1]) / 86400.0
            i += 2
        elif a == "--count":
            out["count"] = int(argv[i + 1])
            i += 2
        elif a == "--deltat":
            out["deltat"] = float(argv[i + 1])
            i += 2
        elif a == "--obj":
            out["name"], out["kind"] = "body " + argv[i + 1], 0
            i += 2
        elif a == "--node":
            out["name"], out["kind"] = "node " + argv[i + 1], 1
            i += 2
        elif a == "--star":
            out["name"], out["kind"] = argv[i + 1], 2
            i += 2
        elif a == "--topo":
            out["observer"] = "topo"
            i += 2
        elif a == "--helio":
            out["observer"] = "helio"
            i += 1
        elif a == "--bary":
            out["observer"] = "bary"
            i += 1
        elif a == "--center":
            out["observer"] = "body"
            i += 2
        else:
            i += 1
    return out


def declared_for(scn, observer_bit, kind):
    """The same union corrapplied.py computes, from the scenario's WELCOME."""
    allowed = 0
    for observers, mask in scn.get("corrmasks", []):
        if observers & (1 << observer_bit):
            allowed |= mask
    for observers, kinds, mask in scn.get("corrkinds", []):
        if observers & (1 << observer_bit) and kinds & (1 << kind):
            allowed |= mask
    return allowed


# A smooth analytic ephemeris, so the RATE columns can be the *exact*
# derivative and any difference ratesweep.py measures is the scenario's doing
# and not the model's. The period is long next to the 1/1024-day step, which
# leaves the five-point formula's truncation error around 1e-14 deg/day --
# nine orders under the tightest bound in play, so "within the bound" is
# never a statement about this arithmetic.
PERIOD_DAYS = 27.3
W_LON = 0.9856  # deg/day of secular drift
A_LON, A_LAT, A_DIST = 5.0, 2.0, 0.2


def ephemeris(name, t):
    """(lon, lat, dist, dlon, dlat, ddist) -- position and its true rate."""
    w = 2.0 * math.pi / PERIOD_DAYS
    ph = w * (t - 2451545.0) + (sum(ord(c) for c in name) % 360) * math.pi / 180.0
    base = sum(ord(c) for c in name) % 360
    lon = (base + W_LON * (t - 2451545.0) + A_LON * math.sin(ph)) % 360.0
    lat = A_LAT * math.sin(ph + 1.0)
    dist = 1.5 + A_DIST * math.sin(ph + 2.0)
    return (lon, lat, dist,
            W_LON + A_LON * math.cos(ph) * w,
            A_LAT * math.cos(ph + 1.0) * w,
            A_DIST * math.cos(ph + 2.0) * w)


def emit_series(scn, ask):
    """`--count` rows at `--step`, with the rate columns the scenario asks for."""
    answers = scn.get("answer_objects")
    if answers is not None and not any(s in ask["name"] for s in answers):
        print(f'# object 0 name "{ask["name"]}" rowsOk 0 corr 0 err 2 '
              f'"not supported by this server for this object"')
        return 0

    print(f'# object 0 name "{ask["name"]}" rowsOk {ask["count"]} corr 7 err 0 ""')
    for i in range(ask["count"]):
        lon, lat, dist, dlon, dlat, ddist = ephemeris(ask["name"], ask["jd"] + i * ask["step"])
        dlon += scn.get("rate_error_deg", 0.0)
        ddist += scn.get("rate_error_au", 0.0)
        if ask["deltat"] < scn.get("rate_error_alt_under", float("-inf")):
            dlon += scn.get("rate_error_deg_alt", 0.0)
            ddist += scn.get("rate_error_au_alt", 0.0)
        # Twelve places: the central difference divides by 12h = 0.0117 d, so
        # the printed resolution alone sets a noise floor of ~1e-10 deg/day.
        # At the .9f the corrApplied scenarios use, that floor is 1e-7 and
        # the A.3 default bound of 1e-5 would be only two decades away.
        print(f"0 {i} " + " ".join(f"{v:.12f}" for v in (lon, lat, dist, dlon, dlat, ddist)))
    return 0


def main(argv):
    path = os.environ.get("FAKEWIRE_SCENARIO")
    if not path:
        print("FAKEWIRE_SCENARIO is not set", file=sys.stderr)
        return 2
    with open(path) as f:
        scn = json.load(f)

    ask = parse_argv(argv)
    obs_bit = OBS_BIT[ask["observer"]]

    if not scn.get("no_welcome"):
        print('# WELCOME fakewire/1 protocol 4 engine "scripted" dataset none maxCells 100000')
    for observers, mask in scn.get("corrmasks", []):
        print(f"# corrmask observers {observers} corrections {mask}")
    for observers, kinds, mask in scn.get("corrkinds", []):
        print(f"# corrkind observers {observers} kinds {kinds} corrections {mask}")
    bound = scn.get("ratesbound")
    alt_when = scn.get("ratesbound_alt_when")
    if alt_when is not None and alt_when in ask["name"]:
        bound = scn.get("ratesbound_alt", bound)
    if bound:
        print(f"# ratesbound {bound[0]:g} {bound[1]:g}")
    print("# caps kinds 63 observers 31")
    print("# request 1")

    if scn.get("mode") == "rates":
        return emit_series(scn, ask)

    if ask["observer"] not in scn.get("answer", list(OBS_BIT)):
        print(f'# object 0 name "{ask["name"]}" rowsOk 0 corr 0 err 2 '
              f'"not supported by this server for this object"')
        return 0

    declared = declared_for(scn, obs_bit, ask["kind"])
    # What the engine can really do here: everything declared, less whatever
    # the scenario says it cannot apply.
    avail = declared & ~scn.get("unavailable", 0)

    mode = scn.get("mode", "honest")
    if mode == "honest":
        claimed = avail
    elif mode == "varies":
        # Structural in name only: the slot grows when the request asks for
        # more. Still a subset of what WELCOME declares, and still correct at
        # mask 0, so only `independence` can see it.
        claimed = avail | (declared & ask["mask"])
    elif mode == "overclaim":
        claimed = avail | scn.get("claim_extra", 0)
    elif mode == "false-denial":
        # The bit is applied -- the position moves -- and the server says no.
        claimed = avail & ~scn.get("deny", LIGHT_TIME)
    else:
        print(f"unknown mode {mode!r}", file=sys.stderr)
        return 2

    # The position: a fixed direction per object, moved by each correction
    # the engine actually applies for this request.
    base = sum(ord(c) for c in ask["name"]) % 360
    lon = float(base)
    for bit, shift in SHIFT_ARCSEC.items():
        if ask["mask"] & bit & avail:
            lon += shift / 3600.0
    lat = 5.0 + (obs_bit * 0.25)  # off the equator: a degree of longitude is
    #                               not a degree of sky, and separation_arcsec
    #                               is the thing being exercised

    print(f'# object 0 name "{ask["name"]}" rowsOk 1 corr {claimed} err 0 ""')
    print(f"0 0 {lon:.9f} {lat:.9f} 1.0 0.0 0.0 0.0")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

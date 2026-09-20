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
"""

import json
import os
import sys

LIGHT_TIME, DEFLECTION, ABERRATION = 1, 2, 4
OBS_BIT = {"geo": 0, "topo": 1, "helio": 2, "bary": 3, "body": 4}
# Each correction shifts the answer by its own amount, all far above
# corrapplied.py's 1e-3" floor, so "moved" and "did not move" are decided by
# the scenario and never by arithmetic noise.
SHIFT_ARCSEC = {LIGHT_TIME: 20.0, DEFLECTION: 5.0, ABERRATION: 12.0}


def parse_argv(argv):
    """The subset of the client's command line corrapplied.py ever passes."""
    out = {"observer": "geo", "kind": 0, "name": "?", "mask": 0}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--corrections":
            out["mask"] = int(argv[i + 1])
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


def main(argv):
    path = os.environ.get("FAKEWIRE_SCENARIO")
    if not path:
        print("FAKEWIRE_SCENARIO is not set", file=sys.stderr)
        return 2
    with open(path) as f:
        scn = json.load(f)

    ask = parse_argv(argv)
    obs_bit = OBS_BIT[ask["observer"]]

    print('# WELCOME fakewire/1 protocol 4 engine "scripted" dataset none maxCells 100000')
    for observers, mask in scn.get("corrmasks", []):
        print(f"# corrmask observers {observers} corrections {mask}")
    for observers, kinds, mask in scn.get("corrkinds", []):
        print(f"# corrkind observers {observers} kinds {kinds} corrections {mask}")
    print("# caps kinds 63 observers 31")
    print("# request 1")

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

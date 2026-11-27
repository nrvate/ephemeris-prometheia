#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a small protocol v4 conformance directory, for fixturestest.py.

`tools/check/ephproto4_fixtures.py` reads Astrolog's conformance set. To
grade *its* assertions something has to be read, and their set is the wrong
thing to read: it is another project's, it is not in this tree, and on this
machine it is not on disk at all -- so a selftest that needed it would skip,
and a skip is a green that could not have been red.

These are not conformance fixtures and must never be mistaken for them. They
carry no authority about the protocol: they exist so that a manifest can be
emptied, a checksum broken and a verdict flipped, and the reader's reaction
graded. The real set stays the authority for what the bytes mean; this is a
set of inputs with known verdicts.

The envelopes are written from `third_party/ephproto/v4/ephproto.h` and
`registries.json`, the vendored byte-level authority (CLAUDE.md), and not
from Astrolog's codec.

**What this cannot see:** whether the reader reads the protocol correctly.
The verdicts here are what *our* reader should say, so a shared
misunderstanding stays invisible -- that is exactly what the real fixture set
is for, and this does not replace it.
"""
import hashlib
import os
import struct

MAGIC = 0x1EF0


def env(mtype, request_id, payload, version=4, flags=0):
    return struct.pack("<HBBHHII", MAGIC, version, flags, mtype, 0, request_id,
                       len(payload)) + payload


def s8(text):
    b = text.encode()
    assert len(b) < 256
    return bytes([len(b)]) + b


def tlv(entries):
    """entries: [(tag, payload)], written in ascending tag order."""
    body = b""
    for tag, payload in sorted(entries):
        body += struct.pack("<HH", tag, len(payload)) + payload
    return struct.pack("<H", len(body)) + body


def hello(token="", extra=()):
    payload = struct.pack("<IIII", 4, 4, 0, 0) + s8("fixturestest") + s8(token) + tlv(extra)
    return env(1, 0, payload)


# 0x0004: one entry, every observer, correction mask 7. 0x0014 adds the
# per-kind entry the judgements below turn on.
def corrections_tlv(mask=7, observers=0x1F):
    return bytes([1]) + struct.pack("<IB", observers, mask)


def welcome(by_kind=None):
    caps = [
        (0x0001, struct.pack("<I", 1)), (0x0002, struct.pack("<I", 63)),
        (0x0003, struct.pack("<III", 10000, 100, 64)),
        (0x0004, corrections_tlv()),
        (0x0005, struct.pack("<II", 1, 1)), (0x0006, struct.pack("<I", 15)),
        (0x0008, struct.pack("<I", 1)), (0x0009, struct.pack("<I", 1)),
    ]
    if by_kind is not None:
        caps.append((0x0014, by_kind))
    payload = struct.pack("<IIIIIII", 4, 4, 0, 0, 0, 0, 0) + bytes([4]) + b"\0\0\0"
    payload += s8("fakefixtures") + s8("none") + s8("none") + tlv(caps)
    return env(2, 0, payload)


def profile(observer=0, corrections=7):
    return (bytes([observer, 0, 0, 0, corrections, 1, 0, 0])
            + struct.pack("<i", 0) + struct.pack("<ddd", 0.0, 0.0, 0.0)
            + struct.pack("<dd", 0.0, 0.0) + struct.pack("<d", 0.0)
            + struct.pack("<I", 0) + s8("") + tlv(()))


def request(corrections=7, kind=0, naif=399):
    payload = bytes([0, 0, 0, 0]) + struct.pack("<I", 0) + struct.pack("<f", 0.0)
    payload += struct.pack("<I", 0)
    payload += bytes([0, 0, 0, 0])                       # TT, grid
    payload += struct.pack("<dd", 2451545.0, 0.0) + struct.pack("<q", 0)
    payload += struct.pack("<I", 1)                      # nTime = 1
    payload += struct.pack("<d", 0.0)                    # deltaTSec
    payload += bytes([1]) + profile(corrections=corrections)
    payload += struct.pack("<H", 1) + bytes([kind, 0, 0, 0]) + struct.pack("<i", naif)
    payload += tlv(())
    return env(3, 1, payload)


def error(code=11, text="not permitted"):
    return env(5, 1, struct.pack("<HHI", code, 0, 0) + s8(text) + tlv(()))


# name -> (bytes, the verdict the reader should reach, why it is in the set)
def fixtures():
    return {
        "hello-ok": (hello(), "ok", "the smallest well-formed message"),
        "hello-trailing": (env(1, 0, hello()[16:] + b"\0"), "malformed",
                           "one byte past the payload"),
        "hello-critical-tlv": (hello(extra=[(0x8001, b"")]), "unsupported",
                               "an unknown critical TLV tag"),
        "error-ok": (error(), "ok", "an ERROR, which carries free text"),
        "welcome-ok": (welcome(), "ok", "the WELCOME the judgements are made under"),
        "welcome-kind": (welcome(by_kind=bytes([1]) + struct.pack("<IIB", 0x1F, 0x3F, 3)),
                         "ok", "a WELCOME whose 0x0014 narrows kinds to mask 3"),
        "request-7": (request(corrections=7), "ok", "asks for all three corrections"),
        "request-3": (request(corrections=3), "ok", "asks for light time and deflection"),
    }


# request, welcome, the outcome. Both outcomes appear: a table of one passes a
# reader that always gives it, which ephproto4_fixtures.py asserts about.
def judgements():
    return [
        ("request-7", "welcome-ok", "served"),
        ("request-3", "welcome-kind", "served"),
        ("request-3", "welcome-ok", "error11"),
    ]


def write(directory, rows=None, judge_rows=None, set_sha=True, with_judgements=True):
    """Write a conformance-shaped directory. Returns the files written."""
    os.makedirs(directory, exist_ok=True)
    table = fixtures()
    rows = list(table) if rows is None else rows
    for name in rows:
        with open(os.path.join(directory, name + ".hex"), "w") as f:
            f.write(table[name][0].hex() + "\n")
    lines = ["\t".join((name + ".hex", "c2s", "x", table[name][1], table[name][2]))
             for name in rows]
    h = hashlib.sha256()
    for name in rows:
        with open(os.path.join(directory, name + ".hex"), "rb") as f:
            h.update(f.read())
    with open(os.path.join(directory, "MANIFEST.tsv"), "w") as f:
        if set_sha:
            f.write(f"# set-sha256 {h.hexdigest()}\n")
        f.write("".join(l + "\n" for l in lines))
    path = os.path.join(directory, "JUDGEMENTS.tsv")
    if not with_judgements:
        if os.path.exists(path):
            os.remove(path)
        return
    body = "".join("\t".join(r) + "\n" for r in
                   (judgements() if judge_rows is None else judge_rows))
    with open(path, "w") as f:
        f.write(f"# set-sha256 {hashlib.sha256(body.encode()).hexdigest()}\n")
        f.write(body)


if __name__ == "__main__":
    import sys
    write(sys.argv[1])
    print(f"wrote a synthetic conformance directory to {sys.argv[1]}")

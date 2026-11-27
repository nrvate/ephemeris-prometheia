# SPDX-License-Identifier: GPL-2.0-or-later
"""The client half of protocol version 4, in Python (docs/PYTHON.md, docs/SERVER.md).

Written from the vendored byte-level authority, third_party/ephproto/v4/
ephproto.h (Astrolog's specification, EPHEMERIS_PLUGINS_PLAN.md section 3):
the messages a client sends (HELLO, REQUEST, LOOKUP) are encoded, and the
ones it receives (WELCOME, DATA, ERROR, LOOKUP_RESULT) decoded. Everything is
little-endian, and nothing is read that the message does not carry.
"""
import math
import struct

from ._errors import Error

MAGIC = 0x1EF0
VERSION = 4
ENVELOPE = 16

MSG_HELLO, MSG_WELCOME, MSG_REQUEST, MSG_DATA, MSG_ERROR = 1, 2, 3, 4, 5
MSG_PING, MSG_PONG, MSG_CANCEL, MSG_LOOKUP, MSG_LOOKUP_RESULT = 6, 7, 8, 9, 10

CAP_F32, CAP_CANCEL, CAP_LOOKUP, CAP_INSTANT_LISTS = 1, 4, 8, 16

OBS_GEO, OBS_TOPO, OBS_HELIO, OBS_BARY, OBS_BODY = 0, 1, 2, 3, 4
PLANE_ECLIPTIC, PLANE_EQUATOR = 0, 1
FORM_SPHERICAL = 0
FRAMES = {"true-of-date": 0, "mean-of-date": 1, "j2000": 2, "icrf": 3}
CORR_LIGHT_TIME, CORR_DEFLECTION, CORR_ABERRATION = 1, 2, 4
SIDEREAL_PLANES = {"date": 0, "anchor": 1, "invariable": 2}
TIME_UT1, TIME_TT, TIME_TDB = 0, 1, 2

KIND_BODY, KIND_ORBIT_POINT, KIND_STAR, KIND_HYPOTHETICAL = 0, 1, 2, 3
KIND_ELEMENTS, KIND_DESIGNATION, KIND_HOUSE = 4, 5, 6
POINTS = {"ascending-node": 0, "descending-node": 1, "perihelion": 2, "aphelion": 3}
METHODS = {"mean": 0, "osculating": 1, "interpolated": 2}

COL_SIGMA, COL_AYANAMSA, COL_LIGHT_TIME, COL_DELTA_T, COL_ARMC, COL_OBLIQUITY = (
    1, 2, 4, 8, 16, 32)
META_NO_SPEEDS, META_NO_DISTANCE, META_RATES_APPROX = 16, 32, 64
CHUNK_LAST, CHUNK_META = 1, 4
TAG_PRECESSION = 0x0003

# A.17, in the JSON surface's words (docs/JSON_API.md).
OBJECT_ERRORS = {
    1: "unknown-name", 2: "unsupported", 3: "outside-coverage", 4: "data-unavailable",
    5: "unsupported", 6: "ambiguous-name", 7: "numerical-failure", 8: "internal",
    9: "undefined-at-latitude",
}

_CANONICAL_NAN = struct.pack("<Q", 0x7FF8000000000000)


class Writer:
    def __init__(self):
        self.out = bytearray()

    def u8(self, v):
        self.out += struct.pack("<B", v)

    def u16(self, v):
        self.out += struct.pack("<H", v)

    def u32(self, v):
        self.out += struct.pack("<I", v)

    def i32(self, v):
        self.out += struct.pack("<i", v)

    def i64(self, v):
        self.out += struct.pack("<q", v)

    def f32(self, v):
        self.out += struct.pack("<f", v)

    def f64(self, v):
        if not math.isfinite(v):
            raise Error("a number sent to the server must be finite")
        self.out += struct.pack("<d", v)

    def time(self, jd):
        # A two-part Julian date; the whole in the first part keeps the
        # fraction's precision in the second.
        whole = math.floor(jd)
        self.f64(float(whole))
        self.f64(jd - whole)

    def str8(self, s):
        b = s.encode("utf-8")
        if len(b) > 255:
            raise Error("a name sent to the server is at most 255 bytes")
        self.u8(len(b))
        self.out += b

    def tlv(self, entries):
        """entries: [(tag, bytes)], tags ascending."""
        body = bytearray()
        for tag, value in sorted(entries):
            body += struct.pack("<HH", tag, len(value)) + value
        self.u16(len(body))
        self.out += body


class Reader:
    def __init__(self, data):
        self.data = memoryview(data)
        self.at = 0

    def _take(self, n, what):
        if self.at + n > len(self.data):
            raise Error("the server's %s is truncated" % what)
        b = self.data[self.at:self.at + n]
        self.at += n
        return b

    def u8(self, what="message"):
        return self._take(1, what)[0]

    def u16(self, what="message"):
        return struct.unpack("<H", self._take(2, what))[0]

    def u32(self, what="message"):
        return struct.unpack("<I", self._take(4, what))[0]

    def i32(self, what="message"):
        return struct.unpack("<i", self._take(4, what))[0]

    def f32(self, what="message"):
        return struct.unpack("<f", self._take(4, what))[0]

    def f64(self, what="message"):
        return struct.unpack("<d", self._take(8, what))[0]

    def time(self, what="message"):
        return self.f64(what) + self.f64(what)

    def str8(self, what="message"):
        n = self.u8(what)
        return bytes(self._take(n, what)).decode("utf-8", "replace")

    def tlv(self, what="message"):
        total = self.u16(what)
        area = Reader(self._take(total, what))
        out = {}
        while area.at < len(area.data):
            tag = area.u16(what)
            n = area.u16(what)
            out[tag] = bytes(area._take(n, what))
        return out

    def left(self):
        return len(self.data) - self.at


# ---- envelope -------------------------------------------------------------------


def envelope(mtype, request_id, payload):
    return struct.pack("<HBBHHII", MAGIC, VERSION, 0, mtype, 0, request_id, len(payload)) + payload


def open_envelope(message):
    if len(message) < ENVELOPE:
        raise Error("the server sent a message shorter than the envelope")
    magic, version, flags, mtype, reserved, request_id, n = struct.unpack(
        "<HBBHHII", message[:ENVELOPE])
    if magic != MAGIC or n != len(message) - ENVELOPE:
        raise Error("the server sent a message that is not protocol v4")
    if flags & 1:
        raise Error("the server sent a compressed payload, which this client did not ask for")
    return mtype, request_id, bytes(message[ENVELOPE:])


# ---- HELLO, WELCOME -----------------------------------------------------------------


def hello(client_name, token=""):
    w = Writer()
    w.u32(VERSION)
    w.u32(VERSION)
    w.u32(0)
    w.u32(CAP_LOOKUP | CAP_INSTANT_LISTS)
    w.str8(client_name)
    w.str8(token or "")
    w.tlv([])
    return bytes(w.out)


def _tokens(value):
    r = Reader(value)
    return [r.str8("capability") for _ in range(r.u16("capability"))]


def welcome(payload):
    r = Reader(payload)
    out = {
        "protocol": r.u32("WELCOME"), "caps": r.u32("WELCOME"), "max_objects": r.u32("WELCOME"),
        "max_rows": r.u32("WELCOME"), "max_chunk_rows": r.u32("WELCOME"),
        "max_payload": r.u32("WELCOME"), "max_cells": r.u32("WELCOME"),
        "max_profiles": r.u8("WELCOME"),
    }
    r.u8("WELCOME")
    r.u16("WELCOME")
    out["server"] = r.str8("WELCOME")
    out["engine"] = r.str8("WELCOME")
    out["dataset"] = r.str8("WELCOME")
    tlvs = r.tlv("WELCOME")
    get32 = lambda tag: struct.unpack("<I", tlvs[tag][:4])[0] if tag in tlvs else 0  # noqa: E731
    out["kinds"] = get32(0x0001)
    out["observers"] = get32(0x0002)
    out["columns"] = get32(0x0006)
    out["time_scales"] = get32(0x0009)
    out["zodiacs"] = _tokens(tlvs[0x0007]) if 0x0007 in tlvs else []
    out["hypotheticals"] = _tokens(tlvs[0x0011]) if 0x0011 in tlvs else []
    out["delta_t_model"] = Reader(tlvs[0x000C]).str8() if 0x000C in tlvs else ""
    if 0x0015 in tlvs:
        hr = Reader(tlvs[0x0015])
        out["house_systems"] = [hr.u8() for _ in range(hr.u16())]
    else:
        out["house_systems"] = []
    out["sidereal_time"] = Reader(tlvs[0x0016]).str8() if 0x0016 in tlvs else ""
    coverage = []
    if 0x000A in tlvs:
        cr = Reader(tlvs[0x000A])
        for _ in range(cr.u16()):
            coverage.append({"id": cr.str8(), "first_jd_tdb": cr.time(), "last_jd_tdb": cr.time()})
    out["coverage"] = coverage
    return out


# ---- REQUEST ----------------------------------------------------------------------


def profile(observer=OBS_GEO, plane=PLANE_ECLIPTIC, frame=0, corrections=7, speeds=True,
            sidereal_plane=0, observer_body=0, site=(0.0, 0.0, 0.0), anchor=(0.0, 0.0),
            columns=0, zodiac=""):
    """One A.9 profile as a dict (encoded by request())."""
    return dict(observer=observer, plane=plane, frame=frame, corrections=corrections,
                speeds=speeds, sidereal_plane=sidereal_plane, observer_body=observer_body,
                site=site, anchor=anchor, columns=columns, zodiac=zodiac)


def request(time_scale, instants, profiles, objects, precession=None):
    """A samples REQUEST over a list of instants (JD in `time_scale`).

    objects: dicts with 'kind', 'profile' and the kind's fields: 'naif'
    (body, orbit point), 'point' and 'method' (orbit point; kind 6's point),
    'system' (kind 6), 'name' (star, hypothetical, designation)."""
    w = Writer()
    # Delivery block (16 bytes): f64, priority 0, samples, no hints, no deadline.
    for v in (0, 0, 0, 0):
        w.u8(v)
    w.u32(0)
    w.f32(0.0)
    w.u32(0)
    # Question block: an instant list, the server's own delta T (canonical NaN).
    w.u8(time_scale)
    w.u8(1)
    w.u16(0)
    w.u32(len(instants))
    for jd in instants:
        w.time(jd)
    w.out += _CANONICAL_NAN
    w.u8(len(profiles))
    for p in profiles:
        w.u8(p["observer"])
        w.u8(p["plane"])
        w.u8(FORM_SPHERICAL)
        w.u8(p["frame"])
        w.u8(p["corrections"])
        w.u8(1 if p["speeds"] else 0)
        w.u8(p["sidereal_plane"])
        w.u8(0)
        w.i32(p["observer_body"])
        for x in p["site"]:
            w.f64(float(x))
        if p["anchor"] == (0.0, 0.0):
            w.f64(0.0)
            w.f64(0.0)
        else:
            w.time(p["anchor"][0])
        w.f64(float(p["anchor"][1]))
        w.u32(p["columns"])
        w.str8(p["zodiac"])
        w.tlv([])
    w.u16(len(objects))
    for o in objects:
        w.u8(o["kind"])
        w.u8(o.get("profile", 0))
        w.u16(0)
        k = o["kind"]
        if k == KIND_BODY:
            w.i32(o["naif"])
        elif k == KIND_ORBIT_POINT:
            w.i32(o["naif"])
            w.u8(o["point"])
            w.u8(o["method"])
            w.u16(0)
        elif k == KIND_HOUSE:
            w.u8(o["system"])
            w.u8(o["point"])
        elif k in (KIND_STAR, KIND_HYPOTHETICAL, KIND_DESIGNATION):
            w.str8(o["name"])
        else:
            raise Error("object kind %d is not sent by this client" % k)
    ext = []
    if precession:
        pw = Writer()
        pw.str8(precession)
        ext.append((TAG_PRECESSION, bytes(pw.out)))
    w.tlv(ext)
    return bytes(w.out)


# ---- DATA, ERROR ---------------------------------------------------------------------


def data(payload):
    r = Reader(payload)
    chunk = {"index": r.u32("DATA"), "i_time": r.u32("DATA"), "n_rows": r.u32("DATA"),
             "total_rows": r.u32("DATA"), "precision": r.u8("DATA"), "flags": r.u8("DATA"),
             "n_obj": r.u16("DATA"), "columns": r.u32("DATA")}
    chunk["sources"], chunk["meta"] = [], []
    if chunk["flags"] & CHUNK_META:
        chunk["sources"] = [r.str8("DATA") for _ in range(r.u8("DATA"))]
        for _ in range(chunk["n_obj"]):
            chunk["meta"].append({
                "rows_ok": r.i32("META"), "err_code": r.u16("META"), "source": r.u8("META"),
                "flags": r.u8("META"), "corr_applied": r.u8("META"), "naif": r.i32("META"),
                "first_failed_row": r.u32("META"), "name": r.str8("META"),
                "err_text": r.str8("META")})
    n_cols = 6 + bin(chunk["columns"]).count("1")
    n = chunk["n_obj"] * chunk["n_rows"] * n_cols
    size = 4 if chunk["precision"] == 1 else 8
    if r.left() != n * size:
        raise Error("the server's DATA values do not fill its payload")
    fmt = "<%d%s" % (n, "f" if size == 4 else "d")
    chunk["values"] = list(struct.unpack(fmt, bytes(r._take(n * size, "DATA"))))
    chunk["n_cols"] = n_cols
    return chunk


def error(payload):
    r = Reader(payload)
    code = r.u16("ERROR")
    r.u16("ERROR")
    r.u32("ERROR")
    return code, r.str8("ERROR")


# ---- LOOKUP ----------------------------------------------------------------------------


def lookup(queries, max_matches=8, prefix=False):
    w = Writer()
    w.u16(max_matches)
    w.u8(1 if prefix else 0)
    w.u8(len(queries))
    for q in queries:
        w.str8(q)
    w.tlv([])
    return bytes(w.out)


def lookup_result(payload):
    r = Reader(payload)
    n_queries = r.u8("LOOKUP_RESULT")
    r.u8("LOOKUP_RESULT")
    sources = [r.str8("LOOKUP_RESULT") for _ in range(r.u8("LOOKUP_RESULT"))]
    out = []
    for _ in range(n_queries):
        matches = []
        for _ in range(r.u16("LOOKUP_RESULT")):
            quality = r.u8()
            r.u8()
            match_len = r.u16()
            start = r.at
            kind = r.u8()
            r.u8()
            r.u16()
            obj = {"kind": kind}
            if kind == KIND_BODY:
                obj["naif"] = r.i32()
            elif kind == KIND_ORBIT_POINT:
                obj["naif"] = r.i32()
                obj["point"], obj["method"] = r.u8(), r.u8()
                r.u16()
            elif kind == KIND_HOUSE:
                obj["system"], obj["point"] = r.u8(), r.u8()
            elif kind in (KIND_STAR, KIND_HYPOTHETICAL, KIND_DESIGNATION):
                obj["name"] = r.str8()
            else:
                r.at = start + match_len  # a kind this client does not know: skipped
                continue
            name, designation = r.str8(), r.str8()
            r.time()
            r.time()
            r.at = start + match_len
            matches.append({"quality": quality, "object": obj, "name": name,
                            "designation": designation})
        out.append(matches)
    del sources
    return out

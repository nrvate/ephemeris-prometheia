#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Parse Astrolog's protocol version 4 conformance fixtures with an
independent reader written from the spec text (docs/SERVER.md records the
design; the normative text is EPHEMERIS_PLUGINS_PLAN.md §3 and Appendix A in
Astrolog's tree).

This is Prometheia's second reading of the spec: it shares no code with
Astrolog's codec or with its fixture generator, so a disagreement between the
two readings is a place where the spec, one parser or one fixture is wrong.
It is a review tool, not the server's codec; prometheiad's C++ codec comes
with the version 4 migration.

Usage:
  ephproto4_fixtures.py --dir /nvm/work/ephv4/ephsrv/conformance
  ephproto4_fixtures.py --dir ... --verbose      # print every fixture
  ephproto4_fixtures.py --dir ... --judge REQUEST.hex WELCOME.hex
      # the per-kind drop, section 2: is this request served under that WELCOME?
"""
import argparse
import hashlib
import math
import os
import re
import struct
import sys

MAGIC = 0x1EF0
CANONICAL_NAN = 0x7FF8000000000000

ZODIACS = {
    "fagan-bradley", "lahiri", "deluce", "raman", "usha-shashi", "krishnamurti",
    "djwhal-khul", "yukteshwar", "jn-bhasin", "babyl-kugler1", "babyl-kugler2",
    "babyl-kugler3", "babyl-huber", "babyl-etpsc", "aldebaran-15tau", "hipparchos",
    "sassanian", "galcent-0sag", "j2000", "j1900", "b1950", "suryasiddhanta",
    "suryasiddhanta-msun", "aryabhata", "aryabhata-msun", "ss-revati", "ss-citra",
    "true-citra", "true-revati", "true-pushya", "galcent-rgilbrand", "galequ-iau1958",
    "galequ-true", "galequ-mula", "galalign-mardyks", "true-mula",
    "galcent-mula-wilhelm", "aryabhata-522", "babyl-britton", "true-sheoran",
    "galcent-cochrane", "galequ-fiorenza", "valens-moon", "lahiri-1940",
    "lahiri-vp285", "krishnamurti-vp291", "lahiri-icrc", "user",
}
HYPOTHETICALS = {
    "cupido", "hades", "zeus", "kronos", "apollon", "admetos", "vulcanus", "poseidon",
    "isis-transpluto", "nibiru", "harrington", "neptune-leverrier", "neptune-adams",
    "pluto-lowell", "pluto-pickering", "vulcan", "white-moon", "proserpina", "waldemath",
}
REQUEST_TLVS = {0x0003, 0x8001, 0x8002, 0x8003, 0x8004}


class Malformed(Exception):
    """ERROR 1: malformed or non-canonical."""


class Unsupported(Exception):
    """ERROR 11: a value or extension the registries do not list."""


class Reader:
    def __init__(self, data):
        self.d = data
        self.i = 0

    def take(self, n):
        if self.i + n > len(self.d):
            raise Malformed(f"truncated: wanted {n} bytes at {self.i} of {len(self.d)}")
        out = self.d[self.i:self.i + n]
        self.i += n
        return out

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def i32(self):
        return struct.unpack("<i", self.take(4))[0]

    def i64(self):
        return struct.unpack("<q", self.take(8))[0]

    def f32(self, what="f32"):
        v = struct.unpack("<f", self.take(4))[0]
        if not math.isfinite(v):
            raise Malformed(f"{what}: non-finite float")
        return v

    def f64(self, what="f64", allow_nan=False):
        raw = self.take(8)
        bits = struct.unpack("<Q", raw)[0]
        v = struct.unpack("<d", raw)[0]
        if math.isfinite(v):
            return v
        if allow_nan and bits == CANONICAL_NAN:
            return v
        raise Malformed(f"{what}: non-finite float (bits {bits:#018x})"
                        + ("; only the canonical NaN is allowed here" if allow_nan else ""))

    def zero(self, n, what):
        if any(b for b in self.take(n)):
            raise Malformed(f"{what}: reserved bytes must be zero")

    def str8(self, what="str8"):
        n = self.u8()
        raw = self.take(n)
        try:
            s = raw.decode("utf-8")
        except UnicodeDecodeError as e:
            raise Malformed(f"{what}: not valid UTF-8 ({e})") from None
        if any(ord(c) < 0x20 for c in s):
            raise Malformed(f"{what}: C0 control character")
        return s

    def time(self, what="TIME", allow_zero=True):
        jd1 = self.f64(what + ".jd1")
        jd2 = self.f64(what + ".jd2")
        if abs(jd1) + abs(jd2) > 1e8:
            raise Malformed(f"{what}: |jd1| + |jd2| exceeds 1e8")
        if not allow_zero and jd1 == 0.0 and jd2 == 0.0:
            raise Malformed(f"{what}: must not be zero here")
        return (jd1, jd2)

    def tlv(self, known, what="TLV"):
        total = self.u16()
        end = self.i + total
        if end > len(self.d):
            raise Malformed(f"{what}: totalLen {total} runs past the payload")
        seen = []
        out = {}
        while self.i < end:
            if self.i + 4 > end:
                raise Malformed(f"{what}: entry header runs past totalLen")
            tag = self.u16()
            n = self.u16()
            if self.i + n > end:
                raise Malformed(f"{what}: entry {tag:#06x} runs past totalLen")
            payload = self.take(n)
            if seen and tag <= seen[-1]:
                raise Malformed(f"{what}: tags must ascend strictly ({tag:#06x} after "
                                f"{seen[-1]:#06x})")
            seen.append(tag)
            experimental = 0x7000 <= (tag & 0x7FFF) <= 0x7FFF
            if tag not in known and not experimental:
                if tag & 0x8000:
                    raise Unsupported(f"{what}: unknown critical tag {tag:#06x}")
            out[tag] = payload
        if self.i != end:
            raise Malformed(f"{what}: entries do not fill totalLen")
        return out

    def done(self, what):
        if self.i != len(self.d):
            raise Malformed(f"{what}: {len(self.d) - self.i} trailing bytes")


def parse_envelope(data):
    if len(data) < 16:
        raise Malformed("shorter than the 16-byte envelope")
    magic, version, flags, mtype, reserved, request_id, payload_len = struct.unpack(
        "<HBBHHII", data[:16])
    if magic != MAGIC:
        raise Malformed(f"bad magic {magic:#06x}")
    if flags & ~0x01:
        raise Malformed(f"envelope flag bits {flags:#04x}")
    if reserved:
        raise Malformed("envelope reserved u16 must be zero")
    if payload_len != len(data) - 16:
        raise Malformed(f"payloadLen {payload_len} but {len(data) - 16} bytes follow")
    return version, flags, mtype, request_id, data[16:]


def parse_profile(r, request_id_unused=None):
    observer = r.u8()
    plane = r.u8()
    form = r.u8()
    frame = r.u8()
    corrections = r.u8()
    speeds = r.u8()
    sidereal_plane = r.u8()
    r.zero(1, "PROFILE reserved")
    if observer > 4:
        raise Unsupported(f"observer {observer} is not in A.5")
    if plane > 1:
        raise Unsupported(f"plane {plane} is not in A.6")
    if form > 1:
        raise Unsupported(f"form {form} is not in A.6")
    if frame > 3:
        raise Unsupported(f"frame {frame} is not in A.6")
    if corrections & ~0x07:
        raise Unsupported(f"correction bits {corrections:#04x} are not in A.7")
    if speeds > 1:
        raise Malformed(f"speeds {speeds} is not 0 or 1")
    if sidereal_plane > 2:
        raise Unsupported(f"siderealPlane {sidereal_plane} is not in A.8")
    observer_body = r.i32()
    site = [r.f64("site"), r.f64("site"), r.f64("site")]
    anchor = r.time("anchorEpoch")
    anchor_ayanamsa = r.f64("anchorAyanamsaDeg")
    columns = r.u32()
    zodiac = r.str8("zodiac")
    r.tlv(set(), "PROFILE TLV")

    if observer != 4 and observer_body != 0:
        raise Malformed("observerBody must be zero unless observer = 4")
    if observer != 1 and any(v != 0.0 for v in site):
        raise Malformed("the site must be zero unless observer = 1")
    if observer == 1:
        if not -180.0 <= site[0] <= 180.0:
            raise Malformed("site longitude out of [-180, 180]")
        if not -90.0 <= site[1] <= 90.0:
            raise Malformed("site latitude out of [-90, 90]")
    if columns & ~0x0F:
        raise Unsupported(f"column bits {columns:#010x} are not in A.10")
    if zodiac and zodiac not in ZODIACS:
        raise Unsupported(f"zodiac token '{zodiac}' is not in A.11")
    if zodiac and plane == 1:
        raise Malformed("a sidereal zodiac applies to the ecliptic plane only")
    if not zodiac and sidereal_plane != 0:
        raise Malformed("tropical requires siderealPlane = 0")
    if zodiac != "user" and (anchor != (0.0, 0.0) or anchor_ayanamsa != 0.0):
        raise Malformed("the anchor must be zero unless the zodiac is 'user'")
    if zodiac == "user" and anchor == (0.0, 0.0):
        raise Malformed("zodiac 'user' requires a nonzero anchor epoch")
    return {"form": form, "columns": columns, "zodiac": zodiac, "observer": observer,
            "corrections": corrections}


def parse_object(r, n_profiles):
    kind = r.u8()
    profile = r.u8()
    r.zero(2, "OBJECT reserved")
    if kind > 5:
        raise Unsupported(f"object kind {kind} is not in A.12")
    if profile >= n_profiles:
        raise Malformed(f"object names profile {profile} of {n_profiles}")
    if kind == 0:
        r.i32()
    elif kind == 1:
        r.i32()
        point = r.u8()
        method = r.u8()
        r.zero(2, "orbit point reserved")
        if point > 3:
            raise Unsupported(f"orbit point {point} is not in A.13")
        if method > 4:
            raise Unsupported(f"orbit method {method} is not in A.14")
    elif kind == 2:
        r.str8("star name")
    elif kind == 3:
        token = r.str8("hypothetical")
        if token not in HYPOTHETICALS:
            raise Unsupported(f"hypothetical '{token}' is not in A.15")
    elif kind == 4:
        r.time("elements epoch")
        equinox = r.u8()
        centre = r.u8()
        n_terms = r.u8()
        r.zero(1, "elements reserved")
        equinox_jd = r.f64("equinoxJd")
        if equinox > 4:
            raise Unsupported(f"element equinox {equinox} is not in A.16")
        if centre > 1:
            raise Unsupported(f"element centre {centre} is not 0 or 1")
        if not 1 <= n_terms <= 5:
            raise Malformed(f"elements nTerms {n_terms} outside 1..5")
        if equinox != 4 and equinox_jd != 0.0:
            raise Malformed("equinoxJd must be zero unless equinox = 4")
        for _ in range(6 * n_terms):
            r.f64("element coefficient")
        r.str8("elements name")
    elif kind == 5:
        r.str8("designation")
    return kind, profile


def parse_request(r, request_id):
    precision = r.u8()
    priority = r.u8()
    representation = r.u8()
    max_degree_hint = r.u8()
    r.u32()  # chunkRows: a hint, any value
    seg_target = r.f32("segTargetErrArcsec")
    r.u32()  # deadlineMs: advisory, outside the cache key; any value
    if precision > 1:
        raise Malformed(f"precision {precision}")
    if priority > 1:
        raise Malformed(f"priority {priority}")
    if representation > 1:
        raise Unsupported(f"representation {representation}")
    segments = representation == 1
    if not segments:
        if max_degree_hint:
            raise Malformed("maxDegreeHint must be 0 for a samples request")
        if seg_target != 0.0:
            raise Malformed("segTargetErrArcsec must be 0 for a samples request")
    elif not seg_target > 0.0:
        raise Malformed("a segments request needs segTargetErrArcsec > 0")

    time_scale = r.u8()
    time_mode = r.u8()
    r.zero(2, "question reserved")
    if time_scale > 2:
        raise Unsupported(f"time scale {time_scale} is not in A.9")
    if time_mode > 1:
        raise Malformed(f"timeMode {time_mode}")
    if time_mode == 0:
        r.time("grid start")
        step_ns = r.i64()
        n_time = r.u32()
        if n_time == 0:
            raise Malformed("grid nTime is 0")
        if (n_time == 1) != (step_ns == 0):
            raise Malformed("stepNs must be 0 exactly when nTime = 1")
        if n_time > 1 and (n_time - 1) * abs(step_ns) > 0x7FFFFFFFFFFFFFFF:
            raise Malformed("(nTime-1) x |stepNs| overflows i64")
    else:
        if segments:
            raise Unsupported("segments require grid mode")
        n_time = r.u32()
        if n_time == 0:
            raise Malformed("list nTime is 0")
        for _ in range(n_time):
            r.time("list instant")
    delta_t = r.f64("deltaTSec", allow_nan=True)

    n_profiles = r.u8()
    if n_profiles == 0:
        raise Malformed("nProfiles is 0")
    profiles = [parse_profile(r) for _ in range(n_profiles)]
    n_obj = r.u16()
    if n_obj == 0:
        raise Malformed("nObj is 0")
    objects = [parse_object(r, n_profiles) for _ in range(n_obj)]
    tlvs = r.tlv(REQUEST_TLVS, "REQUEST TLV")

    if segments:
        for p in profiles:
            if p["form"] != 1:
                raise Unsupported("segments require rectangular form")
            if p["columns"]:
                raise Unsupported("segments allow no extra columns")
    if 0x8004 in tlvs:
        t = Reader(tlvs[0x8004])
        n = t.u32()
        if n < 2:
            raise Malformed(f"the delta T table has {n} entries; 2 or more are needed")
        last = None
        for _ in range(n):
            when = t.time("delta T table instant")
            t.f64("delta T table value")
            key = when[0] + when[1]
            if last is not None and key <= last:
                raise Malformed("the delta T table's instants must ascend strictly")
            last = key
        t.done("delta T table")
        if not math.isnan(delta_t):
            raise Malformed("with a delta T table, deltaTSec must be the canonical NaN")
    r.done("REQUEST")
    return {"profiles": profiles, "objects": objects, "segments": segments}


def parse_meta(r, n_obj):
    n_sources = r.u8()
    for _ in range(n_sources):
        r.str8("source")
    for _ in range(n_obj):
        r.i32()          # rowsOk
        err_code = r.u16()
        r.u8()           # sourceIdx
        meta_flags = r.u8()
        corr_applied = r.u8()   # §3.4: after metaFlags; three low bits used
        r.i32()          # resolvedNaif
        r.u32()          # firstFailedRow
        r.str8("meta name")
        r.str8("meta errText")
        # §3.1, the answering direction: an unknown per-object error code means
        # the object failed for an unknown reason, and unknown META flag bits
        # are ignored. Neither is malformed. corrApplied: the five low spares
        # are specified zero and the high bits are reserved; clients ignore
        # them, so neither reading refuses.
        _ = (err_code, meta_flags, corr_applied)


def parse_data(r):
    chunk_index = r.u32()
    i_time = r.u32()
    n_rows = r.u32()
    total_rows = r.u32()
    precision = r.u8()
    chunk_flags = r.u8()
    n_obj = r.u16()
    columns = r.u32()
    if precision > 1:
        # A registry value this build does not know (§3.1) — unsupported,
        # though refused, because the row width changes with it.
        raise Unsupported(f"precision {precision}")
    # §3.1: unknown bits in an answer's flag fields are ignored — except the
    # column bits: an unadvertised one changes the row width, so the message
    # cannot be read at all. The verdict is still unsupported (registry
    # growth), the taxonomy the codec uses.
    if columns & ~0x0F:
        raise Unsupported(f"columnsPresent {columns:#010x} are not in A.10")
    if i_time + n_rows > total_rows:
        raise Malformed("this chunk's rows run past totalRows")
    if chunk_index == 0 and not chunk_flags & 0x04:
        raise Malformed("the meta-present flag must be set on chunk 0")
    if chunk_flags & 0x04:
        parse_meta(r, n_obj)
    n_cols = 6 + bin(columns).count("1")
    width = 4 if precision == 1 else 8
    need = n_obj * n_rows * n_cols * width
    r.take(need)  # values; NaN marks a failed row, so they are not checked here
    r.done("DATA")


def parse_segment(r):
    r.time("segment mid")
    half = r.f64("halfSpanDays")
    degree = r.u8()
    r.zero(3, "SEGMENT reserved")
    r.f32("errArcsec")
    r.f32("errRelDist")
    r.f32("errRateArcsecPerDay")
    if not half > 0.0:
        raise Malformed("halfSpanDays must be > 0")
    if degree > 31:
        raise Malformed(f"degree {degree} exceeds 31")
    for _ in range(3 * (degree + 1)):
        r.f64("segment coefficient")


def parse_segdata(r):
    chunk_index = r.u32()
    chunk_flags = r.u8()
    r.zero(1, "SEGDATA reserved")
    n_obj = r.u16()
    i_obj = r.u16()
    n_obj_chunk = r.u16()
    r.zero(4, "SEGDATA reserved")
    # §3.1: unknown chunkFlags bits are ignored.
    if i_obj + n_obj_chunk > n_obj:
        raise Malformed("this chunk's objects run past nObj")
    if chunk_index == 0 and not chunk_flags & 0x04:
        raise Malformed("the meta-present flag must be set on chunk 0")
    if chunk_flags & 0x04:
        parse_meta(r, n_obj)
        n_ayan = r.u8()
        for _ in range(n_ayan):
            r.u8()  # profile
            n_seg = r.u32()
            for _ in range(n_seg):
                r.time("ayanamsa segment mid")
                if not r.f64("ayanamsa halfSpanDays") > 0.0:
                    raise Malformed("ayanamsa halfSpanDays must be > 0")
                degree = r.u8()
                r.zero(3, "AYANSEG reserved")
                r.f32("ayanamsa errArcsec")
                for _ in range(degree + 1):
                    r.f64("ayanamsa coefficient")
    for _ in range(n_obj_chunk):
        n_seg = r.u32()
        for _ in range(n_seg):
            parse_segment(r)
    r.done("SEGDATA")


# §3.2: requestId is 0 on connection-level messages and nonzero on a request
# and its answers.
ID_MUST_BE_ZERO = {1, 2, 6, 7}
ID_MUST_BE_NONZERO = {3, 4, 8, 9, 10, 15}


def parse_corrections_by_kind(value):
    """WELCOME tag 0x0014, CORRECTIONS_BY_KIND (the per-kind drop, revision 3,
    section 1): u8 n, then n entries of {u32 observerMask, u32 kindMask,
    u8 correctionMask}. Unknown observer and kind bits are ignored (1.4);
    correction bits above 0x07 are reserved, must be zero, and make the
    WELCOME non-canonical (1.4, 3.1). Entries only add to 0x0004 (1.1), so
    nothing about them can contradict it."""
    r = Reader(value)
    n = r.u8()
    entries = []
    for _ in range(n):
        observers, kinds, mask = r.u32(), r.u32(), r.u8()
        if mask & ~0x07:
            raise Malformed(f"CORRECTIONS_BY_KIND: reserved correction bits {mask:#04x}")
        entries.append((observers, kinds, mask))
    r.done("CORRECTIONS_BY_KIND")
    return entries


def welcome_corrections(tlvs):
    """(0x0004 entries, 0x0014 entries) of a WELCOME's TLVs."""
    t = Reader(tlvs[0x0004])
    by_observer = [(t.u32(), t.u8()) for _ in range(t.u8())]
    t.done("CORRECTIONS")
    by_kind = parse_corrections_by_kind(tlvs[0x0014]) if 0x0014 in tlvs else []
    return {"corrections": by_observer, "corrections_by_kind": by_kind}


def permitted(caps, observer, kind, mask):
    """The per-kind drop, section 1.1: the masks a client may send for an
    (observer, kind) pair are 0x0004's masks for the observer, union every
    0x0014 entry naming the pair. A pair no entry names falls back to 0x0004
    (1.3), which the union already says."""
    if any(o & (1 << observer) and m == mask for o, m in caps["corrections"]):
        return True
    return any(o & (1 << observer) and k & (1 << kind) and m == mask
               for o, k, m in caps["corrections_by_kind"])


def judge(caps, request):
    """The per-kind drop, section 2: each profile's mask is checked against
    every (observer, kind) of the objects that reference it; an unreferenced
    profile against 0x0004 alone. Any failure refuses the whole request with
    ERROR 11, whatever the representation. Returns 'served' or 'ERROR 11 ...'."""
    for idx, p in enumerate(request["profiles"]):
        kinds = {k for k, prof in request["objects"] if prof == idx}
        if not kinds:
            if not any(o & (1 << p["observer"]) and m == p["corrections"]
                       for o, m in caps["corrections"]):
                return f"ERROR 11: unreferenced profile {idx} mask {p['corrections']}"
            continue
        for k in sorted(kinds):
            if not permitted(caps, p["observer"], k, p["corrections"]):
                return (f"ERROR 11: profile {idx} mask {p['corrections']} not permitted "
                        f"for observer {p['observer']} kind {k}")
    return "served"


def parse_payload(version, mtype, request_id, payload):
    if mtype in ID_MUST_BE_ZERO and request_id != 0:
        raise Malformed(f"requestId must be 0 on message type {mtype}")
    if mtype in ID_MUST_BE_NONZERO and request_id == 0:
        raise Malformed(f"requestId must be nonzero on message type {mtype}")
    r = Reader(payload)
    if mtype == 1:
        proto_max = r.u32()
        proto_min = r.u32()
        r.u32()  # build
        r.u32()  # clientCaps
        r.str8("clientName")
        token = r.str8("token")
        r.tlv(set(), "HELLO TLV")
        r.done("HELLO")
        if proto_min > proto_max:
            raise Malformed(f"protoMin {proto_min} exceeds protoMax {proto_max}")
        if len(token.encode()) > 128:
            raise Malformed("token longer than 128 bytes")
    elif mtype == 2:
        for _ in range(7):
            r.u32()
        r.u8()            # maxProfiles
        r.zero(3, "WELCOME reserved")
        r.str8("serverName")
        r.str8("engine")
        r.str8("datasetId")
        tlvs = r.tlv(set(range(0x0001, 0x0015)), "WELCOME TLV")
        r.done("WELCOME")
        missing = [t for t in (1, 2, 3, 4, 5, 6, 8, 9) if t not in tlvs]
        if missing:
            raise Malformed("WELCOME is missing required capability tags "
                            + ", ".join(f"{t:#06x}" for t in missing))
        return welcome_corrections(tlvs)
    elif mtype == 3:
        return parse_request(r, request_id)
    elif mtype == 4:
        parse_data(r)
    elif mtype == 5:
        code = r.u16()
        flags = r.u16()
        r.u32()  # retryAfterMs
        r.str8("error text")
        r.tlv(set(), "ERROR TLV")
        r.done("ERROR")
        # An unknown ERROR code is a failure of unknown kind (§3.1); its flags
        # still say whether the connection closes and whether a retry may work.
        # An unknown ERROR code, and unknown flag bits, are tolerated (§3.1);
        # flags bits 0 and 1 keep their meaning.
        _ = (code, flags)
    elif mtype in (6, 7):
        r.done("PING/PONG")
    elif mtype == 8:
        r.done("CANCEL")
    elif mtype == 9:
        # Batched LOOKUP (f84d208): one message, many queries in order;
        # maxMatches is the budget for the whole answer.
        max_matches = r.u16()
        flags = r.u8()
        n_queries = r.u8()
        queries = []
        for _ in range(n_queries):
            queries.append(r.str8("query"))
        r.tlv(set(), "LOOKUP TLV")
        r.done("LOOKUP")
        if n_queries == 0:
            raise Malformed("LOOKUP with no queries")
        if max_matches == 0:
            raise Malformed("maxMatches is 0")
        if flags & ~0x07:
            raise Malformed(f"LOOKUP flags {flags:#04x}")
        for q in queries:
            if not q:
                raise Malformed("empty LOOKUP query")
    elif mtype == 10:
        # One answer list per LOOKUP query, in the order asked; a shared
        # source table; each match length-delimited so an unknown kind can
        # be skipped without costing the matches after it.
        n_queries = r.u8()
        flags = r.u8()
        n_sources = r.u8()
        for _ in range(n_sources):
            r.str8("source")
        if n_queries == 0:
            raise Malformed("LOOKUP_RESULT with no queries")
        for _ in range(n_queries):
            count = r.u16()
            for _ in range(count):
                quality = r.u8()
                source_idx = r.u8()
                match_len = r.u16()
                end = r.i + match_len
                if end > len(r.d):
                    raise Malformed("MATCH matchLen runs past the message")
                if source_idx >= n_sources:
                    raise Malformed("match names a source not in the table")
                _ = quality  # an unknown quality is tolerated (§3.1)
                try:
                    parse_object(r, 1)
                    r.str8("canonicalName")
                    r.str8("designation")
                    r.time("validMin")
                    r.time("validMax")
                except Unsupported:
                    # A kind this client cannot ask for: skip it by its length.
                    r.i = end
                    continue
                if r.i != end:
                    raise Malformed(f"MATCH matchLen {match_len} disagrees with a match "
                                f"this reader can read ({r.i - (end - match_len)} bytes)")
        r.done("LOOKUP_RESULT")
        # Unknown LOOKUP_RESULT flag bits are ignored (§3.1).
        _ = flags
    elif mtype == 15:
        parse_segdata(r)
    else:
        raise Unsupported(f"message type {mtype} has no layout in A.1")


def verdict(data):
    """'ok', 'malformed' or 'unsupported', with the reason."""
    try:
        version, _flags, mtype, request_id, payload = parse_envelope(data)
    except Malformed as e:
        return "malformed", str(e)
    if version < 4:
        # A pre-version-4 client: only an ERROR in that client's own layout.
        if mtype != 5:
            return "malformed", f"version {version} message of type {mtype}"
        r = Reader(payload)
        try:
            r.u32()
            r.i32()
            rest = r.take(len(payload) - 8)
            if not rest.endswith(b"\x00"):
                return "malformed", "legacy ERROR text is not NUL-terminated"
        except Malformed as e:
            return "malformed", str(e)
        return "ok", f"legacy version {version} ERROR"
    try:
        parse_payload(version, mtype, request_id, payload)
    except Malformed as e:
        return "malformed", str(e)
    except Unsupported as e:
        return "unsupported", str(e)
    except struct.error as e:
        return "malformed", f"truncated: {e}"
    return "ok", ""


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", required=True, help="the conformance directory")
    ap.add_argument("--judge", nargs=2, action="append", metavar=("REQUEST", "WELCOME"),
                    default=[], help="judge a REQUEST fixture against a WELCOME fixture's "
                    "correction capabilities (the per-kind drop, section 2); repeatable")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    manifest = os.path.join(args.dir, "MANIFEST.tsv")
    rows = []
    set_sha = None
    with open(manifest) as f:
        for line in f:
            if line.startswith("#"):
                m = re.match(r"#\s*set-sha256\s+([0-9a-fA-F]{64})\s*$", line)
                if m:
                    set_sha = m.group(1).lower()
                continue
            if not line.strip():
                continue
            cells = line.rstrip("\n").split("\t")
            rows.append(cells)

    # §3.10: the manifest is written last and carries a checksum over the set,
    # so a directory read while the generator is writing is caught rather than
    # reported against. The input is each fixture file's bytes exactly as
    # committed, concatenated in the manifest's row order; the manifest itself
    # is not part of it.
    h = hashlib.sha256()
    for cells in rows:
        with open(os.path.join(args.dir, cells[0]), "rb") as fh:
            h.update(fh.read())
    computed = h.hexdigest()
    if set_sha is None:
        print("note: the manifest carries no '# set-sha256' line; the set cannot be "
              "checked for consistency")
    elif set_sha != computed:
        print(f"set-sha256 MISMATCH: manifest {set_sha[:16]}…, computed {computed[:16]}… — "
              "the set is inconsistent (half-written?), so no verdict is reported")
        return 2
    else:
        print(f"set-sha256 ok ({set_sha[:16]}…)")

    agree = 0
    disagreements = []
    for file, direction, mtype, expect, note in rows:
        path = os.path.join(args.dir, file)
        with open(path) as f:
            data = bytes.fromhex("".join(f.read().split()))
        got, why = verdict(data)
        if got == expect:
            agree += 1
            if args.verbose:
                print(f"  ok   {file}: {got}" + (f" ({why})" if why else ""))
        else:
            disagreements.append((file, expect, got, why, note))
            print(f"  DIFF {file}: manifest says {expect}, we say {got}"
                  + (f" — {why}" if why else ""))
            print(f"       note: {note}")
    print(f"\n{agree}/{len(rows)} agree; {len(disagreements)} disagreements")
    for req, wel in args.judge:
        def load(name):
            with open(os.path.join(args.dir, name)) as fh:
                version, _f, _t, rid, payload = parse_envelope(
                    bytes.fromhex("".join(fh.read().split())))
            return parse_payload(version, _t, rid, payload)
        print(f"  judge {req} under {wel}: {judge(load(wel), load(req))}")
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main())

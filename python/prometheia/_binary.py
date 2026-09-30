# SPDX-License-Identifier: GPL-2.0-or-later
"""The JSON tools, asked of prometheiad in protocol v4 (docs/PYTHON.md).

BinaryTransport takes the same calls as the stdio and HTTP transports --
positions, houses, convert_time, lookup, capabilities, with the JSON tools'
arguments -- asks them as v4 messages, and answers in the JSON tools' shape,
so prometheia.swe does not know which transport it is on.

What v4 does not carry is refused by name, never approximated:
- a clock time (UTC): v4 takes Julian dates in UT1, TT or TDB, and
  converting a clock time needs the leap-second table, which a time message
  agreed with Astrolog will carry (it follows houses);
- a bare name that is not a planet or a lunar point: v4 names objects by
  kind, so a star, an asteroid or a hypothetical is named as such.
"""
import itertools
import math
import threading

from . import _v4
from ._errors import Error
from ._transport import ToolError, USER_AGENT
from ._ws import WebSocket

NAIF = {"sun": 10, "moon": 301, "mercury": 199, "venus": 299, "earth": 399, "mars": 4,
        "jupiter": 5, "saturn": 6, "uranus": 7, "neptune": 8, "pluto": 9}
# The Moon's points by the names the JSON surface gives them: (point, method).
LUNAR = {
    "mean node": ("ascending-node", "mean"), "north node": ("ascending-node", "mean"),
    "true node": ("ascending-node", "osculating"),
    "south node": ("descending-node", "mean"),
    "true south node": ("descending-node", "osculating"),
    "lilith": ("aphelion", "mean"), "mean lilith": ("aphelion", "mean"),
    "mean apogee": ("aphelion", "mean"), "true lilith": ("aphelion", "osculating"),
    "osculating lilith": ("aphelion", "osculating"),
    "osculating apogee": ("aphelion", "osculating"),
    "mean perigee": ("perihelion", "mean"), "osculating perigee": ("perihelion", "osculating"),
    "natural apogee": ("aphelion", "interpolated"), "natural lilith": ("aphelion", "interpolated"),
    "natural perigee": ("perihelion", "interpolated"), "priapus": ("perihelion", "interpolated"),
}
HOUSE_TOKENS = ("placidus", "koch", "porphyry", "regiomontanus", "campanus", "equal",
                "whole-sign", "alcabitius", "morinus", "meridian", "topocentric")
HOUSE_LETTERS = "PKORCAWBMXT"
HOUSE_NAMES = ("Placidus", "Koch", "Porphyry", "Regiomontanus", "Campanus", "Equal",
               "Whole Sign", "Alcabitius", "Morinus", "Meridian", "Topocentric")


def _naif(body):
    if isinstance(body, int):
        return body
    n = NAIF.get(str(body).lower())
    if n is None:
        raise Error("'%s' is not a planet; the binary protocol names a body by its NAIF id"
                    % body)
    return n


def _object(item, profile):
    """A JSON tool object, as a v4 object dict."""
    if isinstance(item, str):
        name = item.lower()
        if name in NAIF:
            return {"kind": _v4.KIND_BODY, "naif": NAIF[name], "profile": profile}
        if name in LUNAR:
            point, method = LUNAR[name]
            return {"kind": _v4.KIND_ORBIT_POINT, "naif": 301, "point": _v4.POINTS[point],
                    "method": _v4.METHODS[method], "profile": profile}
        raise Error("'%s': on the binary protocol name a star, asteroid or hypothetical as "
                    "{\"star\": ...}, {\"asteroid\": ...} or {\"hypothetical\": ...}" % item)
    if "body" in item:
        return {"kind": _v4.KIND_BODY, "naif": _naif(item["body"]), "profile": profile}
    if "naif" in item:
        return {"kind": _v4.KIND_BODY, "naif": int(item["naif"]), "profile": profile}
    if "star" in item:
        return {"kind": _v4.KIND_STAR, "name": item["star"], "profile": profile}
    if "asteroid" in item:
        return {"kind": _v4.KIND_DESIGNATION, "name": str(item["asteroid"]), "profile": profile}
    if "hypothetical" in item:
        return {"kind": _v4.KIND_HYPOTHETICAL, "name": item["hypothetical"], "profile": profile}
    if "point" in item:
        return {"kind": _v4.KIND_ORBIT_POINT, "naif": _naif(item.get("of", "moon")),
                "point": _v4.POINTS[item["point"]],
                "method": _v4.METHODS[item.get("method", "mean")], "profile": profile}
    raise Error("an object is a name or {body|naif|star|asteroid|hypothetical|point: ...}")


def _instants(args):
    """(time scale, [JD]) from time / times / series."""
    def one(t):
        if isinstance(t, dict) and len(t) == 1:
            (k, v), = t.items()
            if k == "jd_ut1":
                return _v4.TIME_UT1, float(v)
            if k == "jd_tt":
                return _v4.TIME_TT, float(v)
        raise Error("the binary protocol takes {\"jd_ut1\": ...} or {\"jd_tt\": ...}; a clock "
                    "time needs the leap-second table, which v4 does not carry yet -- use the "
                    "stdio or HTTP transport for it")
    if "time" in args:
        scale, jd = one(args["time"])
        return scale, [jd]
    if "times" in args:
        pairs = [one(t) for t in args["times"]]
        if len({s for s, _ in pairs}) != 1:
            raise Error("one request's instants share one time scale")
        return pairs[0][0], [jd for _, jd in pairs]
    if "series" in args:
        s = args["series"]
        scale, start = one(s["start"])
        return scale, [start + k * float(s["step_days"]) for k in range(int(s["count"]))]
    raise Error("give time, times or series")


def _zodiac(args):
    z = args.get("zodiac", "tropical")
    if isinstance(z, dict) and "user" in z:
        return "user", (float(z["user"]["epoch_jd_tt"]), float(z["user"]["ayanamsa_deg"]))
    return ("" if z == "tropical" else str(z)), (0.0, 0.0)


def _corrections(c):
    if c is None or c == "apparent":
        return 7
    if c == "astrometric":
        return _v4.CORR_LIGHT_TIME
    if c == "geometric":
        return 0
    bits = {"light-time": _v4.CORR_LIGHT_TIME, "gravitational-deflection": _v4.CORR_DEFLECTION,
            "deflection": _v4.CORR_DEFLECTION, "aberration": _v4.CORR_ABERRATION}
    return sum(bits[x] for x in c)


class BinaryTransport:
    """A prometheiad over protocol v4 (ws:// or wss://), one connection."""

    def __init__(self, url, token=None, timeout=60.0):
        self.url = url
        self._token = token
        self._timeout = timeout
        self._ws = None
        self._welcome = None
        self._ids = itertools.count(1)
        self._lock = threading.Lock()

    # ---- the connection ----

    def _open(self):
        ws = WebSocket(self.url, timeout=self._timeout)
        ws.connect()
        ws.send(_v4.envelope(_v4.MSG_HELLO, 0, _v4.hello(USER_AGENT, self._token)))
        mtype, _, payload = _v4.open_envelope(ws.recv())
        if mtype == _v4.MSG_ERROR:
            ws.close()
            raise ToolError("%s refused the session: %s" % (self.url, _v4.error(payload)[1]))
        if mtype != _v4.MSG_WELCOME:
            ws.close()
            raise Error("%s answered HELLO with message type %d" % (self.url, mtype))
        self._welcome = _v4.welcome(payload)
        self._ws = ws

    def _exchange(self, mtype, payload):
        """Send one request-scoped message; the replies to it, until the last."""
        with self._lock:
            if self._ws is None:
                self._open()
            rid = next(self._ids)
            self._ws.send(_v4.envelope(mtype, rid, payload))
            replies = []
            while True:
                t, r, body = _v4.open_envelope(self._ws.recv())
                if t == _v4.MSG_PING:
                    self._ws.send(_v4.envelope(_v4.MSG_PONG, 0, body))
                    continue
                if r != rid and t != _v4.MSG_ERROR:
                    continue
                if t == _v4.MSG_ERROR:
                    code, text = _v4.error(body)
                    raise ToolError("the server refused the request (ERROR %d): %s"
                                    % (code, text), code=code)
                replies.append((t, body))
                if t == _v4.MSG_LOOKUP_RESULT:
                    return replies
                if t == _v4.MSG_DATA and _v4.data(body)["flags"] & _v4.CHUNK_LAST:
                    return replies

    def _samples(self, scale, instants, profiles, objects, precession=None):
        """(meta, rows per object): rows are lists of column values, None when failed."""
        payload = _v4.request(scale, instants, profiles, objects, precession)
        chunks = [_v4.data(b) for t, b in self._exchange(_v4.MSG_REQUEST, payload)]
        first = chunks[0]
        n_obj, n_cols, total = first["n_obj"], first["n_cols"], first["total_rows"]
        rows = [[None] * total for _ in range(n_obj)]
        for c in chunks:
            for o in range(n_obj):
                for r in range(c["n_rows"]):
                    at = (o * c["n_rows"] + r) * n_cols
                    vals = c["values"][at:at + n_cols]
                    rows[o][c["i_time"] + r] = None if math.isnan(vals[0]) else vals
        return first["meta"], rows, first["columns"]

    @staticmethod
    def _column(columns, bit):
        """A present extra column's index in a row."""
        return 6 + bin(columns & (bit - 1)).count("1")

    # ---- the tools ----

    def call(self, tool, arguments):
        if tool == "positions":
            return self._positions(arguments)
        if tool == "houses":
            return self._houses(arguments)
        if tool == "convert_time":
            return self._convert_time(arguments)
        if tool == "lookup":
            return self._lookup(arguments)
        if tool == "capabilities":
            with self._lock:
                if self._ws is None:
                    self._open()
            return dict(self._welcome)
        raise ToolError("the binary protocol has no tool '%s'" % tool)

    def _positions(self, a):
        scale, instants = _instants(a)
        zodiac, anchor = _zodiac(a)
        observer = {"geocentric": _v4.OBS_GEO, "topocentric": _v4.OBS_TOPO,
                    "heliocentric": _v4.OBS_HELIO, "barycentric": _v4.OBS_BARY,
                    "body": _v4.OBS_BODY}[a.get("observer", "geocentric")]
        site = (0.0, 0.0, 0.0)
        if observer == _v4.OBS_TOPO:
            s = a["site"]
            site = (float(s["lon_deg"]), float(s["lat_deg"]), float(s.get("height_m", 0.0)))
        columns = _v4.COL_DELTA_T | (_v4.COL_AYANAMSA if zodiac else 0)
        rates = a.get("rates", True)
        corrections = _corrections(a.get("corrections"))
        if observer == _v4.OBS_HELIO:
            # Light is not deflected at the Sun's centre: v4 refuses the bit
            # there, where the JSON tools leave it out and say so.
            corrections &= ~_v4.CORR_DEFLECTION
        prof = _v4.profile(
            observer=observer,
            plane=_v4.PLANE_EQUATOR if a.get("coordinates") == "equatorial" else _v4.PLANE_ECLIPTIC,
            frame=_v4.FRAMES[a.get("frame", "true-of-date")],
            corrections=corrections, speeds=rates,
            sidereal_plane=_v4.SIDEREAL_PLANES[a.get("sidereal_plane", "date")],
            observer_body=_naif(a["center"]) if observer == _v4.OBS_BODY else 0,
            site=site, anchor=anchor, columns=columns, zodiac=zodiac)
        objects = [_object(item, 0) for item in a["objects"]]
        meta, rows, cols = self._samples(scale, instants, [prof], objects, a.get("precession"))
        ecl = a.get("coordinates", "ecliptic") != "equatorial"
        results = []
        for item, m, obj_rows in zip(a["objects"], meta, rows):
            res = {"object": {"asked": item, "resolved": m["name"]}, "error": None}
            out_rows = []
            for jd, v in zip(instants, obj_rows):
                if v is None:
                    break
                no_dist = bool(m["flags"] & _v4.META_NO_DISTANCE)
                row = {"time": {("jd_ut1" if scale == _v4.TIME_UT1 else "jd_tt"): jd,
                                "delta_t_s": v[self._column(cols, _v4.COL_DELTA_T)]},
                       ("longitude_deg" if ecl else "right_ascension_deg"): v[0],
                       ("latitude_deg" if ecl else "declination_deg"): v[1],
                       "distance_au": None if no_dist else v[2]}
                if rates:
                    row["rates"] = {
                        ("longitude_deg_per_day" if ecl else "right_ascension_deg_per_day"): v[3],
                        ("latitude_deg_per_day" if ecl else "declination_deg_per_day"): v[4],
                        "distance_au_per_day": None if no_dist else v[5]}
                if zodiac:
                    row["ayanamsa_deg"] = v[self._column(cols, _v4.COL_AYANAMSA)]
                out_rows.append(row)
            if m["err_code"]:
                res["error"] = {"code": _v4.OBJECT_ERRORS.get(m["err_code"], "internal"),
                                "message": m["err_text"]}
            if out_rows:
                res["rows"] = out_rows
            results.append(res)
        return {"engine": self._welcome["engine"], "dataset": self._welcome["dataset"],
                "results": results}

    def _houses(self, a):
        scale, instants = _instants(a)
        zodiac, anchor = _zodiac(a)
        if a.get("sidereal_plane", "date") != "date":
            raise ToolError("houses are counted on the ecliptic of date; sidereal_plane is date")
        s = a["site"]
        columns = _v4.COL_ARMC | _v4.COL_OBLIQUITY | (_v4.COL_AYANAMSA if zodiac else 0)
        prof = _v4.profile(observer=_v4.OBS_TOPO, corrections=0, speeds=False,
                           site=(float(s["lon_deg"]), float(s["lat_deg"]), 0.0), anchor=anchor,
                           columns=columns, zodiac=zodiac)
        systems = []
        for asked in a.get("systems", ["placidus"]):
            w = str(asked)
            if len(w) == 1 and w in HOUSE_LETTERS:
                systems.append((asked, HOUSE_LETTERS.index(w)))
            elif w.lower() in HOUSE_TOKENS:
                systems.append((asked, HOUSE_TOKENS.index(w.lower())))
            else:
                systems.append((asked, None))
        objects = [{"kind": _v4.KIND_HOUSE, "system": sid, "point": p, "profile": 0}
                   for _, sid in systems if sid is not None for p in range(1, 17)]
        meta, rows, cols = (self._samples(scale, instants, [prof], objects, a.get("precession"))
                            if objects else ([], [], 0))
        results = []
        k = 0
        for asked, sid in systems:
            if sid is None:
                results.append({"system": {"asked": asked}, "error": {
                    "code": "unsupported", "message": "that house system is not served"}})
                continue
            m, r = meta[k:k + 16], rows[k:k + 16]
            k += 16
            res = {"system": {"asked": asked, "token": HOUSE_TOKENS[sid],
                              "name": HOUSE_NAMES[sid]}, "error": None}
            failed = next((x for x in m[:12] if x["err_code"]), None)
            out_rows = []
            for i, jd in enumerate(instants):
                if any(r[p][i] is None for p in range(16)):
                    break
                asc = r[12][i]  # every point's row carries the ARMC and obliquity used
                row = {"time": {("jd_ut1" if scale == _v4.TIME_UT1 else "jd_tt"): jd},
                       "cusps_deg": [r[p][i][0] for p in range(12)],
                       "ascendant_deg": asc[0], "mc_deg": r[13][i][0],
                       "vertex_deg": r[14][i][0], "equatorial_ascendant_deg": r[15][i][0],
                       "armc_deg": asc[self._column(cols, _v4.COL_ARMC)],
                       "obliquity_deg": asc[self._column(cols, _v4.COL_OBLIQUITY)]}
                if zodiac:
                    row["ayanamsa_deg"] = asc[self._column(cols, _v4.COL_AYANAMSA)]
                out_rows.append(row)
            if failed:
                res["error"] = {"code": _v4.OBJECT_ERRORS.get(failed["err_code"], "internal"),
                                "message": failed["err_text"]}
            if out_rows:
                res["rows"] = out_rows
            results.append(res)
        return {"engine": self._welcome["engine"], "dataset": self._welcome["dataset"],
                "results": results}

    def _convert_time(self, a):
        # The delta T column of a row gives TT - UT1; the clock (UTC) has no
        # v4 form yet.
        scale, instants = _instants({"time": a.get("time")})
        prof = _v4.profile(speeds=False, columns=_v4.COL_DELTA_T)
        meta, rows, cols = self._samples(scale, instants, [prof],
                                         [{"kind": _v4.KIND_BODY, "naif": 10, "profile": 0}])
        if rows[0][0] is None:
            raise ToolError(meta[0]["err_text"] or "the server could not answer that instant")
        dt = rows[0][0][self._column(cols, _v4.COL_DELTA_T)]
        jd = instants[0]
        tt = jd + dt / 86400.0 if scale == _v4.TIME_UT1 else jd
        return {"jd_tt": tt, "jd_ut1": tt - dt / 86400.0 if scale != _v4.TIME_UT1 else jd,
                "delta_t_s": dt}

    def _lookup(self, a):
        body = _v4.lookup([str(a["query"])], prefix=bool(a.get("prefix")))
        (_, payload), = self._exchange(_v4.MSG_LOOKUP, body)
        matches = _v4.lookup_result(payload)[0]
        return {"query": a["query"],
                "matches": [{"name": m["name"] or m["designation"], "object": m["object"],
                             "designation": m["designation"]} for m in matches]}

    def close(self):
        with self._lock:
            if self._ws is not None:
                self._ws.close()
                self._ws = None

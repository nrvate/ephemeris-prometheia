# SPDX-License-Identifier: GPL-2.0-or-later
"""The Python v4 codec (prometheia._v4) without a server.

What it encodes is judged by our independent reader of protocol v4
(tools/check/ephproto4_fixtures.py, taught from section 3's text); what it
decodes is Astrolog's conformance messages, when their tree is named by
$PROMETHEIA_ASTROLOG."""
import os
import sys
import unittest

from prometheia import _v4

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools", "check"))
import ephproto4_fixtures as reader  # noqa: E402


def judged(mtype, rid, payload):
    return reader.verdict(_v4.envelope(mtype, rid, payload))


class Encode(unittest.TestCase):
    def test_hello(self):
        self.assertEqual(judged(_v4.MSG_HELLO, 0, _v4.hello("prometheia-python", "t0k")),
                         ("ok", ""))

    def test_requests_every_kind(self):
        topo = _v4.profile(observer=_v4.OBS_TOPO, site=(8.55, 47.37, 500.0), corrections=0,
                           columns=_v4.COL_ARMC | _v4.COL_OBLIQUITY | _v4.COL_AYANAMSA,
                           zodiac="lahiri")
        geo = _v4.profile(frame=_v4.FRAMES["j2000"], plane=_v4.PLANE_EQUATOR, speeds=False,
                          columns=_v4.COL_DELTA_T)
        user = _v4.profile(zodiac="user", anchor=(2415020.5, 22.46), sidereal_plane=1)
        objects = [
            {"kind": _v4.KIND_BODY, "naif": 4, "profile": 1},
            {"kind": _v4.KIND_ORBIT_POINT, "naif": 301, "point": 3, "method": 2, "profile": 1},
            {"kind": _v4.KIND_STAR, "name": "Spica", "profile": 1},
            {"kind": _v4.KIND_HYPOTHETICAL, "name": "cupido", "profile": 1},
            {"kind": _v4.KIND_DESIGNATION, "name": "C", "profile": 2},
            {"kind": _v4.KIND_HOUSE, "system": 0, "point": 11, "profile": 0},
            {"kind": _v4.KIND_HOUSE, "system": 10, "point": 16, "profile": 0},
        ]
        for scale in (_v4.TIME_UT1, _v4.TIME_TT, _v4.TIME_TDB):
            payload = _v4.request(scale, [2451545.0, 2451545.25, 2460000.123456789],
                                  [topo, geo, user], objects, precession="vondrak2011")
            v = judged(_v4.MSG_REQUEST, 7, payload)
            self.assertEqual(v[0], "ok", v)

    def test_lookup(self):
        self.assertEqual(judged(_v4.MSG_LOOKUP, 3, _v4.lookup(["Ceres", "433"], 16, True))[0],
                         "ok")

    def test_two_part_time_keeps_the_fraction(self):
        r = _v4.Reader(_v4.request(1, [2460000.123456789], [_v4.profile()], [
            {"kind": 0, "naif": 10}])[16 + 4 + 4:16 + 4 + 4 + 16])
        self.assertEqual(r.time(), 2460000.123456789)

    def test_a_non_finite_number_is_refused(self):
        with self.assertRaises(Exception):
            _v4.request(1, [float("nan")], [_v4.profile()], [{"kind": 0, "naif": 10}])


ASTROLOG = os.environ.get("PROMETHEIA_ASTROLOG")


@unittest.skipUnless(ASTROLOG and os.path.isdir(os.path.join(ASTROLOG or "", "ephsrv",
                                                             "conformance")),
                     "needs $PROMETHEIA_ASTROLOG with ephsrv/conformance")
class Decode(unittest.TestCase):
    def fixture(self, name):
        with open(os.path.join(ASTROLOG, "ephsrv", "conformance", name + ".hex")) as f:
            return bytes.fromhex("".join(f.read().split()))

    def test_welcome_houses(self):
        mtype, _, payload = _v4.open_envelope(self.fixture("welcome_houses"))
        self.assertEqual(mtype, _v4.MSG_WELCOME)
        w = _v4.welcome(payload)
        self.assertTrue(w["kinds"] & (1 << _v4.KIND_HOUSE))
        self.assertTrue(w["house_systems"])
        self.assertTrue(w["sidereal_time"])

    def test_data_houses(self):
        mtype, _, payload = _v4.open_envelope(self.fixture("data_houses"))
        self.assertEqual(mtype, _v4.MSG_DATA)
        d = _v4.data(payload)
        self.assertEqual(d["columns"] & (_v4.COL_ARMC | _v4.COL_OBLIQUITY),
                         _v4.COL_ARMC | _v4.COL_OBLIQUITY)
        self.assertTrue(any(m["err_code"] == 9 for m in d["meta"]))
        self.assertEqual(len(d["values"]), d["n_obj"] * d["n_rows"] * d["n_cols"])


if __name__ == "__main__":
    unittest.main()

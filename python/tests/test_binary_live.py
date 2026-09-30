# SPDX-License-Identifier: GPL-2.0-or-later
"""prometheia.swe over protocol v4 against a real prometheiad, held to the
stdio transport's answers on the same DE440: the two must be identical,
since the same engine answers both.

Skips without build/prometheiad, build/prometheia-json and
ephe/linux_p1550p2650.440. About a second."""
import math
import os
import socket
import subprocess
import time
import unittest

import prometheia
from prometheia import swe

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DAEMON = os.path.join(REPO, "build", "prometheiad")
JSON = os.environ.get("PROMETHEIA_JSON", os.path.join(REPO, "build", "prometheia-json"))
DE440 = os.environ.get("PROMETHEIA_DE440", os.path.join(REPO, "ephe", "linux_p1550p2650.440"))


def same(a, b):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, float) and math.isnan(a):
        return isinstance(b, float) and math.isnan(b)
    return a == b


@unittest.skipUnless(all(os.path.exists(p) for p in (DAEMON, JSON, DE440)),
                     "needs build/prometheiad, build/prometheia-json and DE440")
class Binary(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        cls.daemon = subprocess.Popen(
            [DAEMON, "--ephemeris", DE440, "--bind", "127.0.0.1", "--port", str(port),
             "--threads", "1", "--log-level", "quiet"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 20
        while True:
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                if time.monotonic() > deadline or cls.daemon.poll() is not None:
                    cls.daemon.kill()
                    cls.daemon.wait()
                    raise
                time.sleep(0.05)
        cls.binary = prometheia.Client(server="ws://127.0.0.1:%d" % port)
        cls.stdio = prometheia.Client(ephemeris=DE440, executable=JSON)

    @classmethod
    def tearDownClass(cls):
        prometheia._default = None
        cls.binary.close()
        cls.stdio.close()
        cls.daemon.terminate()
        cls.daemon.wait()

    def both(self, fn):
        prometheia._default = self.binary
        a = fn()
        prometheia._default = self.stdio
        b = fn()
        prometheia._default = None
        return a, b

    def test_identical_to_stdio(self):
        swe.set_topo(8.55, 47.37, 500.0)
        swe.set_sid_mode(swe.SIDM_LAHIRI)
        calls = [lambda b=b: swe.calc_ut(2451545.0, b) for b in
                 (swe.SUN, swe.MOON, swe.MERCURY, swe.MARS, swe.PLUTO, swe.MEAN_NODE,
                  swe.TRUE_NODE, swe.MEAN_APOG, swe.OSCU_APOG, swe.INTP_APOG, swe.INTP_PERG)]
        calls += [
            lambda: swe.calc(2461300.5, swe.VENUS, swe.FLG_SPEED | swe.FLG_EQUATORIAL
                             | swe.FLG_J2000),
            lambda: swe.calc_ut(2451545.0, swe.MOON, swe.FLG_SPEED | swe.FLG_TOPOCTR),
            lambda: swe.calc_ut(2451545.0, swe.MARS, swe.FLG_SPEED | swe.FLG_HELCTR),
            lambda: swe.calc_ut(2451545.0, swe.JUPITER, swe.FLG_SPEED | swe.FLG_SIDEREAL),
            lambda: swe.calc_ut(2451545.0, swe.SATURN, swe.FLG_TRUEPOS | swe.FLG_NOABERR
                                | swe.FLG_NOGDEFL | swe.FLG_XYZ),
            lambda: swe.get_ayanamsa_ut(2451545.0),
            lambda: swe.get_ayanamsa_ex_ut(2451545.0, 0),
            lambda: swe.deltat(2451545.0),
            lambda: swe.sidtime(2451545.0),
            lambda: swe.fixstar2_ut("Spica", 2451545.0),
            lambda: swe.nod_aps_ut(2451545.0, swe.MARS, swe.NODBIT_MEAN),
        ]
        calls += [lambda h=h: swe.houses(2451545.0, 47.37, 8.55, h.encode())
                  for h in "PKORCAWBMXT"]
        calls += [lambda: swe.houses_ex(2451545.0, -33.9, 18.4, b"W", swe.FLG_SIDEREAL),
                  lambda: swe.houses(2451545.0, 70.0, 0.0, b"R")]
        for fn in calls:
            a, b = self.both(fn)
            self.assertTrue(same(a, b), (a, b))

    def test_refusals(self):
        prometheia._default = self.binary
        with self.assertRaises(swe.Error) as e:
            swe.houses(2451545.0, 70.0, 0.0, b"P")
        self.assertEqual(e.exception.code, "undefined-at-latitude")
        self.assertIn("polar circle", str(e.exception))
        with self.assertRaises(swe.Error) as e:
            swe.utc_to_jd(2000, 1, 1, 12, 0, 0.0)  # a clock time: not in v4 yet
        self.assertIn("leap-second", str(e.exception))
        with self.assertRaises(swe.Error):
            swe.calc_ut(2451545.0, swe.CHIRON)  # no catalog on this server
        prometheia._default = None

    def test_capabilities(self):
        w = self.binary.capabilities()
        self.assertTrue(w["kinds"] & (1 << 6))
        self.assertEqual(w["house_systems"], list(range(11)))
        self.assertEqual(w["sidereal_time"], "iau2006-2000a")


if __name__ == "__main__":
    unittest.main()

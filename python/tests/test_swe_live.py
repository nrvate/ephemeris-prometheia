# SPDX-License-Identifier: GPL-2.0-or-later
"""prometheia.swe against a real prometheia-json over stdio, on DE440.

Skips when build/prometheia-json or ephe/linux_p1550p2650.440 is absent (or
PROMETHEIA_JSON / PROMETHEIA_DE440 name others). About half a second."""
import math
import os
import unittest

import prometheia
from prometheia import swe

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
EXE = os.environ.get("PROMETHEIA_JSON", os.path.join(REPO, "build", "prometheia-json"))
DE440 = os.environ.get("PROMETHEIA_DE440", os.path.join(REPO, "ephe", "linux_p1550p2650.440"))


@unittest.skipUnless(os.path.exists(EXE) and os.path.exists(DE440),
                     "needs build/prometheia-json and ephe/linux_p1550p2650.440")
class Live(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ["PROMETHEIA_JSON"] = EXE
        swe.set_ephe_path(DE440)

    @classmethod
    def tearDownClass(cls):
        swe.close()

    def test_calc_ut_is_calc_at_ut1_plus_delta_t(self):
        jd = 2451545.0
        a, ret = swe.calc_ut(jd, swe.MARS)
        b, _ = swe.calc(jd + swe.deltat(jd), swe.MARS)
        self.assertEqual(ret, swe.FLG_JPLEPH | swe.FLG_SPEED)
        for x, y in zip(a, b):
            self.assertAlmostEqual(x, y, places=9)

    def test_sidereal_is_tropical_less_the_true_ayanamsa(self):
        jd = 2451545.0
        swe.set_sid_mode(swe.SIDM_LAHIRI)
        trop, _ = swe.calc_ut(jd, swe.SUN)
        sid, _ = swe.calc_ut(jd, swe.SUN, swe.FLG_SWIEPH | swe.FLG_SPEED | swe.FLG_SIDEREAL)
        _, aya = swe.get_ayanamsa_ex_ut(jd, 0)
        self.assertAlmostEqual(swe.difdeg2n(trop[0] - aya, sid[0]), 0.0, places=9)
        self.assertLess(abs(swe.get_ayanamsa_ut(jd) - aya), 20.0 / 3600)  # mean vs true: nutation

    def test_houses(self):
        cusps, ascmc = swe.houses(2451545.0, 47.37, 8.55, b"P")
        self.assertEqual(cusps[0], ascmc[swe.ASC])
        self.assertEqual(cusps[9], ascmc[swe.MC])
        self.assertTrue(math.isnan(ascmc[swe.COASC1]))
        with self.assertRaises(swe.Error) as e:
            swe.houses(2451545.0, 70.0, 0.0, b"P")
        self.assertIn("polar circle", str(e.exception))
        cusps, _ = swe.houses(2451545.0, 70.0, 0.0, b"O")  # Porphyry answers there
        self.assertEqual(len(cusps), 12)

    def test_time_round_trip(self):
        jd_tt, jd_ut = swe.utc_to_jd(2000, 1, 1, 12, 0, 0.0)
        self.assertAlmostEqual((jd_tt - jd_ut) * 86400, swe.deltat(jd_ut) * 86400, places=3)
        y, m, d, h, mi, s = swe.jdut1_to_utc(jd_ut)
        self.assertEqual((y, m, d, h, mi), (2000, 1, 1, 12, 0))
        self.assertAlmostEqual(s, 0.0, places=2)

    def test_errors_carry_the_servers_sentence(self):
        with self.assertRaises(swe.Error) as e:
            swe.calc_ut(2451545.0, swe.CHIRON)  # no catalog given
        self.assertIn("catalog", str(e.exception))

    def test_native_client_batch(self):
        out = prometheia.default_client().positions(
            time={"jd_ut1": 2451545.0}, objects=["Sun", "Moon", "Mars"])
        self.assertEqual([r["object"]["resolved"] for r in out["results"]], ["Sun", "Moon", "Mars"])


if __name__ == "__main__":
    unittest.main()

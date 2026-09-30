# SPDX-License-Identifier: GPL-2.0-or-later
"""prometheia.swe against a scripted client: what each flag asks the server,
and how an answer becomes pyswisseph's shape. No server, no ephemeris."""
import math
import unittest

import prometheia
from prometheia import swe


class Scripted:
    """Records each call and answers with a canned JSON reply."""

    def __init__(self, reply):
        self.reply = reply
        self.calls = []

    def _call(self, tool, arguments):
        self.calls.append((tool, arguments))
        return self.reply(tool, arguments) if callable(self.reply) else self.reply

    def positions(self, **a):
        return self._call("positions", a)

    def houses(self, **a):
        return self._call("houses", a)

    def convert_time(self, **a):
        return self._call("convert_time", a)

    def lookup(self, **a):
        return self._call("lookup", a)

    def close(self):
        pass


def row(lon=10.0, lat=1.0, dist=2.0, rates=(0.5, 0.01, 0.001), ecl=True):
    r = {"time": {"jd_tt": 2451545.0}, "distance_au": dist}
    r["longitude_deg" if ecl else "right_ascension_deg"] = lon
    r["latitude_deg" if ecl else "declination_deg"] = lat
    if rates:
        r["rates"] = {("longitude_deg_per_day" if ecl else "right_ascension_deg_per_day"): rates[0],
                      ("latitude_deg_per_day" if ecl else "declination_deg_per_day"): rates[1],
                      "distance_au_per_day": rates[2]}
    return r


def answer(r=None, error=None):
    res = {"object": {"resolved": "Spica"}, "error": error}
    if r is not None:
        res["rows"] = [r]
    return {"results": [res]}


class Base(unittest.TestCase):
    def use(self, reply):
        self.fake = Scripted(reply)
        prometheia._default = self.fake
        swe._state.update(sid_mode=swe.SIDM_FAGAN_BRADLEY, t0=0.0, ayan_t0=0.0, topo=None)

    def tearDown(self):
        prometheia._default = None

    def last(self):
        return self.fake.calls[-1][1]


class Positions(Base):
    def test_default_is_apparent_geocentric_with_rates(self):
        self.use(answer(row()))
        xx, ret = swe.calc_ut(2451545.0, swe.MARS)
        self.assertEqual(xx, (10.0, 1.0, 2.0, 0.5, 0.01, 0.001))
        self.assertEqual(ret, swe.FLG_JPLEPH | swe.FLG_SPEED)
        a = self.last()
        self.assertEqual(a["time"], {"jd_ut1": 2451545.0})
        self.assertEqual(a["objects"], ["Mars"])
        self.assertEqual(a["corrections"], ["light-time", "gravitational-deflection", "aberration"])
        self.assertTrue(a["rates"])
        self.assertNotIn("observer", a)
        self.assertNotIn("frame", a)

    def test_calc_is_tt(self):
        self.use(answer(row()))
        swe.calc(2451545.0, swe.SUN, 0)
        self.assertEqual(self.last()["time"], {"jd_tt": 2451545.0})

    def test_without_speed_the_rates_are_zero(self):
        self.use(answer(row(rates=None)))
        xx, ret = swe.calc_ut(2451545.0, swe.SUN, swe.FLG_SWIEPH)
        self.assertEqual(xx[3:], (0.0, 0.0, 0.0))
        self.assertFalse(self.last()["rates"])
        self.assertEqual(ret, swe.FLG_JPLEPH)  # the ephemeris answering is JPL's

    def test_flags_map_to_the_request(self):
        self.use(answer(row(ecl=False)))
        swe.calc_ut(0.0, swe.SUN, swe.FLG_EQUATORIAL | swe.FLG_J2000 | swe.FLG_HELCTR
                    | swe.FLG_TRUEPOS | swe.FLG_NOABERR | swe.FLG_NOGDEFL)
        a = self.last()
        self.assertEqual(a["coordinates"], "equatorial")
        self.assertEqual(a["frame"], "j2000")
        self.assertEqual(a["observer"], "heliocentric")
        self.assertEqual(a["corrections"], [])
        swe.calc_ut(0.0, swe.SUN, swe.FLG_NONUT | swe.FLG_BARYCTR | swe.FLG_ASTROMETRIC)
        a = self.last()
        self.assertEqual((a["frame"], a["observer"], a["corrections"]),
                         ("mean-of-date", "barycentric", ["light-time"]))
        swe.calc_ut(0.0, swe.SUN, swe.FLG_ICRS | swe.FLG_J2000)
        self.assertEqual(self.last()["frame"], "icrf")

    def test_topocentric_needs_set_topo(self):
        self.use(answer(row()))
        with self.assertRaises(swe.Error):
            swe.calc_ut(0.0, swe.MOON, swe.FLG_TOPOCTR)
        swe.set_topo(8.55, 47.37, 500)
        swe.calc_ut(0.0, swe.MOON, swe.FLG_TOPOCTR)
        self.assertEqual(self.last()["site"], {"lon_deg": 8.55, "lat_deg": 47.37, "height_m": 500.0})

    def test_unserved_flags_raise(self):
        self.use(answer(row()))
        for flag in (swe.FLG_CENTER_BODY, swe.FLG_JPLHOR, swe.FLG_JPLHOR_APPROX):
            with self.assertRaises(swe.Error):
                swe.calc_ut(0.0, swe.MARS, flag)
        self.assertEqual(self.fake.calls, [])

    def test_bodies(self):
        self.use(answer(row()))
        expect = {swe.MEAN_NODE: "mean node", swe.TRUE_NODE: "true node", swe.MEAN_APOG: "lilith",
                  swe.OSCU_APOG: "true lilith", swe.INTP_APOG: "natural apogee",
                  swe.INTP_PERG: "natural perigee", swe.CHIRON: {"asteroid": "2060"},
                  swe.CERES: {"asteroid": "1"}, swe.AST_OFFSET + 433: {"asteroid": "433"},
                  swe.CUPIDO: {"hypothetical": "cupido"},
                  swe.NEPTUNE_LEVERRIER: {"hypothetical": "neptune-leverrier"}}
        for body, obj in expect.items():
            swe.calc_ut(0.0, body)
            self.assertEqual(self.last()["objects"], [obj], body)
        for body in (swe.ECL_NUT, 999, -3):
            with self.assertRaises(swe.Error):
                swe.calc_ut(0.0, body)

    def test_a_refused_object_raises_with_the_servers_sentence(self):
        self.use(answer(error={"code": "unknown-name", "message": "no loaded catalog answers '2060'"}))
        with self.assertRaises(swe.Error) as e:
            swe.calc_ut(0.0, swe.CHIRON)
        self.assertIn("2060", str(e.exception))
        self.assertEqual(e.exception.code, "unknown-name")

    def test_xyz_and_radians(self):
        self.use(answer(row(lon=90.0, lat=0.0, dist=2.0, rates=(1.0, 0.0, 0.1))))
        xx, _ = swe.calc_ut(0.0, swe.MARS, swe.FLG_SPEED | swe.FLG_XYZ)
        self.assertAlmostEqual(xx[0], 0.0)
        self.assertAlmostEqual(xx[1], 2.0)
        self.assertAlmostEqual(xx[2], 0.0)
        self.assertAlmostEqual(xx[3], -2.0 * math.radians(1.0))  # moving east at 90 deg: -x
        self.assertAlmostEqual(xx[4], 0.1)
        xx, _ = swe.calc_ut(0.0, swe.MARS, swe.FLG_SPEED | swe.FLG_RADIANS)
        self.assertAlmostEqual(xx[0], math.pi / 2)
        self.assertAlmostEqual(xx[3], math.radians(1.0))

    def test_a_missing_distance_is_nan_not_zero(self):
        self.use(answer(row(dist=None)))
        xx, name, _ = swe.fixstar2_ut("Spica", 0.0)
        self.assertTrue(math.isnan(xx[2]))
        self.assertEqual(name, "Spica")


class Sidereal(Base):
    def test_modes_are_tokens(self):
        self.use(answer(row()))
        swe.calc_ut(0.0, swe.SUN, swe.FLG_SIDEREAL)  # never set: Fagan/Bradley
        self.assertEqual((self.last()["zodiac"], self.last()["sidereal_plane"]),
                         ("fagan-bradley", "date"))
        swe.set_sid_mode(swe.SIDM_LAHIRI | swe.SIDBIT_ECL_T0)
        swe.calc_ut(0.0, swe.SUN, swe.FLG_SIDEREAL)
        self.assertEqual((self.last()["zodiac"], self.last()["sidereal_plane"]), ("lahiri", "anchor"))
        swe.set_sid_mode(swe.SIDM_TRUE_CITRA | swe.SIDBIT_SSY_PLANE)
        swe.calc_ut(0.0, swe.SUN, swe.FLG_SIDEREAL)
        self.assertEqual((self.last()["zodiac"], self.last()["sidereal_plane"]),
                         ("true-citra", "invariable"))
        swe.set_sid_mode(swe.SIDM_USER, 2415020.5, 22.5)
        swe.calc_ut(0.0, swe.SUN, swe.FLG_SIDEREAL)
        self.assertEqual(self.last()["zodiac"], {"user": {"epoch_jd_tt": 2415020.5,
                                                          "ayanamsa_deg": 22.5}})
        for mode in (swe.SIDM_LAHIRI | swe.SIDBIT_ECL_DATE, 99):
            swe.set_sid_mode(mode)
            with self.assertRaises(swe.Error):
                swe.calc_ut(0.0, swe.SUN, swe.FLG_SIDEREAL)

    def test_ayanamsa_mean_and_true(self):
        r = row()
        r["ayanamsa_deg"] = 23.85
        self.use(answer(r))
        swe.set_sid_mode(swe.SIDM_LAHIRI)
        self.assertEqual(swe.get_ayanamsa_ut(2451545.0), 23.85)
        self.assertEqual(self.last()["frame"], "mean-of-date")
        self.assertEqual(swe.get_ayanamsa_ex_ut(2451545.0, swe.FLG_SWIEPH), (swe.FLG_JPLEPH, 23.85))
        self.assertEqual(self.last()["frame"], "true-of-date")
        swe.get_ayanamsa_ex_ut(2451545.0, swe.FLG_NONUT)
        self.assertEqual(self.last()["frame"], "mean-of-date")


class Houses(Base):
    def reply(self, tool, a):
        return {"results": [{"error": None, "rows": [{
            "cusps_deg": [float(k) for k in range(12)], "ascendant_deg": 1.5, "mc_deg": 2.5,
            "armc_deg": 45.0, "vertex_deg": 3.5, "equatorial_ascendant_deg": 4.5}]}]}

    def test_shape_and_the_unserved_angles(self):
        self.use(self.reply)
        cusps, ascmc = swe.houses(2451545.0, 47.37, 8.55, b"K")
        self.assertEqual(len(cusps), 12)
        self.assertEqual(ascmc[:5], (1.5, 2.5, 45.0, 3.5, 4.5))
        self.assertTrue(all(math.isnan(x) for x in ascmc[5:]))  # not served: NaN, never 0
        a = self.last()
        self.assertEqual(a["systems"], ["K"])
        self.assertEqual(a["site"], {"lon_deg": 8.55, "lat_deg": 47.37})  # houses() is (lat, lon)
        self.assertEqual(a["time"], {"jd_ut1": 2451545.0})
        swe.houses(0.0, 0.0, 0.0, "W")
        self.assertEqual(self.last()["systems"], ["W"])
        self.assertAlmostEqual(swe.sidtime(0.0), 3.0)

    def test_sidereal_houses(self):
        self.use(self.reply)
        swe.set_sid_mode(swe.SIDM_LAHIRI)
        swe.houses_ex(0.0, 10.0, 20.0, b"P", swe.FLG_SIDEREAL)
        self.assertEqual(self.last()["zodiac"], "lahiri")
        swe.set_sid_mode(swe.SIDM_LAHIRI | swe.SIDBIT_SSY_PLANE)
        with self.assertRaises(swe.Error):
            swe.houses_ex(0.0, 10.0, 20.0, b"P", swe.FLG_SIDEREAL)

    def test_names(self):
        self.assertEqual(swe.house_name(b"P"), "Placidus")
        self.assertEqual(swe.house_name("W"), "Whole Sign")
        self.assertEqual(swe.house_name(b"G"), "")


class Time(Base):
    def test_utc_to_jd(self):
        self.use({"jd_tt": 2.0, "jd_ut1": 1.0})
        self.assertEqual(swe.utc_to_jd(2016, 12, 31, 23, 59, 60.25), (2.0, 1.0))
        self.assertEqual(self.last()["time"], "2016-12-31T23:59:60.25Z")
        swe.utc_to_jd(1582, 10, 4, 0, 0, 0.0, swe.JUL_CAL)
        self.assertEqual(self.last()["time"], "1582-10-14T00:00:00Z")

    def test_to_utc(self):
        self.use({"utc": "2000-01-01T11:59:59.645Z"})
        self.assertEqual(swe.jdut1_to_utc(2451545.0), (2000, 1, 1, 11, 59, 59.645))
        self.use({"ut1": "1900-01-01T00:00:01.000Z"})
        self.assertEqual(swe.jdet_to_utc(2415020.5), (1900, 1, 1, 0, 0, 1.0))

    def test_deltat_is_days(self):
        self.use({"delta_t_s": 86.4})
        self.assertAlmostEqual(swe.deltat(0.0), 0.001)


class Arithmetic(unittest.TestCase):
    def test_split_deg(self):
        # Observed pyswisseph 2.10.03 outputs (tools/check/pyswe_oracle.py sweeps 78,592).
        cases = [
            ((123.456789, 0), (123, 27, 24, 0.4404000000019614, 1)),
            ((123.456789, swe.SPLIT_DEG_ZODIACAL | swe.SPLIT_DEG_ROUND_SEC), (3, 27, 24, 24.0, 4)),
            ((0.0, swe.SPLIT_DEG_ROUND_MIN), (0, 0, 30, 30.0, 1)),
            ((-12.5, 0), (12, 30, 0, 0.0, -1)),
            ((-200.25, swe.SPLIT_DEG_ZODIACAL), (20, 15, 0, 0.0, 6)),
            ((-200.25, swe.SPLIT_DEG_NAKSHATRA), (200, 15, 0, 0.0, -1)),
            ((29.99999, swe.SPLIT_DEG_ZODIACAL | swe.SPLIT_DEG_ROUND_SEC | swe.SPLIT_DEG_KEEP_SIGN),
             (29, 59, 59, 59.0, 0)),
            ((359.9999999, swe.SPLIT_DEG_ZODIACAL | swe.SPLIT_DEG_ROUND_SEC), (0, 0, 0, 0.0, 0)),
            ((400.0, swe.SPLIT_DEG_ZODIACAL), (10, 0, 0, 0.0, 13)),
            ((12.99999, swe.SPLIT_DEG_NAKSHATRA | swe.SPLIT_DEG_ZODIACAL | swe.SPLIT_DEG_ROUND_DEG),
             (0, 9, 59, 59.0, 1)),
        ]
        for (x, flag), want in cases:
            got = swe.split_deg(x, flag)
            self.assertEqual(got[:3] + got[4:], want[:3] + want[4:], (x, flag))
            self.assertAlmostEqual(got[3], want[3], places=6)

    def test_degrees(self):
        self.assertEqual(swe.degnorm(-30.0), 330.0)
        self.assertEqual(swe.degnorm(720.0), 0.0)
        self.assertEqual(swe.difdeg2n(10.0, 350.0), 20.0)
        self.assertEqual(swe.difdeg2n(350.0, 10.0), -20.0)
        self.assertEqual(swe.difdegn(10.0, 350.0), 20.0)
        lon, lat, dist = swe.cotrans((0.0, 0.0, 1.0), -23.4)
        self.assertEqual((lon, lat, dist), (0.0, 0.0, 1.0))
        back = swe.cotrans(swe.cotrans((123.0, 4.5, 2.0), -23.44), 23.44)
        self.assertAlmostEqual(back[0], 123.0)
        self.assertAlmostEqual(back[1], 4.5)

    def test_not_served_raise_by_name(self):
        with self.assertRaises(swe.Error) as e:
            swe.sol_eclipse_when_glob(2451545.0)
        self.assertIn("sol_eclipse_when_glob", str(e.exception))


class Transport(unittest.TestCase):
    def test_binary_server_is_not_in_this_release(self):
        with self.assertRaises(prometheia.Error):
            prometheia.Client(server="ws://127.0.0.1:47190")

    def test_no_ephemeris_is_an_error(self):
        with self.assertRaises(prometheia.Error):
            prometheia.Client(ephemeris=None, executable="/bin/false")


if __name__ == "__main__":
    unittest.main()

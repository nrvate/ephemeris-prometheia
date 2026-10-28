# SPDX-License-Identifier: GPL-2.0-or-later
"""prometheia._native, the engine in process (docs/PYTHON.md, "Native").

Held to the C results bit for bit through prometheia-json's stdio answers,
which print every double exactly: the same engine behind both. The .pxd is
kept in step with prometheia.h, and the words with the v4 registries.
SKIPs when the extension is not built (tools/gate.sh builds it) or DE440 is
absent.
"""
import json
import os
import re
import threading
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DE440 = os.path.join(REPO, "ephe", "linux_p1550p2650.440")
SAMPLE = os.path.join(REPO, "tests", "data", "sample-100.epm")
JSON = os.environ.get("PROMETHEIA_JSON", os.path.join(REPO, "build", "prometheia-json"))

try:
    from prometheia import _native as n
except ImportError:  # not built: the pure-Python client stands alone
    n = None

import prometheia


def registry(appendix):
    with open(os.path.join(REPO, "third_party", "ephproto", "v4", "registries.json")) as f:
        reg = json.load(f)

    def find(o):
        if isinstance(o, dict):
            if o.get("appendix") == appendix:
                return o
            for v in o.values():
                got = find(v)
                if got:
                    return got
        return None
    return find(reg)["entries"]


@unittest.skipIf(n is None, "prometheia._native is not built")
class Surface(unittest.TestCase):
    def test_pxd_declares_every_function_and_constant_of_the_header(self):
        with open(os.path.join(REPO, "include", "prometheia", "prometheia.h")) as f:
            header = f.read()
        with open(os.path.join(REPO, "python", "prometheia", "cprometheia.pxd")) as f:
            pxd = f.read()
        functions = set(re.findall(r"\b(prometheia_[a-z0-9_]+)\(", header))
        constants = set(re.findall(r"#define (PROMETHEIA_[A-Z0-9_]+) ", header))
        constants -= {"PROMETHEIA_API", "PROMETHEIA_H"}
        missing = sorted(x for x in functions | constants
                         if not re.search(r"\b%s\b" % x, pxd))
        self.assertEqual(missing, [])
        self.assertEqual(n.abi_version(), 6)

    def test_words_are_the_registry_names(self):
        a11 = {e["index"]: e["token"] for e in registry("A.11")}
        for token, value in n.ZODIACS.items():
            if token != "tropical":
                self.assertEqual(a11[value], token)
        a22 = {e["value"]: e["name"] for e in registry("A.22")}
        self.assertEqual({v: k for k, v in n.HOUSE_SYSTEMS.items()}, a22)

    def test_bad_words_are_refused_by_name(self):
        for kw in ({"zodiac": "krishnamurti"}, {"frame": "b1950"}, {"corrections": "true"},
                   {"corrections": ["light"]}, {"observer": "topocentric"},
                   {"observer": "body"}, {"center": 5}, {"zodiac": {"user": {}}}):
            with self.assertRaises(prometheia.Error) as e:
                n.Options(**kw)
            self.assertEqual(e.exception.code, "argument")

    def test_pure_functions(self):
        ra, dec = n.ecliptic_to_equatorial(90.0, 0.0, 23.4392911)
        self.assertAlmostEqual(ra, 90.0, places=12)
        self.assertAlmostEqual(dec, 23.4392911, places=12)
        lon, lat = n.equatorial_to_ecliptic(ra, dec, 23.4392911)
        self.assertAlmostEqual(lon, 90.0, places=12)
        self.assertAlmostEqual(lat, 0.0, places=12)
        jd = n.utc_to_tt(2016, 12, 31, 23, 59, 60.5)  # inside a leap second
        self.assertEqual(n.tt_to_utc(jd)[5], 60.5)
        self.assertEqual(n.civil_from_jd(n.jd_from_ymdhms(2000, 1, 1, 12)), (2000, 1, 1.5))
        self.assertGreater(n.delta_t(2451545.0), 63.0)
        spica = n.star_find("Spica")
        self.assertEqual(n.star_info(spica)["constellation"], "Vir")
        self.assertEqual(n.star_lookup("Spica")[0][:2], (spica, "exact"))
        self.assertEqual(n.constellation_at(201.3, -11.2), "Vir")
        h = n.houses_from_armc("porphyry", 30.0, 23.44, 47.0)
        self.assertEqual(len(h.cusps), 12)
        with self.assertRaises(prometheia.Error):
            n.houses_from_armc("placidus", 30.0, 23.44, 70.0)


@unittest.skipIf(n is None or not os.path.exists(DE440) or not os.path.exists(JSON),
                 "needs prometheia._native, DE440 and build/prometheia-json")
class Engine(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ds = n.Dataset(DE440, catalogs=[SAMPLE])
        cls.stdio = prometheia.Client(ephemeris=DE440, executable=JSON, catalogs=[SAMPLE])

    @classmethod
    def tearDownClass(cls):
        cls.stdio.close()

    def json_row(self, obj, jd, **args):
        out = self.stdio.positions(time={"jd_tt": jd}, objects=[obj], **args)
        r = out["results"][0]
        self.assertIsNone(r.get("error"), r)
        return r["rows"][0]

    def assert_same(self, p, row):
        self.assertEqual(p.lon_deg, row["longitude_deg"])
        self.assertEqual(p.lat_deg, row["latitude_deg"])
        self.assertEqual(p.dist_au, row["distance_au"])
        self.assertEqual(p.lon_speed, row["rates"]["longitude_deg_per_day"])
        self.assertEqual(p.lat_speed, row["rates"]["latitude_deg_per_day"])
        self.assertEqual(p.dist_speed, row["rates"]["distance_au_per_day"])
        self.assertEqual(p.ayanamsa_deg, row.get("ayanamsa_deg"))

    def test_positions_equal_the_json_tools_bit_for_bit(self):
        ctx = self.ds.local()
        cases = [({}, {}),
                 ({"zodiac": "lahiri"}, {"zodiac": "lahiri"}),
                 ({"zodiac": "true-citra"}, {"zodiac": "true-citra"}),
                 ({"frame": "j2000"}, {"frame": "j2000"}),
                 ({"corrections": "geometric"}, {"corrections": "geometric"}),
                 ({"observer": "heliocentric"}, {"observer": "heliocentric"}),
                 ({"observer": "topocentric", "site": (8.55, 47.37, 500.0)},
                  {"observer": "topocentric",
                   "site": {"lon_deg": 8.55, "lat_deg": 47.37, "height_m": 500.0}}),
                 ({"zodiac": "lahiri", "sidereal_plane": "invariable"},
                  {"zodiac": "lahiri", "sidereal_plane": "invariable"}),
                 ({"precession": "iau2006", "frame": "mean-of-date"},
                  {"precession": "iau2006", "frame": "mean-of-date"})]
        for jd in (2451545.0, 2461000.71):
            for native, args in cases:
                o = n.Options(**native)
                for name in ("Sun", "Moon", "Mars", "Pluto"):
                    if name == "Sun" and native.get("observer") == "heliocentric":
                        out = self.stdio.positions(time={"jd_tt": jd}, objects=[name], **args)
                        with self.assertRaises(prometheia.Error) as e:
                            ctx.calc(n.BODIES["sun"], jd, o)
                        self.assertEqual(str(e.exception), out["results"][0]["error"]["message"])
                        continue
                    self.assert_same(ctx.calc(n.BODIES[name.lower()], jd, o),
                                     self.json_row(name, jd, **args))
                self.assert_same(ctx.calc(ctx.lookup("Ceres"), jd, o),
                                 self.json_row({"asteroid": "Ceres"}, jd, **args))
                self.assert_same(ctx.calc_star("Spica", jd, o),
                                 self.json_row({"star": "Spica"}, jd, **args))
                self.assert_same(ctx.calc_hypothetical("cupido", jd, o),
                                 self.json_row({"hypothetical": "cupido"}, jd, **args))
                self.assert_same(
                    ctx.calc_orbit_point(301, "ascending-node", "mean", jd, o),
                    self.json_row({"point": "ascending-node", "of": "Moon", "method": "mean"},
                                  jd, **args))

    def test_houses_equal_the_json_tools_bit_for_bit(self):
        ctx = self.ds.local()
        for system in n.HOUSE_SYSTEMS:
            for zodiac in ("tropical", "lahiri"):
                for lat in (47.37, -33.9, 70.0):
                    out = self.stdio.houses(time={"jd_tt": 2451545.0}, systems=[system],
                                            site={"lon_deg": 8.55, "lat_deg": lat},
                                            zodiac=zodiac)["results"][0]
                    try:
                        h = ctx.houses(system, 2451545.0, 8.55, lat, n.Options(zodiac=zodiac))
                    except prometheia.Error as e:
                        self.assertEqual(out["error"]["message"], str(e))
                        continue
                    row = out["rows"][0]
                    self.assertEqual(list(h.cusps), row["cusps_deg"])
                    self.assertEqual(h.asc_deg, row["ascendant_deg"])
                    self.assertEqual(h.armc_deg, row["armc_deg"])

    def test_refusals_carry_the_engines_sentence_and_status(self):
        ctx = self.ds.local()
        with self.assertRaises(prometheia.Error) as e:
            ctx.calc(n.BODIES["mars"], 1e7)
        self.assertEqual(e.exception.code, "coverage")
        with self.assertRaises(prometheia.Error) as e:
            ctx.lookup("No Such Body")
        self.assertEqual(e.exception.code, "not-found")
        with self.assertRaises(prometheia.Error) as e:
            ctx.houses("placidus", 2451545.0, 0.0, 70.0)
        self.assertIn("polar circle", str(e.exception))
        with self.assertRaises(prometheia.Error):
            ctx.calc(4, 2451545.0, scale="utc")

    def test_one_context_per_thread(self):
        jds = [2451545.0 + 37.3 * k for k in range(40)]
        want = [self.ds.context().calc(n.BODIES["moon"], jd).lon_deg for jd in jds]
        got, errors = {}, []

        def work(i):
            try:
                ctx = self.ds.local()
                got[i] = [ctx.calc(n.BODIES["moon"], jd).lon_deg for jd in reversed(jds)][::-1]
            except Exception as e:  # pragma: no cover - reported below
                errors.append(e)
        threads = [threading.Thread(target=work, args=(i,)) for i in range(6)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(errors, [])
        for i in range(6):
            self.assertEqual(got[i], want)
        # A context answers only the thread that made it.
        ctx = self.ds.local()
        caught = []
        t = threading.Thread(target=lambda: caught.append(
            self.assertRaises(prometheia.Error, ctx.calc, 4, 2451545.0)))
        t.start()
        t.join()
        closed = self.ds.context()
        closed.close()
        with self.assertRaises(prometheia.Error):
            closed.calc(4, 2451545.0)

    def test_sidereal_time_is_the_houses_armc(self):
        ctx = self.ds.local()
        jd_ut1 = 2451545.0
        h = ctx.houses("porphyry", jd_ut1, 8.55, 47.37, scale="ut1")
        gast = ctx.sidereal_time(jd_ut1)
        self.assertAlmostEqual((gast + 8.55) % 360.0, h.armc_deg, places=10)
        self.assertAlmostEqual(ctx.obliquity(2451545.0), h.obliquity_deg, delta=1e-6)
        self.assertEqual(ctx.ephemeris_spans()[0][0], ctx.source)
        self.assertEqual(ctx.catalogs(), [(SAMPLE, 100)])


if __name__ == "__main__":
    unittest.main()

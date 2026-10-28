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

    def test_stub_matches_the_module(self):
        import ast
        with open(os.path.join(REPO, "python", "prometheia", "_native.pyi")) as f:
            tree = ast.parse(f.read())
        stub = {}
        for node in tree.body:
            if isinstance(node, ast.ClassDef):
                stub[node.name] = {m.name for m in node.body
                                   if isinstance(m, ast.FunctionDef) and not m.name.startswith("_")}
            elif isinstance(node, ast.FunctionDef):
                stub[node.name] = None
            elif isinstance(node, ast.AnnAssign) and node.target.id.isupper():
                stub[node.target.id] = None
        public = {k for k in dir(n) if not k.startswith("_") and k not in (
            "Error", "threading", "Atmosphere")}
        self.assertEqual(public - set(stub), set())  # nothing untyped
        self.assertEqual(set(stub) - public - {"Scale", "Site", "Atmosphere", "Object"}, set())
        for cls, methods in stub.items():
            if methods:
                real = {m for m in dir(getattr(n, cls)) if not m.startswith("_")}
                self.assertEqual(methods - real, set(), cls)
                attrs = {m for m in real if not callable(getattr(getattr(n, cls), m, None))}
                self.assertEqual(real - methods - attrs, set(), cls)
        # And the package re-exports every one.
        for name in public:
            self.assertIs(getattr(prometheia, name), getattr(n, name))

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
                 ({"zodiac": "raman"}, {"zodiac": "raman"}),
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

    def test_horizon(self):
        import math
        ctx = self.ds.local()
        jd_ut1, lon, lat = 2461000.25, 8.55, 47.37
        o = n.Options(observer="topocentric", site=(lon, lat, 400.0), frame="true-of-date",
                      coordinates="equatorial")
        sun = ctx.calc(n.BODIES["sun"], jd_ut1, o, scale="ut1")
        h = ctx.horizontal(jd_ut1, lon, lat, sun.lon_deg, sun.lat_deg)
        # Independently: the hour angle from GAST, then the spherical triangle.
        ha = math.radians((ctx.sidereal_time(jd_ut1) + lon - sun.lon_deg) % 360.0)
        d, p = math.radians(sun.lat_deg), math.radians(lat)
        alt = math.degrees(math.asin(math.sin(p) * math.sin(d) +
                                     math.cos(p) * math.cos(d) * math.cos(ha)))
        az = math.degrees(math.atan2(-math.cos(d) * math.sin(ha), math.sin(d) * math.cos(p) -
                                     math.cos(d) * math.sin(p) * math.cos(ha))) % 360.0
        self.assertAlmostEqual(h.altitude_deg, alt, places=9)
        self.assertAlmostEqual(h.azimuth_deg, az, places=9)
        self.assertEqual(h.apparent_altitude_deg, h.altitude_deg)  # no atmosphere
        refr = ctx.horizontal(jd_ut1, lon, lat, sun.lon_deg, sun.lat_deg, "standard", 400.0)
        self.assertAlmostEqual(refr.apparent_altitude_deg - refr.altitude_deg,
                               n.refraction(refr.altitude_deg, "true", "standard", 400.0),
                               places=12)
        ra, dec = ctx.equatorial_from_horizontal(jd_ut1, lon, lat, refr.azimuth_deg,
                                                 refr.apparent_altitude_deg, True, "standard",
                                                 400.0)
        self.assertAlmostEqual(ra, sun.lon_deg, places=9)
        self.assertAlmostEqual(dec, sun.lat_deg, places=9)
        self.assertAlmostEqual(n.refraction(0.0, "apparent") * 60.0, 33.9676, places=4)
        self.assertEqual(n.refraction(-0.5, "apparent"), 0.0)
        self.assertGreater(n.refraction(-0.5, "apparent", height_m=1000.0), 0.0)
        with self.assertRaises(prometheia.Error):
            ctx.horizontal(jd_ut1, lon, lat, 0.0, 0.0, atmosphere="thin")
        with self.assertRaises(prometheia.Error):
            n.refraction(5.0, "upward")

    def test_lunar_eclipse(self):
        ctx = self.ds.local()
        e = ctx.lunar_eclipse_next(2461042.5, kinds="total")  # 2026-01-01
        self.assertIsInstance(e, n.LunarEclipse)
        self.assertEqual((e.kind, e.saros_series, e.saros_member), ("total", 133, 27))
        self.assertLess(abs(e.umbral_magnitude - 1.1507), 0.002)
        self.assertEqual(len(e.contacts_tt), 6)
        self.assertTrue(all(x is not None for x in e.contacts_tt))
        p = ctx.lunar_eclipse_next(2461042.5, kinds=["penumbral"])
        self.assertEqual(p.kind, "penumbral")
        self.assertIsNone(p.contacts_tt[1])
        b = ctx.lunar_eclipse_next(e.jd_max_tt, backward=True)
        self.assertLess(b.jd_max_tt, e.jd_max_tt)
        loc = ctx.lunar_eclipse_at(e.jd_max_tt, (-157.86, 21.31))
        self.assertGreater(loc.altitude_deg[6], 30.0)
        self.assertEqual(loc.eclipse.jd_max_tt, e.jd_max_tt)
        self.assertIsNone(ctx.lunar_eclipse_at(e.jd_max_tt).altitude_deg[0])
        with self.assertRaises(prometheia.Error):
            ctx.lunar_eclipse_next(2461042.5, kinds="annular")
    def test_solar_eclipses(self):
        ctx = self.ds.local()
        start = 2461253.5  # 2026-08-01 0h
        g = ctx.solar_eclipse_next(start, "total")
        self.assertEqual((g.kind, g.central, g.saros), ("total", True, 126))
        self.assertAlmostEqual(g.max_ut1, 2461265.2403, places=3)
        self.assertLess(g.elements.l2, 0.0)
        self.assertEqual(ctx.solar_eclipse_next(start, ["hybrid", "annular"]).kind, "annular")
        self.assertEqual(ctx.solar_eclipse_next(start + 30, backward=True).lunation, 329)
        w = ctx.solar_eclipse_where(g.max_tt)
        self.assertTrue(w.axis_hits)
        self.assertEqual(w.kind, "total")
        self.assertGreater(w.north_lat_deg, w.lat_deg)
        miss = ctx.solar_eclipse_where(g.max_tt + 0.5)
        self.assertFalse(miss.axis_hits)
        self.assertIsNone(miss.lat_deg)
        loc = ctx.solar_eclipse_local_next(start - 40, 13.405, 52.52, 34.0)
        self.assertEqual(loc.kind, "partial")
        self.assertIsNone(loc.contact_tt[3])  # the Sun sets first
        self.assertIsNone(loc.contact_tt[1])
        self.assertLess(abs(loc.magnitude - 0.8739), 1e-3)
        with self.assertRaises(prometheia.Error):
            ctx.solar_eclipse_next(start, "lunar")
        with self.assertRaises(prometheia.Error):
            ctx.solar_eclipse_next(start, k_penumbra=2.0)

    def test_heliacal(self):
        import math
        ctx = self.ds.local()
        cairo = (31.2, 30.05)
        # Sirius at Cairo: swetest's recorded heliacal rising is JD 2461258.613
        # (docs/HELIACAL.md); ours is within three days, and is what the C test
        # held to the same number.
        h = ctx.heliacal_next(("star", "Sirius"), 2461222.5, cairo)
        self.assertEqual((h.event, h.model), ("rising", "schaefer"))
        self.assertLess(abs(h.jd_ut1 - 2461258.613), 3.5)
        self.assertLessEqual(h.jd_ut1, h.optimum_ut1)
        self.assertLessEqual(h.optimum_ut1, h.end_ut1)
        self.assertGreater(h.margin, 0.0)
        self.assertAlmostEqual(h.magnitude, -1.46, places=2)
        self.assertEqual(h.options["detection_margin_mag"], 0.0)
        self.assertEqual(h.atmosphere["relative_humidity_pct"], 40.0)
        self.assertIsNone(h.atmosphere["visual_range_km"])
        # The same through the registry id, and bit for bit the same.
        again = ctx.heliacal_next("star:Sirius", 2461222.5, {"lon_deg": 31.2, "lat_deg": 30.05})
        self.assertEqual(again.jd_tt, h.jd_tt)
        # A hazier sky is a later rising; an arc is the caller's.
        hazy = ctx.heliacal_next("star:Sirius", 2461222.5, cairo, visual_range_km=10.0)
        self.assertGreater(hazy.jd_ut1, h.jd_ut1 + 3)
        with self.assertRaises(prometheia.Error) as e:
            ctx.heliacal_next("star:Sirius", 2461222.5, cairo, model="arcus-visionis")
        self.assertEqual(e.exception.code, "argument")
        arc = ctx.heliacal_next("star:Sirius", 2461222.5, cairo, model="arcus-visionis",
                                arc_deg=10.0)
        self.assertGreaterEqual(arc.sun_depression_deg, 10.0 - 1e-3)
        self.assertIsNone(arc.limiting_magnitude)
        with self.assertRaises(prometheia.Error) as e:
            ctx.heliacal_next("star:Sirius", 2461222.5, cairo, event="evening-first")
        self.assertEqual(e.exception.code, "argument")
        with self.assertRaises(prometheia.Error):
            ctx.heliacal_next("star:Sirius", 2461222.5, cairo, event="noon")
        v = ctx.visibility_at("star:Sirius", h.optimum_tt, cairo)
        self.assertTrue(v.visible)
        self.assertAlmostEqual(v.margin, h.margin, places=9)
        self.assertAlmostEqual(v.limiting_magnitude_outside + v.extinction_mag,
                               v.limiting_magnitude, places=12)
        # The limiting magnitude alone: Schaefer's 220 nL example, standard eye.
        self.assertAlmostEqual(n.limiting_magnitude(220.0, age_years=25.0), 5.80, delta=0.03)
        self.assertLess(n.limiting_magnitude(2000.0), n.limiting_magnitude(200.0))
        with self.assertRaises(prometheia.Error):
            n.limiting_magnitude(-1.0)

    def test_rise_set(self):
        import math
        ctx = self.ds.local()
        start, lon, lat = 2461119.5, 8.0, 47.0  # 2026-03-20 0h UT
        rise = ctx.rise_set(n.BODIES["sun"], "rise", start, lon, lat)
        self.assertEqual(rise.outcome, "found")
        # The swetest time the C++ test holds us to within 3 s.
        self.assertLess(abs((rise.jd_ut1 - start) * 86400.0 - (5 * 3600 + 31 * 60 + 19.8)), 3.0)
        # The disc centre without refraction is at the true horizon.
        geo = ctx.rise_set(n.BODIES["sun"], "rise", start, lon, lat, limb="centre",
                           atmosphere=None)
        o = n.Options(observer="topocentric", site=(lon, lat), coordinates="equatorial")
        sun = ctx.calc(n.BODIES["sun"], geo.jd_ut1, o, scale="ut1")
        h = ctx.horizontal(geo.jd_ut1, lon, lat, sun.lon_deg, sun.lat_deg)
        self.assertLess(abs(h.altitude_deg) * 3600.0, 0.01)
        transit = ctx.rise_set(("star", "Sirius"), "upper-transit", start, lon, lat)
        self.assertEqual(transit.outcome, "found")
        cupido = ctx.rise_set(("hypothetical", "cupido"), "set", start, lon, lat)
        self.assertEqual(cupido.outcome, "found")
        polar = ctx.rise_set(n.BODIES["sun"], "rise", 2461211.5, lon, 75.0, window_days=1.0)
        self.assertEqual(polar.outcome, "always-above")
        self.assertTrue(math.isnan(polar.jd_ut1))
        before = ctx.rise_set(n.BODIES["sun"], "set", start, lon, lat, backward=True)
        self.assertLess(before.jd_ut1, start)
        with self.assertRaises(prometheia.Error):
            ctx.rise_set(n.BODIES["sun"], "dawn", start, lon, lat)
        with self.assertRaises(prometheia.Error):
            ctx.rise_set("Sun", "rise", start, lon, lat)

    def test_phenomena(self):
        ctx = self.ds.local()
        jd = 2461042.0
        venus = ctx.phenomena(n.BODIES["venus"], jd)
        self.assertEqual(venus.magnitude_model, "Mallama & Hilton 2018 eq. 3")
        self.assertTrue(-5.0 < venus.magnitude < -3.0)
        self.assertAlmostEqual(venus.illuminated_fraction,
                               (1.0 + __import__("math").cos(venus.phase_angle_deg * 0.017453292519943295)) / 2,
                               places=12)
        # Every object form calc() takes; a star has no phase, a point no V.
        regulus = ctx.phenomena(("star", "Regulus"), jd)
        self.assertIsNone(regulus.phase_angle_deg)
        self.assertAlmostEqual(regulus.magnitude, 1.35, places=2)
        node = ctx.phenomena("orbit:naif:301:ascending-node:mean", jd)
        self.assertIsNone(node.magnitude)
        self.assertTrue(node.magnitude_model.startswith("no magnitude"))
        ceres = ctx.phenomena("sb:1", jd)
        self.assertEqual(ceres.magnitude_model, "H, G (Bowell et al. 1989)")
        helio = ctx.phenomena(n.BODIES["mars"], jd, n.Options(observer="heliocentric"))
        self.assertIsNone(helio.elongation_deg)  # seen from the Sun
        self.assertLess(helio.phase_angle_deg, 1e-4)  # the Sun moves in the light time

    def test_object_registry(self):
        ctx = self.ds.local()
        # One namespace-wide lookup; ambiguity is reported, not resolved.
        (mars,) = ctx.object_lookup("mars")
        self.assertEqual((mars.id, mars.kind, mars.quality, mars.ambiguous),
                         ("naif:4", "planet", "exact", False))
        self.assertEqual([m.id for m in ctx.object_lookup("Ceres")], ["sb:1"])
        self.assertEqual(ctx.object_lookup("Lilith")[0].id, "orbit:naif:301:aphelion:mean")
        self.assertTrue(all(m.quality == "prefix" for m in ctx.object_lookup("node", prefix=True)))
        self.assertEqual(ctx.object_lookup("Nosuchbody"), [])
        # Info, and calc by id, by the object info gives, and by the old forms.
        ceres = ctx.object_info("sb:1")
        self.assertEqual((ceres.kind, ceres.name, ceres.number, ceres.standing),
                         ("asteroid", "Ceres", 1, "iau"))
        self.assertIsNone(ceres.vmag)
        self.assertAlmostEqual(ceres.sidereal_mean_motion_deg_day, 0.2141, delta=5e-4)
        node = ctx.object_info("orbit:naif:301:ascending-node:mean")
        self.assertEqual(node.object, ("orbit-point", 301, "ascending-node", "mean"))
        self.assertLess(node.mean_motion_deg_day, 0.0)
        jd = 2451545.0
        for id_, old in (("sb:1", 20000001), ("star:HR 3982", ("star", "Regulus")),
                         ("hyp:cupido", ("hypothetical", "cupido")),
                         ("orbit:naif:301:ascending-node:mean", node.object)):
            info = ctx.object_info(id_)
            by_id, by_ref, by_old = (ctx.calc(id_, jd).lon_deg, ctx.calc(info.object, jd).lon_deg,
                                     ctx.calc(old, jd).lon_deg)
            self.assertEqual(by_id, by_ref, id_)
            self.assertEqual(by_id, by_old, id_)
        self.assertEqual(ctx.calc("naif:5", jd).lon_deg, ctx.calc(5, jd).lon_deg)
        self.assertEqual(ctx.rise_set("naif:10", "rise", 2461119.5, 8.0, 47.0).outcome, "found")
        with self.assertRaises(prometheia.Error) as e:
            ctx.object_info("sb:Nosuchbody")
        self.assertEqual(e.exception.code, "not-found")
        # Listing, paged.
        ids, cursor = ctx.object_list(kind="hypothetical", limit=3)
        while cursor is not None:
            more, cursor = ctx.object_list(kind="hypothetical", cursor=cursor, limit=3)
            ids += more
        self.assertEqual(ids, ["hyp:" + t for t in ctx.hypotheticals()])
        bright, _ = ctx.object_list(kind=["star", "deep-sky"], vmag_max=1.0)
        self.assertTrue(15 <= len(bright) <= 25)
        with self.assertRaises(prometheia.Error):
            ctx.object_list(kind="galaxy")

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

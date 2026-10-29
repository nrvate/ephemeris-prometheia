# SPDX-License-Identifier: GPL-2.0-or-later
"""Segments through prometheia._native (docs/C_API.md, "Segments").

Held to C bit for bit: Segments.eval is prometheia_segments_eval, and a
Python loop over Segments.layout(), the documented arithmetic in the
documented order, gives the same doubles (Python's floats are C doubles
and contract nothing). Then the fit against Context.calc, the refusals, and
one handle shared by threads. Then segment_roots: its roots against
Segments.eval and Context.calc, the list the same for any capacity, month
handles back to back as one, the defaults, the refusals, and calls from
threads at once. SKIPs when the
extension is not built or DE440 is absent.
"""
import math
import os
import random
import threading
import unittest
import warnings

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DE440 = os.path.join(REPO, "ephe", "linux_p1550p2650.440")
SAMPLE = os.path.join(REPO, "tests", "data", "sample-100.epm")

try:
    from prometheia import _native as n
except ImportError:  # not built: the pure-Python client stands alone
    n = None

import prometheia

J2026 = 2461041.5
R = 180.0 / math.pi


def clenshaw(c, u):
    b1 = b2 = 0.0
    for k in range(len(c) - 1, 0, -1):
        b1, b2 = 2.0 * u * b1 - b2 + c[k], b1
    return u * b1 - b2 + c[0]


def own_eval(lay, jd):
    """docs/C_API.md's evaluation, from the layout alone."""
    bounds, stride = lay["bounds_jd_tt"], lay["stride"]
    i = 0
    while i + 1 < lay["count"] and jd >= bounds[i + 1]:
        i += 1
    half = lay["half_span_days"][i]
    u = (jd - lay["mid_jd_tt"][i]) / half
    p, v = [], []
    for c in range(3):
        at = (3 * i + c) * stride
        p.append(clenshaw(lay["coef"][at:at + stride], u))
        v.append(clenshaw(lay["dcoef"][at:at + stride], u) / half)
    x, y, z = p
    vx, vy, vz = v
    rho2 = x * x + y * y
    r = math.sqrt(rho2 + z * z)
    lon = math.atan2(y, x) * R
    if lon < 0.0:
        lon += 360.0
    return (lon, math.atan2(z, math.sqrt(rho2)) * R, r,
            (x * vy - y * vx) / rho2 * R,
            (vz * rho2 - z * (x * vx + y * vy)) / (r * r * math.sqrt(rho2)) * R,
            (x * vx + y * vy + z * vz) / r, tuple(p), tuple(v))


def as_tuple(pt):
    return (pt.lon_deg, pt.lat_deg, pt.dist_au, pt.lon_speed, pt.lat_speed, pt.dist_speed,
            pt.xyz_au, pt.vel_au_day)


def angle_arcsec(a, b):
    cx = a[1] * b[2] - a[2] * b[1]
    cy = a[2] * b[0] - a[0] * b[2]
    cz = a[0] * b[1] - a[1] * b[0]
    dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
    return math.atan2(math.sqrt(cx * cx + cy * cy + cz * cz), dot) * R * 3600.0


@unittest.skipIf(n is None or not os.path.exists(DE440), "needs prometheia._native and DE440")
class Segments(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ds = n.Dataset(DE440, catalogs=[SAMPLE])

    def test_eval_is_the_documented_arithmetic(self):
        ctx = self.ds.context()
        for opts in (None, n.Options(coordinates="equatorial"), n.Options(zodiac="lahiri")):
            seg = ctx.fit_segments(n.BODIES["moon"], J2026, J2026 + 20.0, opts, 0.001)
            lay = seg.layout()
            self.assertGreater(seg.count, 1)
            self.assertEqual(lay["stride"], 17)  # today's; a kernel indexes with lay["stride"]
            self.assertEqual(lay["bounds_jd_tt"][0], J2026)
            self.assertEqual(lay["bounds_jd_tt"][-1], J2026 + 20.0)
            jds = [J2026, J2026 + 20.0] + lay["bounds_jd_tt"][1:-1]
            rng = random.Random(5)
            jds += [rng.uniform(J2026, J2026 + 20.0) for _ in range(200)]
            for jd in jds:
                self.assertEqual(as_tuple(seg.eval(jd)), own_eval(lay, jd), jd)

    def test_within_tolerance_of_calc(self):
        ctx = self.ds.context()
        rng = random.Random(9)
        cases = [(n.BODIES["mars"], None), (n.BODIES["moon"], n.Options(zodiac="lahiri")),
                 (20000001, n.Options(sigma=False)),
                 (("orbit-point", n.BODIES["moon"], "ascending-node", "mean"), None),
                 (n.BODIES["moon"], n.Options(observer="topocentric", site=(8.55, 47.37, 400.0)))]
        for body, opts in cases:
            for tol in (0.001, 0.05):
                seg = ctx.fit_segments(body, J2026, J2026 + 10.0, opts, tol)
                self.assertLessEqual(seg.err_arcsec, 0.8 * tol)
                self.assertEqual(seg.tolerance_arcsec, tol)
                worst = 0.0
                for _ in range(100):
                    jd = rng.uniform(J2026, J2026 + 10.0)
                    worst = max(worst, angle_arcsec(ctx.calc(body, jd, opts).xyz_au,
                                                    seg.eval(jd).xyz_au))
                self.assertLessEqual(worst, tol, (body, tol))

    def test_refusals(self):
        ctx = self.ds.context()
        mars = n.BODIES["mars"]
        for start, end, tol in ((J2026, J2026, 0.001), (J2026 + 1, J2026, 0.001),
                                (J2026, J2026 + 1, 0.0), (J2026, J2026 + 1, -1.0),
                                (J2026, J2026 + 1, float("nan")), (float("nan"), J2026, 0.001)):
            with self.assertRaises(prometheia.Error) as e:
                ctx.fit_segments(mars, start, end, None, tol)
            self.assertEqual(e.exception.code, "argument")
        with self.assertRaises(prometheia.Error) as e:
            ctx.fit_segments(n.BODIES["sun"], J2026, J2026 + 1,
                             n.Options(observer="heliocentric"), 0.001)
        self.assertEqual(e.exception.code, "argument")
        with self.assertRaises(prometheia.Error) as e:
            ctx.fit_segments(mars, 1000000.5, 1000010.5)
        self.assertEqual(e.exception.code, "coverage")
        seg = ctx.fit_segments(mars, J2026, J2026 + 5.0)
        for jd in (J2026 - 1e-6, J2026 + 5.0 + 1e-6, float("nan")):
            with self.assertRaises(prometheia.Error) as e:
                seg.eval(jd)
            self.assertEqual(e.exception.code, "coverage")
        seg.close()
        with self.assertRaises(prometheia.Error):
            seg.eval(J2026)
        with self.assertRaises(prometheia.Error):
            seg.layout()

    def test_one_handle_many_threads(self):
        ctx = self.ds.context()
        seg = ctx.fit_segments(n.BODIES["moon"], J2026, J2026 + 30.0)
        ctx.close()  # the handle outlives the context that fitted it
        rng = random.Random(3)
        jds = [rng.uniform(J2026, J2026 + 30.0) for _ in range(2000)]
        want = [as_tuple(seg.eval(jd)) for jd in jds]
        got = [None] * 4

        def run(t):
            got[t] = [as_tuple(seg.eval(jd)) for jd in jds]

        threads = [threading.Thread(target=run, args=(t,)) for t in range(4)]
        for th in threads:
            th.start()
        for th in threads:
            th.join()
        for g in got:
            self.assertEqual(g, want)



def wrap(v, m):
    r = v - m * math.floor(v / m)
    return r - m if r > m / 2 else r


def as_root(r):
    return (r.jd_tt, r.slope, r.tangent)


@unittest.skipIf(n is None or not os.path.exists(DE440), "needs prometheia._native and DE440")
class SegmentRoots(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ds = n.Dataset(DE440)
        ctx = cls.ds.context()
        cls.moon = ctx.fit_segments(n.BODIES["moon"], J2026, J2026 + 120.0)
        cls.sun = ctx.fit_segments(n.BODIES["sun"], J2026 + 10.0, J2026 + 100.0)
        ctx.close()

    def test_ingresses_against_eval_and_calc(self):
        ctx = self.ds.context()
        band = self.moon.tolerance_arcsec / 3600.0
        with warnings.catch_warnings():
            warnings.simplefilter("error")  # no unresolved cell
            roots = n.segment_roots(self.moon, modulus=30.0, tol_days=1e-8)
        self.assertIsInstance(roots, n.SegmentRoots)
        self.assertEqual(roots.unresolved, 0)
        self.assertGreater(len(roots), 45)  # one every 2.5 days or so
        last = None
        for r in roots:
            self.assertFalse(r.tangent)
            self.assertGreater(r.slope, 11.0)  # the Moon, never retrograde
            self.assertAlmostEqual(r.err_days, band / r.slope + 1e-8, delta=1e-12)
            if last is not None:
                self.assertGreater(r.jd_tt - last, 1.5)
            last = r.jd_tt
            # eval within the band: near a bound the search's f is made
            # continuous, half the step away from eval's.
            p = self.moon.eval(r.jd_tt)
            self.assertLessEqual(abs(wrap(p.lon_deg, 30.0)), r.slope * r.err_days, r)
            self.assertAlmostEqual(r.slope, p.lon_speed, delta=1e-6 * r.slope)
            # The truth: within the band, and the time it takes to cross it.
            truth = ctx.calc(n.BODIES["moon"], r.jd_tt)
            self.assertLessEqual(abs(wrap(truth.lon_deg, 30.0)),
                                 1.1 * band + r.slope * 1e-8, r)

    def test_capacity_does_not_change_the_list(self):
        whole = [as_root(r) for r in n.segment_roots(self.moon, modulus=30.0)]
        for capacity in (1, 2, 7):
            self.assertEqual([as_root(r) for r in
                              n.segment_roots(self.moon, modulus=30.0, capacity=capacity)],
                             whole)

    def test_two_fits_and_the_defaults(self):
        phases = n.segment_roots(self.moon, self.sun, modulus=90.0)
        self.assertEqual([as_root(r) for r in phases],
                         [as_root(r) for r in n.segment_roots(
                             self.moon, self.sun, ca=1.0, cb=-1.0, modulus=90.0,
                             start=self.sun.start, end=self.sun.end)])
        self.assertGreater(len(phases), 10)  # four a month
        for r in phases:
            self.assertGreaterEqual(r.jd_tt, self.sun.start)
            self.assertLessEqual(r.jd_tt, self.sun.end)
            e = self.moon.eval(r.jd_tt).lon_deg - self.sun.eval(r.jd_tt).lon_deg
            self.assertLessEqual(abs(wrap(e, 90.0)), abs(r.slope) * r.err_days)
        # Each component by name; a moving target with its epoch.
        for comp in n.SEGMENT_COMPONENTS:
            n.segment_roots(self.moon, component=comp, start=J2026, end=J2026 + 5.0)
        moving = n.segment_roots(self.moon, target=0.0, target_rate=13.0, target_jd_tt=J2026)
        self.assertEqual(moving, sorted(moving, key=lambda r: r.jd_tt))

    def test_month_handles_are_one(self):
        ctx = self.ds.context()
        edges = [J2026 + 10.0 * k for k in range(13)]
        months = [ctx.fit_segments(n.BODIES["moon"], edges[k], edges[k + 1]) for k in range(12)]
        one = n.segment_roots(self.moon, modulus=30.0, end=edges[-1])
        chunked = n.segment_roots(months, modulus=30.0)
        self.assertEqual(len(chunked), len(one))
        for x, y in zip(one, chunked):
            self.assertLessEqual(abs(x.jd_tt - y.jd_tt), x.err_days + y.err_days)
        self.assertEqual(len(n.segment_roots(tuple(months), modulus=30.0)), len(one))
        with self.assertRaises(prometheia.Error) as e:
            n.segment_roots(months[:3] + months[4:], modulus=30.0)  # a gap
        self.assertEqual(e.exception.code, "argument")

    def test_refusals(self):
        def code(**kw):
            b = kw.pop("b", None)
            with self.assertRaises(prometheia.Error) as e:
                n.segment_roots(self.moon, b, **kw)
            return e.exception.code
        self.assertEqual(code(component="ra"), "argument")
        self.assertEqual(code(cb=-1.0), "argument")  # cb without b
        self.assertEqual(code(target_rate=1.0), "argument")  # no target_jd_tt
        self.assertEqual(code(b=self.sun, ca=0.5, cb=0.5, modulus=360.0), "argument")
        self.assertEqual(code(component="lat", modulus=30.0), "argument")
        self.assertEqual(code(tol_days=0.0), "argument")
        self.assertEqual(code(capacity=0), "argument")
        self.assertEqual(code(start=J2026 + 5.0, end=J2026 + 5.0), "argument")
        self.assertEqual(code(start=J2026 - 1.0), "coverage")
        self.assertEqual(code(b=self.sun, start=J2026), "coverage")
        ctx = self.ds.context()
        closed = ctx.fit_segments(n.BODIES["mars"], J2026, J2026 + 5.0)
        closed.close()
        with self.assertRaises(prometheia.Error):
            n.segment_roots(closed)
        for bad in (None, [], [self.moon, "moon"]):
            with self.assertRaises(prometheia.Error):
                n.segment_roots(bad)

    def test_sums(self):
        two = [as_root(r) for r in n.segment_roots(self.moon, self.sun, modulus=90.0)]
        self.assertEqual([as_root(r) for r in n.segment_roots_sum(
            [(self.moon, 1.0), (self.sun, -1.0)], modulus=90.0)], two)
        ctx = self.ds.context()
        mars = ctx.fit_segments(n.BODIES["mars"], J2026, J2026 + 120.0)
        # The Sun/Moon midpoint against Mars, modulo 180: both branches.
        terms = [(self.sun, 0.5), (self.moon, 0.5), (mars, -1.0)]
        with warnings.catch_warnings():
            warnings.simplefilter("error")  # no unresolved cell
            roots = n.segment_roots_sum(terms, modulus=180.0)
        self.assertEqual(roots.unresolved, 0)
        self.assertGreater(len(roots), 2)
        for r in roots:
            self.assertGreaterEqual(r.jd_tt, self.sun.start)  # the window all three cover
            self.assertLess(r.jd_tt, self.sun.end)
            f = (0.5 * self.sun.eval(r.jd_tt).lon_deg + 0.5 * self.moon.eval(r.jd_tt).lon_deg
                 - mars.eval(r.jd_tt).lon_deg)
            self.assertLessEqual(abs(wrap(f, 180.0)), abs(r.slope) * r.err_days, r)
        # Terms naming the same handles combine.
        self.assertEqual(
            [as_root(r) for r in n.segment_roots_sum(
                [(self.sun, 0.5), (self.moon, 0.5), (self.sun, -1.0)], modulus=90.0)],
            [as_root(r) for r in n.segment_roots_sum(
                [(self.sun, -0.5), (self.moon, 0.5)], modulus=90.0)])
        for bad in ([], [(self.sun, 1.0), (self.sun, -1.0)], [(self.sun, 1.0, 2.0)],
                    [(self.sun, 1.0)] * (n.SEGMENT_MAX_TERMS + 1), [("sun", 1.0)]):
            with self.assertRaises(prometheia.Error) as e:
                n.segment_roots_sum(bad, modulus=90.0)
            self.assertEqual(e.exception.code, "argument", bad)

    def test_threads_at_once(self):
        want = [as_root(r) for r in n.segment_roots(self.moon, self.sun, modulus=90.0)]
        got = [None] * 4

        def run(t):
            got[t] = [as_root(r) for r in n.segment_roots(self.moon, self.sun, modulus=90.0)]

        threads = [threading.Thread(target=run, args=(t,)) for t in range(4)]
        for th in threads:
            th.start()
        for th in threads:
            th.join()
        for g in got:
            self.assertEqual(g, want)

if __name__ == "__main__":
    unittest.main()

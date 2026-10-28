# SPDX-License-Identifier: GPL-2.0-or-later
"""Segments through prometheia._native (docs/C_API.md, "Segments").

Held to C bit for bit: Segments.eval is prometheia_segments_eval, and a
Python loop over Segments.layout(), the documented arithmetic in the
documented order, gives the same doubles (Python's floats are C doubles
and contract nothing). Then the fit against Context.calc, the refusals, and
one handle shared by threads. SKIPs when the extension is not built or DE440
is absent.
"""
import math
import os
import random
import threading
import unittest

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


if __name__ == "__main__":
    unittest.main()

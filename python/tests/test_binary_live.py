# SPDX-License-Identifier: GPL-2.0-or-later
"""prometheia.Client over protocol v4 against a real prometheiad, held to the
stdio transport's answers on the same DE440: the two must agree on every field
the binary answer carries, since the same engine answers both.

Skips without build/prometheiad, build/prometheia-json and
ephe/linux_p1550p2650.440. About a second."""
import math
import os
import socket
import subprocess
import time
import unittest

import prometheia

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DAEMON = os.path.join(REPO, "build", "prometheiad")
JSON = os.environ.get("PROMETHEIA_JSON", os.path.join(REPO, "build", "prometheia-json"))
DE440 = os.environ.get("PROMETHEIA_DE440", os.path.join(REPO, "ephe", "linux_p1550p2650.440"))


def same_rows(binary, stdio):
    """Every row of the binary answer equals the stdio row on the binary row's keys."""
    if len(binary["results"]) != len(stdio["results"]):
        return False
    for rb, rs in zip(binary["results"], stdio["results"]):
        if rb.get("error") != rs.get("error") or len(rb.get("rows", [])) != len(rs.get("rows", [])):
            return False
        for xb, xs in zip(rb.get("rows", []), rs.get("rows", [])):
            if not subset(xb, xs):
                return False
    return True


def subset(a, b):
    if isinstance(a, dict):
        # The time block differs in spelling (jd_ut1 here, jd_tt and utc there); ΔT is shared.
        return all(k in b and subset(v, b[k]) for k, v in a.items()
                   if k != "time") and ("delta_t_s" not in a.get("time", {}) or
                                        a["time"]["delta_t_s"] == b["time"].get("delta_t_s"))
    return same(a, b)


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
        return fn(self.binary), fn(self.stdio)

    def test_identical_to_stdio(self):
        t = {"jd_ut1": 2451545.0}
        site = {"lon_deg": 8.55, "lat_deg": 47.37, "height_m": 500.0}
        bodies = ["Sun", "Moon", "Mercury", "Mars", "Pluto", "mean node", "true node",
                  "Lilith", "true Lilith", "natural apogee", "natural perigee"]
        calls = [lambda c: c.positions(time=t, objects=bodies),
                 lambda c: c.positions(time=t, objects=bodies, zodiac="lahiri"),
                 lambda c: c.positions(time={"jd_tt": 2461300.5}, objects=["Venus"],
                                       coordinates="equatorial", frame="j2000"),
                 lambda c: c.positions(time=t, objects=["Moon"], observer="topocentric",
                                       site=site),
                 lambda c: c.positions(time=t, objects=["Mars"], observer="heliocentric"),
                 lambda c: c.positions(time=t, objects=["Saturn"], corrections=[], observer="heliocentric",
                                       coordinates="ecliptic"),
                 lambda c: c.positions(time=t, objects=[{"star": "Spica"}]),
                 lambda c: c.positions(time=t, objects=[
                     {"point": "ascending-node", "of": "Mars", "method": "mean"}]),
                 lambda c: c.houses(time=t, site={"lon_deg": 18.4, "lat_deg": -33.9},
                                    systems=["W"], zodiac="lahiri"),
                 lambda c: c.houses(time=t, site={"lon_deg": 0.0, "lat_deg": 66.0},
                                    systems=["R"])]
        # One system per call: v4 bounds the objects in a request (WELCOME), 16 points each.
        calls += [lambda c, h=h: c.houses(time=t, site=site, zodiac="lahiri", systems=[h])
                  for h in "PKORCAWBMXT"]
        for fn in calls:
            try:
                a, b = self.both(fn)
            except prometheia.Error as e:
                self.fail("a call both transports should answer was refused: %s" % e)
            self.assertTrue(same_rows(a, b), (a, b))

    def test_refusals(self):
        t = {"jd_ut1": 2451545.0}
        polar = {"lon_deg": 0.0, "lat_deg": 70.0}
        for c in (self.binary, self.stdio):
            err = c.houses(time=t, site=polar, systems=["placidus"])["results"][0]["error"]
            self.assertEqual(err["code"], "undefined-at-latitude")
            self.assertIn("polar circle", err["message"])
            # Regiomontanus' cusps fall out of zodiacal order here at this hour.
            err = c.houses(time=t, site=polar, systems=["R"])["results"][0]["error"]
            self.assertEqual(err["code"], "undefined-at-latitude")
            self.assertIn("fall out of zodiacal order", err["message"])
            err = c.positions(time=t, objects=[{"asteroid": "2060"}])["results"][0]["error"]
            self.assertTrue(err["message"])  # no catalog on this server: refused, not zeros
        err = self.stdio.positions(time=t, objects=["Chiron"])["results"][0]["error"]
        self.assertIn("catalog", err["message"])
        with self.assertRaises(prometheia.Error) as e:
            self.binary.convert_time(time="2000-01-01T12:00:00Z")  # a clock time: not in v4 yet
        self.assertIn("leap-second", str(e.exception))

    def test_batch_and_errors_carry_the_servers_sentence(self):
        out = self.stdio.positions(time={"jd_ut1": 2451545.0}, objects=["Sun", "Moon", "Mars"])
        self.assertEqual([r["object"]["resolved"] for r in out["results"]],
                         ["Sun", "Moon", "Mars"])

    def test_capabilities(self):
        w = self.binary.capabilities()
        self.assertTrue(w["kinds"] & (1 << 6))
        self.assertEqual(w["house_systems"], list(range(11)))
        self.assertEqual(w["sidereal_time"], "iau2006-2000a")


if __name__ == "__main__":
    unittest.main()

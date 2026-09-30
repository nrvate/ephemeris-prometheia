# SPDX-License-Identifier: GPL-2.0-or-later
"""Calendar arithmetic (prometheia._calendar): local, no server."""
import unittest

from prometheia import swe


class Calendar(unittest.TestCase):
    def test_known_days(self):
        self.assertEqual(swe.julday(2000, 1, 1, 12.0), 2451545.0)
        self.assertEqual(swe.julday(1582, 10, 15, 0.0), 2299160.5)
        self.assertEqual(swe.julday(1582, 10, 4, 0.0, swe.JUL_CAL), 2299159.5)
        self.assertEqual(swe.julday(-4712, 1, 1, 12.0, swe.JUL_CAL), 0.0)
        self.assertEqual(swe.julday(1990, 6, 15), 2448058.0)  # hour defaults to noon

    def test_round_trip(self):
        for cal in (swe.GREG_CAL, swe.JUL_CAL):
            for y in (-4712, -500, 0, 1, 1582, 1600, 1900, 2000, 2024, 9999):
                for m, d, h in ((1, 1, 0.0), (2, 29, 6.5), (3, 1, 23.75), (12, 31, 12.0)):
                    if m == 2 and d == 29 and not (y % 4 == 0 and (cal == swe.JUL_CAL or
                                                                   y % 100 or y % 400 == 0)):
                        continue
                    y2, m2, d2, h2 = swe.revjul(swe.julday(y, m, d, h, cal), cal)
                    self.assertEqual((y2, m2, d2), (y, m, d))
                    self.assertAlmostEqual(h2, h, places=6)

    def test_day_of_week(self):
        self.assertEqual(swe.day_of_week(2451545.0), 5)  # 2000-01-01, a Saturday; Monday is 0
        self.assertEqual(swe.day_of_week(2451545.0 - 5), 0)

    def test_bad_calendar(self):
        with self.assertRaises(ValueError):
            swe.julday(2000, 1, 1, 12.0, 7)

    def test_time_zone(self):
        # 14:30 at UTC+2 is 12:30 UTC, and back.
        self.assertEqual(swe.utc_time_zone(1990, 6, 15, 14, 30, 0.0, 2.0)[:5], (1990, 6, 15, 12, 30))
        self.assertEqual(swe.utc_time_zone(2000, 1, 1, 1, 0, 0.0, 2.0)[:5], (1999, 12, 31, 23, 0))


if __name__ == "__main__":
    unittest.main()

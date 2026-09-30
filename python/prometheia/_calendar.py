# SPDX-License-Identifier: GPL-2.0-or-later
"""Calendar arithmetic, done locally: Julian day numbers and civil dates in
the proleptic Gregorian or the Julian calendar (Meeus, Astronomical
Algorithms, 2nd ed., ch. 7). No time scales here -- those need the server's
delta T and leap-second table."""
import math

JUL_CAL = 0
GREG_CAL = 1


def _check(cal):
    if cal not in (GREG_CAL, JUL_CAL):
        raise ValueError("cal is GREG_CAL or JUL_CAL")


def julday(year, month, day, hour=12.0, cal=GREG_CAL):
    """The Julian day of a calendar date and decimal hour (any year, 0 = 1 BC)."""
    _check(cal)
    y, m = int(year), int(month)
    if m <= 2:
        y -= 1
        m += 12
    if cal == GREG_CAL:
        a = math.floor(y / 100)
        b = 2 - a + math.floor(a / 4)
    else:
        b = 0
    return (math.floor(365.25 * (y + 4716)) + math.floor(30.6001 * (m + 1)) + day + b - 1524.5
            + hour / 24.0)


def revjul(jd, cal=GREG_CAL):
    """(year, month, day, hour) of a Julian day, hour decimal."""
    _check(cal)
    jd5 = jd + 0.5
    z = math.floor(jd5)
    f = jd5 - z
    if cal == GREG_CAL:
        alpha = math.floor((z - 1867216.25) / 36524.25)
        a = z + 1 + alpha - math.floor(alpha / 4)
    else:
        a = z
    b = a + 1524
    c = math.floor((b - 122.1) / 365.25)
    d = math.floor(365.25 * c)
    e = math.floor((b - d) / 30.6001)
    day = int(b - d - math.floor(30.6001 * e))
    month = int(e - 1 if e < 14 else e - 13)
    year = int(c - 4716 if month > 2 else c - 4715)
    return year, month, day, f * 24.0


def day_of_week(jd):
    """0 Monday .. 6 Sunday."""
    return int(math.floor(jd + 0.5)) % 7


def split_hours(hour):
    """Decimal hours to (hour, minute, second); the seconds keep the fraction."""
    h = int(math.floor(hour))
    rem = (hour - h) * 60.0
    mi = int(math.floor(rem))
    return h, mi, (rem - mi) * 60.0

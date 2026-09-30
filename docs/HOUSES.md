# Houses

House cusps and the chart's angles, computed by the engine
(`include/prometheia/houses.hpp`, `Engine::houses` and `houses_ut`) and
served by the JSON/MCP `houses` tool. These are phases 1 and 2 of the
Python library plan ([PYTHON.md](PYTHON.md)); the protocol extension comes
later. Each system is built from its
published geometric definition, and the arithmetic is our own.
`swetest`'s printed output is an oracle only.

## Inputs

- **ARMC:** the Greenwich apparent sidereal time (IAU 2006 GMST from the
  Earth rotation angle, plus the IAU 2000A equation of the equinoxes) plus
  the site's east longitude. Held to ERFA's `gst06a` at eight epochs
  1600–2200: worst 0.00075″.
- **Obliquity:** the true obliquity of date, which is the mean obliquity of
  the precession model asked (Vondrák 2011's pole angle by default,
  [FRAMES.md](FRAMES.md)) plus the IAU 2000A nutation in obliquity.
- **Latitude:** geodetic, strictly between −90° and 90°. The site's height
  does not enter.
- **Time:** UT1 for the Earth's rotation. TT for precession and nutation
  comes through the engine's ΔT model, as for positions.

## Systems

| letter | token | cusps |
|---|---|---|
| P | `placidus` | each point's own semi-arc (diurnal above the horizon, nocturnal below) trisected in time; iterated to 1e-12 rad |
| K | `koch` | the Ascendants when the MC degree's diurnal semi-arc, trisected, carries the sky from that degree's rising to its setting |
| O | `porphyry` | each quadrant's ecliptic arc between the angles trisected |
| R | `regiomontanus` | great circles through the horizon's north and south points, and the equator's points 30° apart from the meridian |
| C | `campanus` | the same circles through the prime vertical's points 30° apart from the zenith |
| A (or E) | `equal` | 30° from the Ascendant |
| W | `whole-sign` | the Ascendant's sign is the first house |
| B | `alcabitius` | the Ascendant's diurnal semi-arc (and the nocturnal one) trisected on the equator, carried to the ecliptic along hour circles |
| M | `morinus` | the equator in 30° steps from the ARMC, carried along circles of ecliptic latitude |
| X | `meridian` | the same steps carried along hour circles (axial rotation) |
| T | `topocentric` | Polich and Page: oblique ascensions ARMC + 30° k under poles tan φₖ = (j/3) tan φ, j = 1, 2 |

Any other letter or token is refused by name.

## Angles

- **Ascendant:** the ecliptic's intersection with the eastern horizon.
- **MC:** the ecliptic's culminating point, at hour angle 0, in every system.
  Inside a polar circle this point can be below the horizon.
- **ARMC:** as above.
- **Vertex:** the ecliptic's intersection with the prime vertical, in the
  west.
- **Equatorial Ascendant:** the ecliptic point at right ascension ARMC + 90°.

The co-Ascendants (Koch's, Munkasey's) and the polar Ascendant are not
served. We have adopted no published definition of them yet.

## Inside a polar circle

A polar circle here means |φ| > 90° − ε, with ε the true obliquity of that
date (66.56° in 2000).

- **Placidus and Koch are refused** (`ArgumentError`, with the latitude, the
  circle's latitude at that date and the systems that do answer) at every
  instant (maintainer, 2026-09-29).
  - There, part of the ecliptic never rises or sets, and the time-trisected
    construction loses its meaning: Koch's shifted Ascendants can be setting
    points.
  - Before the band rule, Koch answered at some polar instants with cusps
    100° from anything meaningful.
  - We never substitute another system. `swetest` substitutes Porphyry and
    says so.
- **Regiomontanus' and Campanus' cusp 10** is where their meridian circle
  meets the ecliptic on the zenith's half, which is above the horizon.
  - That is the MC, except when the MC culminates below the horizon, and
    then it is the IC. Below the polar circles the two coincide, so a sweep
    to ±66° cannot tell them apart.
  - `swetest` agrees on the cusp. It also reports that point as the MC for
    these systems, and for Topocentric. We keep the MC at hour angle 0.
- **Topocentric** answers at every latitude short of the pole. Each cusp is
  the eastern point of its pole's horizon, taken literally, and cusp 10 is
  the MC.
  - Inside a polar circle `swetest` reorders Topocentric cusps. When the MC
    culminates below the horizon it takes the IC as cusp 10 and mirrors the
    quadrants. At other hours it flips single opposite pairs.
  - There, the pole heights j/3·tan φ can themselves exceed 90° − ε. The
    definition gives no rule for reordering, so we do not reorder.

## Where nothing is defined

- **At a pole** (|φ| = 90°) there is no meridian, and the longitude, and so
  the ARMC, is arbitrary. Every system is refused.
- **At an instant when the ecliptic lies along a defining circle** (the
  horizon, the prime vertical, or one of a system's own circles), the
  intersection that circle defines does not exist. That system is refused
  for that instant, with the same code the JSON tool gives the polar
  refusal. It happens only on a polar circle's edge.

## Sidereal houses

- Houses are sidereal on the ecliptic of date only. Every longitude is less
  the zodiac's true ayanamsha, and the ARMC is unchanged.
- **Whole Sign counts from the sidereal Ascendant's sign.** It is not the
  tropical cusps shifted.
- A fixed sidereal plane (anchor, invariable) and a frame other than the true
  equinox of date are refused. No definition of a house on those planes has
  been adopted.

## Validation

`tests/test_houses.cpp`, from `tools/gen/gen_house_fixtures.py`
(`swetest -house` and ERFA; run it with `.venv-oracle`):

- **Geometry:**
  - All eleven systems at eight epochs over 1600–2200, ten latitudes from
    −66° to 66°, and the polar rows the definitions share with `swetest`
    (67°, 70°, 80° and −70°, six hours each).
  - Graded at `swetest`'s own printed ARMC and on its obliquity (Vondrák's
    ε_A series plus nutation, [CROSS-TEST.md](CROSS-TEST.md)).
  - 18,214 values; worst **0.0056″**.
  - The exception is Koch within a degree of a polar circle, at **0.0334″**.
    There the MC's semi-arc moves cusp 11 by 0.087″ per milliarcsecond of
    obliquity (measured at −66°), and `swetest`'s nutation differs from the
    full IAU 2000A at the sub-milliarcsecond level. The bound there is 0.1″,
    and 0.01″ elsewhere.
- **Sidereal time:** within 0.00075″ of ERFA's `gst06a`.
  - `swetest`'s ARMC departs from that IAU 2006/2000A sidereal time by
    +1.26″ at 1600, +0.37″ at 1800, −1.79″ at 2100 and −0.68″ at 2200.
  - It agrees to its printed resolution over 1900–2026.
  - That residual is `swetest`'s. It does not come from ΔT: the two sides'
    ΔT differences at those epochs neither scale with it nor share its
    sign.
- **Checked from the definitions:**
  - the polar refusals on both sides of the circle, 0.001° apart;
  - the other nine systems answering at 80°;
  - Regiomontanus' and Campanus' cusp 10 above the horizon, which is the IC
    in the sample;
  - the MC at hour angle 0;
  - Topocentric's cusp 10 as the MC;
  - every house under 180°, running eastward;
  - sidereal Whole Sign;
  - the refused planes and frames.
- **Fault-injected** before the tests were trusted. Each of the following
  failed the suite: a Koch step changed by 0.5%, Regiomontanus/Campanus'
  cusp 10 put back to the MC, and the polar band rule removed.

## Served as

- `Engine::houses` (TT) and `houses_ut` (UT1), C++;
- the JSON/MCP `houses` tool ([JSON_API.md](JSON_API.md), "Houses").

## Not yet

- rates of the cusps and angles;
- the C ABI (relayed to Astrolog before it lands);
- the protocol v4 messages (being drafted with Astrolog);
- the remaining systems (Gauquelin sectors, Vehlow, Sunshine, APC,
  Krusinski and others).

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
- **Cusps out of zodiacal order are refused** (maintainer, 2026-10-27).
  Every answer's twelve cusps go once round the zodiac, each the next
  eastward, and their forward gaps sum to 360°. Where a system's
  construction gives them out of order, its houses would overlap (the gaps
  summed to 720–3960°), and the system is refused at that latitude and
  sidereal time. It is never reordered or replaced. The Astrolog side
  found the shape in their own house work and asked for the invariant.
  - **Regiomontanus and Campanus** go out of order only inside a polar
    circle, at some sidereal times: from 66.57° at ε = 23.44°, 0.01° past
    the circle.
  - **Topocentric** goes out of order from about 1.6° short of a polar
    circle: 64.98° at ε = 23.44°, 63.87° at 24.5°, 66.50° at 22°. Each cusp
    is the eastern point of its pole's horizon, taken literally, and the
    pole heights j/3·tan φ there can exceed 90° − ε. `swetest` reorders
    these cusps, but the definition gives no rule for reordering, so we
    refuse instead.
  - Below that, every system but Placidus and Koch answers at every
    sidereal time. Porphyry, Equal, Whole Sign, Alcabitius, Morinus and
    Meridian answer at every latitude short of the pole.
- **Regiomontanus' and Campanus' cusp 10** is where their meridian circle
  meets the ecliptic on the zenith's half, which is above the horizon.
  - That is the MC, except when the MC culminates below the horizon, and
    then it is the IC. Wherever it is the IC, the cusps are out of order
    and the system is refused, so in every answer cusp 10 is the MC.
  - `swetest` agrees on the cusp. It also reports that point as the MC for
    these systems, and for Topocentric. We keep the MC at hour angle 0.
- **Topocentric's cusp 10** is the MC.
- **The Ascendant is the rising point at every latitude.** Inside a polar
  circle the ecliptic's two horizon points can swap sides. The Ascendant is
  always the eastern one: on the horizon to 1e-13° and never in the west,
  over ±89.5° and every half degree of ARMC (258,480 cases, 2026-10-27).

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
  - 17,969 values; worst **0.0056″**. The 15 polar rows where
    Regiomontanus, Campanus or Topocentric fall out of zodiacal order are
    refused, not compared (`swetest` prints them, reordering Topocentric's).
  - The exception is Koch within a degree of a polar circle, at **0.0334″**.
    There the MC's semi-arc moves cusp 11 by 0.087″ per milliarcsecond of
    obliquity (measured at −66°), and `swetest`'s nutation differs from the
    full IAU 2000A at the sub-milliarcsecond level. The bound there is 0.1″,
    and 0.01″ elsewhere.
- **Sidereal time:** within 0.00075″ of ERFA's `gst06a` at the fixtures'
  ARMCs. Through the C API at six epochs over 1600–2200 with a fixed ΔT
  (`c_api_sidereal_time_and_obliquity_match_erfa`): GMST 3e-8″ from
  `gmst06`, GAST 0.00015″ from `gst06a`, the IAU 2006 mean obliquity 1e-11″
  from `obl06`, and the true obliquity 0.00005″ from `obl06` plus
  `nut06a`'s Δε.
  - `swetest`'s ARMC departs from that IAU 2006/2000A sidereal time by
    +1.26″ at 1600, +0.37″ at 1800, −1.79″ at 2100 and −0.68″ at 2200.
  - It agrees to its printed resolution over 1900–2026.
  - That residual is `swetest`'s. It does not come from ΔT: the two sides'
    ΔT differences at those epochs neither scale with it nor share its
    sign.
- **Checked from the definitions:**
  - the polar refusals on both sides of the circle, 0.001° apart;
  - the other nine systems answering at 80°, save Regiomontanus, Campanus
    and Topocentric where their cusps fall out of order;
  - the cusp order itself: all eleven systems at every half degree of
    latitude to ±89.9°, every 15° of ARMC and two obliquities, either going
    once round the zodiac or refused, and only the systems named above
    refusing (`house_cusps_go_round_the_zodiac_once_or_are_refused`; the
    0.1°, 7.5°, three-obliquity sweep is a report case);
  - Regiomontanus' and Campanus' cusp 10 above the horizon, and the MC in
    every answer;
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
- the C API's `prometheia_calc_houses`, `_ut` and `prometheia_houses_from_armc`
  ([C_API.md](C_API.md), "Houses");
- the JSON/MCP `houses` tool ([JSON_API.md](JSON_API.md), "Houses");
- protocol v4 object kind 6 in `prometheiad` ([SERVER.md](SERVER.md),
  "House points"), as the drop Astrolog wrote with us specifies (their
  `6235abd`, §3.5b).

**Against the protocol's reference rows.** `ephsrv/houses-rows.tsv` holds
Astrolog's numeric rows, made from §3.5b's definitions alone by their
`tools/houses_ref.py`. It covers every system and angle at 6 ARMCs,
17 latitudes (−89.999° to 90°) and 2 ayanamshas: 2,244 rows and
35,616 graded cells. `server_houses_match_the_reference_rows` grades our
`houses::compute` against them at each row's own ARMC and obliquity.
- **Values:** worst 0.000002″, which is the cells' nine-decimal rounding,
  Placidus included.
- **Refusals:** none where the rows refuse and we answer. Since
  2026-10-27, 66 rows (Topocentric chiefly) give cusps where we refuse
  them as out of zodiacal order. The test accepts a refusal there only
  when the row's own cusps are out of order, and they are in all 66.
- **The tolerance's history:** the rows were first printed to seven
  decimals, against a stated tolerance of 0.0001″ that seven decimals
  cannot express (their rounding alone is 0.00018″). We reported it, and
  their `513437f` reprinted them to nine.
- **Where it runs:** `tools/scheduled.sh`, pinned by the rows' digest.

**Astrolog's server gate, run against ours.** Their `tools/ephsrv_houses.py`,
run against our `prometheiad` (`b312e13`, DE440), found two faults. Both
were in their specification and server, and neither was in our engine.
- They refused a system's four angles inside a polar circle along with its
  cusps. The angles are the same in every system, so they are refused only
  at a pole.
- They gave Whole Sign's angles rate 0. Only Whole Sign's cusps are steps;
  its angles moved 338–383° a day here, correctly.
- Both sat in their server and their gate together, so no check with the
  same engine on both sides could see them.
- Fixed in their `9109449`. The gate passes on both engines: 11,088
  answered rows at each row's own ARMC and obliquity, and 352 rates. It
  runs as a leg of their `tools/ephsrv-prometheia.sh`.

## Not yet

- rates in the engine and the JSON tool (`prometheiad` answers them for kind 6,
  as the stencil of its answered longitudes);
- the C ABI (relayed to Astrolog before it lands);
- the remaining systems (Gauquelin sectors, Vehlow, Sunshine, APC,
  Krusinski and others).

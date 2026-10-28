# Phenomena and magnitudes

`Engine::phenomena(object, jd_tt, opts)` (and `_ut`) gives what an object
looks like from the options' observer. The C API is
`prometheia_calc_phenomena(_ut)`, and Python has `Context.phenomena`. The
object is any `ObjectRef` ([REGISTRY.md](REGISTRY.md)).

| Quantity | Definition |
|---|---|
| phase angle | the angle at the object between the Sun and the observer, when the light left it |
| illuminated fraction | (1 + cos α) / 2 |
| elongation | the angle at the observer between the object and the Sun, as the options see them (apparent by default) |
| apparent diameter | 2 asin(R/d), with R the IAU mean radius |
| magnitude | V, from the published formula named in `magnitude_model` |

Angles between vectors are taken as atan2(|a × b|, a · b), never acos.

- The elongation is unsigned, 0°–180°.
- Every quantity follows the options' observer. A topocentric Moon's
  diameter is the one from the site.
- A quantity that does not apply is NaN, and the call succeeds. The call is
  refused only where `calc` refuses it, as with an observer that is the
  object (the Sun from the Sun, the Earth from the geocentre).

## The phase angle

The angle at the body is taken when the light left it:
- **toward the observer:** the light-time position, without aberration or
  deflection, since those bend the observer's view, not the geometry at
  the body;
- **toward the Sun:** the Sun and the body both at the retarded instant
  t − τ.

Seen from the Sun, the angle is not quite zero: the Sun moves about the
barycentre during the light time (2.3e-6° for Mars, 2026).

A star and an orbit point have no phase angle and no illuminated fraction.
The Sun's own phase angle is 0, and its elongation is 0.

## Magnitudes

**The planets:** Mallama & Hilton (2018), *Computing apparent planetary
magnitudes for The Astronomical Almanac*, Astronomy and Computing 25, 10–24
(arXiv:1808.01973). Each formula is used where the paper says it holds:

| Planet | Equation | Phase range |
|---|---|---|
| Mercury | 2 | the whole curve |
| Venus | 3, then 4 | ≤ 163.7°, then < 179° |
| Earth | 5 | (for observers away from the Earth) |
| Mars | 6, then 7 | ≤ 50°, then beyond |
| Jupiter | 8, then 9 | ≤ 12°, then < 130° |
| Saturn | 10 (globe and rings), then 12 (globe only) | ≤ 6.5°, then 6°–150° |
| Uranus | 14, then 15 | ≤ 3.1°, then ≤ 154° |
| Neptune | 16, then 17 | ≤ 1.9°, then ≤ 133° after 2000 |

- **Saturn:** the rings' effective inclination is √(β_E β_S), from the
  planetocentric latitudes of the observer and the Sun over the ring plane.
  When the two are on opposite sides of the rings, it is 0.
- **Uranus:** the sub-observer and sub-solar latitudes are made
  planetographic (eq. 13, f = 0.0022927) and averaged in absolute value.
- **The poles** are the IAU WGCCRE 2015 report's (Archinal et al. 2018),
  as NAIF's `pck00011.tpc` carries them:
  - Saturn: α₀ = 40.589 − 0.036T and δ₀ = 83.537 − 0.004T;
  - Uranus: α₀ = 257.311 and δ₀ = −15.175.
- **Neptune's secular change (eq. 16):** V(1,0) is −6.89 before 1980, falls
  by 0.0054 a year to 2000, and is −7.00 after. The year is
  2000 + (JD_TT − 2451545)/365.25.
- **Mars:** the L(λₑ) and L(Ls) corrections for the sub-Earth longitude and
  the season are left out, as the port plan decided, which puts them at
  ±0.04 mag (an estimate).
- **No magnitude, with the reason named:**
  - a phase angle beyond a planet's formulas;
  - Pluto, which the paper does not cover;
  - the Earth–Moon barycentre.

**The Moon:** Krisciunas & Schaefer (1991), PASP 103, 1033, eq. 9, after
Allen (1976):

    V = −12.73 + 0.026|α| + 4×10⁻⁹ α⁴

The paper takes it at the Moon's mean distance and leaves the distance out,
which it says is worth up to ±0.12 mag. We scale by the inverse squares of
the actual distances, from 384,400 km and 1 au.

**Near new moon the quartic is unconstrained.** At α = 178° it gives −4.1,
for a Moon nobody can see, and `swetest` prints +2.0 there. The paper gives
the formula no range, and we do not invent one. Read a Moon magnitude beyond
about 150° as a number from outside the formula's data.

**The Sun:** Willmer (2018), ApJS 236, 47, Table 3: V = −26.76 (Vega) at
1 au, plus 5 log₁₀ d.

**Catalog bodies:** H, G (Bowell et al. 1989, *Asteroids II*), from the
record:

    V = H + 5 log₁₀(r d) − 2.5 log₁₀((1 − G) Φ₁ + G Φ₂)
    Φᵢ = exp(−Aᵢ tan(α/2)^Bᵢ), with A = 3.33, 1.87 and B = 0.63, 1.22

A record without G was written with the MPC's 0.15
(`tools/convert`). A record without H has no magnitude.

**Stars and deep sky** use the catalog's V. **Hypothetical bodies and orbit
points** have none.

**Radii** (km):
- the Sun: 695,700, the IAU 2015 Resolution B3 nominal value;
- the rest: the WGCCRE 2015 mean radii, which rise and set use too:

| Body | Mercury | Venus | Earth | Moon | Mars | Jupiter | Saturn | Uranus | Neptune | Pluto |
|---|---|---|---|---|---|---|---|---|---|---|
| Radius | 2439.4 | 6051.8 | 6371.0084 | 1737.4 | 3389.5 | 69,911 | 58,232 | 25,362 | 24,622 | 1188.3 |

A catalog body's diameter comes from its record, where it has one.

## Validation

`swetest` is used for its printed output only, run on our own DE440. There
are 47 rows: the Sun, the Moon and the planets at five instants (1980,
2000, 2026, 2044 and 2100), Mercury at α = 140.9°, and Venus at α = 171.0°.
The instants are TT, so that no ΔT enters. Measured 2026-10-27
(`tests/test_phenomena.cpp`, `tools/gen/gen_phenomena_fixtures.py`):

| | Largest difference |
|---|---|
| V: Mercury, Venus, Mars, Jupiter | 0.0006 (swetest prints 0.001) |
| V: Neptune | 0.0016 |
| V: Saturn | 0.014 |
| V: Uranus | 0.022 |
| V: the Moon, α ≤ 120° | 0.011 (ours brighter, consistently) |
| V: the Sun | 0.10, exactly: swetest's −26.86 at 1 au against Willmer's −26.76 |
| elongation, planets | 0.0001″ |
| elongation, the Moon | 0.001″ |
| diameter | 0.0001″, once swetest's radii are allowed for (696,000 km for the Sun and 1737.5 km for the Moon, against the IAU's 695,700 and 1737.4) |
| phase angle | 10″–39″, a difference of definition (below) |

**The phase angle** differs because `swetest` builds the angle in another way.
We compute it from the geometry at emission, as above. Mercury's curve is the
steepest, at 0.16 mag per degree near 170° and 0.11 below 150°. So even 39″
would move a magnitude by under 0.002, and on these rows the effect is under
0.001. Our Saturn and Uranus magnitudes follow
the paper's equations exactly. The 0.014 and 0.022 there are the size of
the ring and latitude terms, so the two programs evaluate those
differently. We have not looked into how `swetest` does it, and will not.

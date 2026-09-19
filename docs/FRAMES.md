# Frames: precession, nutation, sidereal time

`prometheia::frames` implements the ICRF → date-frames chain from a
single public-domain source: **USNO Circular 179** (Kaplan 2005, "The
IAU Resolutions on Astronomical Reference Systems, Time Scales, and
Earth Rotation Models"; `aa.usno.navy.mil/downloads/Circular_179.pdf`).
Every constant below is printed in that circular: the IAU 2006
precession polynomials (eq. 5.11), mean obliquity (eq. 5.12), the 14
fundamental arguments (eqs. 5.17–5.19, from Simon et al. 1994), the
series formulas (eqs. 5.15–5.16), ERA/GMST/GAST (eqs. 2.10–2.14), and —
uniquely — the complete 1365-term IAU 2000A (MHB2000) nutation series
printed in full at the end of the document. That table is regenerated
into `src/nutation_table.inc` by `tools/gen/gen_nutation_table.py`
(parsing a `pdftotext -layout` extraction); the generator validates the
table's own documented structure (678 lunisolar terms with zero
planetary multipliers, 687 planetary terms with zero rates, contiguous
term numbering).

## Conventions

Vectors are ICRF rectangular (equatorial J2000 axes); matrices are
row-major 3×3 applied as `r_out = M · r_in`, built from the axis
rotations

```
R1(a) = [1 0 0; 0 cos a  sin a; 0 -sin a  cos a]
R2(a) = [cos a 0 -sin a; 0 1 0; sin a 0 cos a]
R3(a) = [cos a sin a 0; -sin a cos a 0; 0 0 1]
```

so that `R1(ε)` converts equatorial components to ecliptic components.

## The chain (derived and verified, not transcribed)

- **Precession:** `P = R3(−z) · R2(θ) · R3(−ζ)` (ICRF → mean equator and
  equinox of date), IAU 2006 angles.
- **Nutation matrix:** `N = R1(−ε_true) · R3(−Δψ) · R1(ε̄)` (mean → true
  equator of date), derived from the classical construction: mean
  equator → mean ecliptic (`R1(ε̄)`), slide the equinox along the
  ecliptic (`R3(−Δψ)`), back to the true equator through the true
  equinox (`R1(−ε̄ − Δε)`). Note the circular's own typography for N
  drops glyphs in text extraction; this form is the one that checks out
  numerically.
- **True ecliptic of date:** `R3(−Δψ) · R1(ε̄) · P`. The ecliptic plane
  does not nutate — only the equinox slides along it — so Δε never
  enters ecliptic output: apparent longitudes are mean longitudes plus
  Δψ, latitudes unchanged. This is the classical statement in the
  circular and the Swiss Ephemeris convention.
- **Sidereal time:** ERA from UT1 (eq. 2.11 precise form), GMST = ERA +
  the accumulated precession-in-RA polynomial (eq. 2.12), GAST adds the
  equation of the equinoxes with the truncated complementary terms
  (eq. 2.14). Two time arguments by design: UT1 drives rotation, TT
  drives the equinox.
- **Topocentric:** WGS84 geodetic → geocentric, rotated into the
  true-equator-of-date frame by GAST. Polar motion (≤0.3″) neglected.
- **Ayanamshas** (`ayanamsa`, `ayanamsa_anchored`): the mean ayanamsha
  is the anchor value plus the IAU 2006 general precession in longitude
  p_A = 5028.796195″·T + 1.1054348″·T² + … (Capitaine et al. 2003)
  accumulated since the anchor epoch, measured on the ecliptic of date
  — the traditional sidereal realization; the true ayanamsha adds Δψ.
  Anchor instants and values, and the engine-side conventions:
  [ENGINE.md](ENGINE.md). Against swetest's `-ay<mode>` output the mean
  series differs only by SWE's precession model: 0.0026″ over
  1800–2200.

## Validation

All checks in `tests/test_frames.cpp`:

- Structure: matrices orthogonal to 1e-12 with det +1; P vanishes at
  J2000; fundamental arguments pinned to their J2000 values (l =
  134.9634°, Ω = 125.0445°, …); mean obliquity 84381.406″ (swetest
  prints 23°26'21.4060"); nutation at J2000 Δψ = −13.932″, Δε =
  −5.769″.
- **Differential fixtures from the installed Swiss Ephemeris**
  (output-only oracle; DE441-derived data). Geometric J2000-ecliptic
  positions of Sun and Moon at J2000 and at JD_TT 2461443.0, transformed
  by our chain, compared against SWE's own mean and true ecliptic-of-date
  outputs:
  - mean chain (IAU 2006 precession only): **0.00004″** worst,
  - true chain (+ full 2000A nutation): **0.00047″** worst — SWE's
    default nutation is a truncated series; the residual is the model
    difference, not an error,
  - GAST vs `swe_sidtime`: **0.0004″** (SWE returns sidereal time in
    *hours* — a classic trap; 16.22733953801 h here).
- The nutation-shift property (Δψ longitude shift, zero latitude shift)
  is asserted directly at four epochs.

## Nutation rates

`frames::nutation_with_rates` sums the IAU 2000A series together with its
analytic first and second time derivatives (each term differentiated
through its linear-in-T amplitudes and the fundamental arguments'
polynomials). Tests: the value equals `nutation()` to 1e-15 rad, the first
derivative matches central differences to 0.55 µas/day, and a
second-order Taylor step of up to 0.05 day reproduces the full series to
0.68 µas over 1800–2100.

`frames::NutationInterpolator` uses it to interpolate nutation from nodes
on a fixed half-day grid. The polynomial is the quintic Hermite matching
value, first and second derivative at both nodes, and it reproduces the
full series to 0.004 µas over 1800–2100. The measured maxima against node
spacing:

| spacing | quintic Hermite | cubic Hermite (no second derivative) |
|---|---:|---:|
| 1 d | 0.27 µas | 59 µas |
| 0.5 d | 0.004 µas | 3.9 µas |
| 0.25 d | 0.0001 µas | 0.24 µas |

Nodes are cached, direct-mapped, 4,096 of them. The engine uses it for
every nutation (docs/ENGINE.md).

## How the series is summed

Every term's argument is an integer combination of the same fourteen
fundamental arguments, so `nutation()` and `nutation_with_rates()` build the
sine and cosine of each multiple the table actually uses — at most 21 — once
per epoch, by angle addition, and compose each term's pair from its two to
six factors. The table is compacted to its nonzero multipliers as well,
which removes an inner loop of fourteen branches per term.

This is arithmetic rearrangement, not approximation. `nutation_printed_form`
keeps the series summed the way the circular prints it, one `sin` and one
`cos` per term, and exists only so `nutation_fast_form_matches_printed` can
hold the two against each other: they agree to 3e-20 rad (6e-9 µas) over
1600–2700, which is roundoff, and five orders below the 0.004 µas the
half-day interpolator already costs.

What it buys, per epoch: the series falls from 51 to 13 µs, and with rates
from 64 to 21 µs. That is the cost of a node, and the first body asked for
over a time window pays for every node in it while the rest ride free — so
it shows up as Jupiter's apparent place falling from 36 to 14 µs when it is
the body that pays, and a year-long segment fit of the Moon from 57 to
26 ms.

## Long-term precession (Vondrák, Capitaine & Wallace 2011)

IAU 2006 precession is a set of polynomials fitted near J2000; their errors
grow quickly beyond a few centuries. For epochs further out (DE441 spans
±13,000 years) the engine offers the long-term model of Vondrák,
Capitaine & Wallace (A&A 534, A22, 2011), selected per query with
`CalcOptions::precession = Precession::Vondrak2011` (C:
`PROMETHEIA_PRECESSION_VONDRAK2011`; `ephem --precession vondrak2011`).
IAU 2006 remains the default.

- **Model:** each primary parameter is a cubic polynomial plus 8–14
  periodic terms, fitted to IAU 2006 near J2000 and to numerical
  integrations over ±200 millennia: the ecliptic pole P_A, Q_A (Eq. 8,
  Table 1, with the corrigendum A&A 541, C1, 2012: Q_A C7 = 198.296701),
  the equator pole X_A, Y_A (Eq. 9, Table 2), and the general precession
  p_A (Eq. 10, Table 3). The tables were extracted programmatically from
  the article text, not retyped.
- **Matrix:** built from the two pole vectors (Eq. 23): rows = the equinox
  n̄ × k (normalized), n̄ × equinox, n̄ — J2000.0 mean equator and equinox
  to mean of date; the engine composes it with the frame bias as for IAU
  2006. The mean obliquity of date is the angle between the two poles, so
  the ecliptic pole of date lies exactly at latitude 90°. (The paper's
  separately fitted ε_A series agrees with that angle to a few arcseconds
  within ±4000 years, and drifts to hundreds of arcseconds at ±200
  millennia.) Sidereal zodiacs use the model's p_A for their drift.
- **Unchanged:** IAU 2000A nutation, and the IAU 2006 GMST polynomial for
  topocentric Earth rotation, which are themselves near-J2000 models.
- **Validation** (`tests/test_frames.cpp`, `tests/test_engine.cpp`):
  - at J2000 every series reduces to its IAU 2006 value to the published
    6-decimal rounding (≤ 1e-6″) — which the corrigendum is required for;
  - agreement with IAU 2006 (largest axis offset of the matrices):
    0.00003″ at ±10 yr, 0.0006″ at ±100 yr, 0.002″ at ±200 yr, 0.012″ at
    −500 yr, 0.056″ at ±1000 yr;
  - orthonormal to 1e-14 across ±200 millennia;
  - against swetest (whose default long-term precession is this model) on
    DE440 over 1800–2100: apparent ecliptic of date 2.5 → 1.7 mas,
    equator of date 3.1 → 1.9 mas with the option selected; the remaining
    ~2 mas are SWE conventions outside precession.

## Sidereal planes (protocol v4 A.8)

`CalcOptions::sidereal_plane` picks the plane a sidereal longitude is
counted along. The engine, `prometheiad` and the C ABI (version 6,
`prometheia_options.sidereal_plane`; `ephem --sid-plane`) expose it.

- **Ecliptic of date** (the default): the ayanamsha is subtracted as it
  moves, as above.
- **Ecliptic of the anchor epoch:** the position is rotated into the mean
  ecliptic and equinox of the zodiac's anchor epoch t0. Longitude is counted
  from A0 there, with no precession term. At t = t0 this equals plane 0 in
  the mean frame (tested to 1e-10°).
- **Invariable plane:** the unit normal to the total angular momentum
  of the Sun, the planetary-system barycentres (the Earth–Moon barycentre
  included) and Pluto, from DE440's states and GM constants at J2000.0.
  `frames::kInvariablePoleIcrf` is ICRF (0.026262993692212,
  −0.389990518230968, 0.920444268194584): inclination 1.578700° and node
  107.582322° on the J2000 ecliptic. It holds to 1e-9 over 1800–2200, so the
  mass set conserves it. `invariable_plane_from_de440` recomputes it when the
  DE440 file is present. The zodiac's zero point (longitude A0 on the mean
  ecliptic of t0) is projected onto the plane, and longitude is the angle
  about the pole from that projection. A test builds the projection
  independently from the public frame matrices and agrees to 1e-9°.
- Both fixed planes need ecliptic coordinates (the protocol's codec
  refuses a zodiac on the equator anyway), ignore the frame option, and
  report A0 as the ayanamsa. Rates rotate with the positions: the planes
  do not move.

**Against `astrolog-ephd`**, 2026-09-18: Sun, Moon, Mars, Jupiter and
Saturn at 1900, 2000 and 2026, for Fagan-Bradley and Lahiri.
- **Now** (Astrolog `284b321` onward, record `2026-09-18j`): all three
  planes agree.
  - Plane 0 and plane 1 agree to ≤ 0.003″.
  - Plane 2's longitude origin agrees to −0.005 … +0.003″.
- **Before**, while their planes 1 and 2 were Swiss's own plane options:
  - Plane 2 agreed in latitude to ≤ 0.03″, so the planes' orientations
    agreed.
  - But every longitude differed by a constant −31.51″ (Fagan-Bradley) or
    −30.42″ (Lahiri). The paragraph below is why.
  - Their first in-house version put the true ayanamsha of t0 on the mean
    ecliptic of t0, which was off by the nutation at t0 (record i).

The protocol said the zero point is "carried onto" the plane and did not
say how. The Astrolog side settled it by measurement on its own server
(ephv4 `9e9e5c4`). Fitting the rotation between its planes 0 and 2 over six
stars puts a different sky direction at 0° for each zodiac (+31.47″ for
Fagan-Bradley, +30.39″ for Lahiri). A plane cannot know which zodiac was
asked for, so an origin tied to the sky must be the same direction for
every zodiac, which is what projection gives (0 by construction). The
proposed §3.5a sentence adopts Prometheia's reading: "the zodiac's own
zero-point direction … projected onto the plane". Both maintainers approved
it on 2026-09-18. The Astrolog side changed its answers by ~31″; nothing
changed here.

## Zodiacs defined at the instant

Most published zodiacs are anchored at an epoch: a mean ayanamsha A0 at t0,
moved by precession. Eleven are defined by where something in the sky is at
the moment asked. The definitions come from the Swiss Ephemeris general
documentation (Astrodienst, published: sections 2.8.7–2.8.9, and 2.8.12
items 4 and 5; the documentation only, never its code). They are
`SiderealMode` values numbered as the protocol's A.11 tokens:

| mode | token | the anchor, at this sidereal longitude |
|---:|---|---|
| 27 | `true-citra` | Spica (α Vir, HR 5056) at 180° |
| 28 | `true-revati` | ζ Psc (HR 361) at 359°50′ |
| 29 | `true-pushya` | δ Cnc (HR 3461) at 106° |
| 35 | `true-mula` | λ Sco (HR 6527) at 240° |
| 17 | `galcent-0sag` | the Galactic Centre (Sgr A*) at 240° |
| 40 | `galcent-cochrane` | Sgr A* at 270° |
| 30 | `galcent-rgilbrand` | Sgr A* at the golden section of 0° Sco–0° Aqu, 244.3769° |
| 36 | `galcent-mula-wilhelm` | the ecliptic point on Sgr A*'s hour circle, at 246°40′ |
| 31 | `galequ-iau1958` | the galactic node (IAU 1958 pole) at 240° |
| 32 | `galequ-true` | the galactic node (Liu et al.'s pole) at 240° |
| 33 | `galequ-mula` | the galactic node (Liu et al.'s pole) at 246°40′ |

**The definition, as published:** the anchor's *true* position (no
aberration, no deflection) is held at that longitude on the **true ecliptic
and equinox of date**.
- It is a longitude, not a polar projection. The exception is Wilhelm's
  mode, which is polar by its definition: the point where the great circle
  through the celestial pole and Sgr A* meets the ecliptic.
- The galactic node is where the galactic equator crosses the ecliptic of
  date. Of the two crossings, it is the one near 0° Capricorn.
- The true ayanamsha is the anchor's longitude less the defined one. The
  mean ayanamsha is that less the nutation in longitude, as for the
  anchored modes. The J2000 and ICRF frames use the constant mean value at
  J2000.0 (protocol v4 §3.5a).

**Data**, each from a pinned source in `tools/fetch/stars_fetch.py`:
- **The stars:** this catalog (Hipparcos, the new reduction), moved by the
  same space-motion model as `calc_star`, barycentric.
- **Sgr A\*:** SIMBAD's ICRS position (Petrov et al. 2011, VLBI), taken at
  epoch J2000.0. Its apparent motion is from Reid & Brunthaler (2020):
  −6.411 mas/yr along the Galactic plane and −0.219 mas/yr toward the pole,
  turned into proper motion about the IAU 1958 pole.
- **The galactic poles:** from Liu, Zhu & Zhang (2011).
  - Their eq. 19 is the IAU 1958 pole carried into the ICRS.
  - Their eq. 22 is the pole of a system centred on Sgr A*. That is the
    "true/modern" pole: its node lies 190.3″ from the IAU 1958 node, and the
    documentation says the correction is 3′11″.

**Two readings were ours to make, and each was checked against a statement
in the documentation:**
- **Gil Brand's golden section:** of the two sections of the 90° from 0°
  Scorpio to 0° Aquarius, the one 90°/φ² from 0° Scorpio (244.3769°). That
  gives 22.47° at J2000, which is "very close to the ayanamsha of B.V.
  Raman"; the other section is not.
- **The modern pole:** see the 3′11″ check above.

A third statement holds without our doing anything: Wilhelm's mode (20.0411°
at J2000) and True Revati (20.0403°) are 3″ apart. The documentation says
that with the Galactic Centre in mid-Mula, Revati is "almost exactly" at
29°50′ Pisces.

**The fixed planes:** these zodiacs have no anchor epoch.
- **The ecliptic of the anchor epoch (plane 1)** is refused (ArgumentError,
  errCode 2 on the wire).
- **The invariable plane (plane 2):** the zero point is sidereal longitude 0
  on the mean ecliptic of the instant asked, projected onto the plane. For
  a zodiac defined at the instant, that is its definition rather than an
  approximation of one.
- This is the §3.5a clause proposed to both maintainers on 2026-09-18.
  The Astrolog side agreed the reading and is taking it to theirs as a
  text drop.

**Tested** (`tests/test_engine.cpp`):
- **Against ERFA**, by other routes (`tools/gen/gen_zodiac_fixtures.py`):
  rigorous `pmsafe` for the stars, the Galactic Centre's motion in galactic
  coordinates, ERFA's own ecliptic and nutation, and the Hipparcos
  catalogue's IAU 1958 pole. Worst 0.06 mas over 1900–2100. The IAU 1958
  node is within 0.012″, which is the milliarcseconds between the two
  published transfers of the pole, magnified by the node's shallow
  crossing.
- **Each star zodiac** puts its own star exactly at its defined longitude
  (to 1e-10″).
- **On the invariable plane**, a zodiac defined at the instant answers
  exactly as a user zodiac anchored at that instant with that instant's
  mean ayanamsha.

## Accuracy notes

- ICRF vs the J2000 mean equator/ecliptic differ by the ~0.02″ frame
  bias. `frame_bias_matrix()` provides B = R1(−η₀)·R2(ξ₀)·R3(dα₀)
  (dα₀ = −14.6 mas, ξ₀ = −16.617 mas, η₀ = −6.8192 mas); the date-frame
  matrices here exclude it and the engine composes M·B
  ([ENGINE.md](ENGINE.md)). Against swetest's `-icrs` vs `-j2000`
  outputs the composed chain agrees to 0.0001″.
- The equation-of-equinoxes complementary series is the circular's
  truncation (sub-microarcsecond for practical dates).
- Nutation evaluation costs 1365 sin/cos pairs (~10 µs); memoize per
  timestamp at the engine layer if that ever matters.
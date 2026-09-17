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
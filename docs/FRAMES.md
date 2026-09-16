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

## Accuracy notes

- ICRF vs the J2000 mean equator/ecliptic differ by the ~0.02″ frame
  bias; we ignore it (below any tolerance we promise; a later increment
  can add the B matrix).
- The equation-of-equinoxes complementary series is the circular's
  truncation (sub-microarcsecond for practical dates).
- Nutation evaluation costs 1365 sin/cos pairs (~10 µs); memoize per
  timestamp at the engine layer if that ever matters.
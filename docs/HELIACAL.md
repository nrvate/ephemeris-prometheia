# Heliacal visibility

When is an object first (or last) seen near the Sun? `heliacal::next`
(`include/prometheia/heliacal.hpp`) finds the heliacal rising, the heliacal
setting and, for planets and the Moon, the evening first and the morning
last. The C API is `prometheia_heliacal_next`, `prometheia_visibility_at` and
`prometheia_limiting_magnitude` ([C_API.md](C_API.md)); Python has
`Context.heliacal_next`, `Context.visibility_at` and `limiting_magnitude`
([PYTHON.md](PYTHON.md)).

| Event | Number | Meaning |
|---|---|---|
| rising | 1 | the first morning the object is seen before sunrise (morning first) |
| setting | 2 | the last evening it is seen after sunset (evening last) |
| evening first | 3 | the first evening it is seen after sunset |
| morning last | 4 | the last morning it is seen before sunrise |

The numbers are swetest's `-hev` types. A star has only 1 and 2 (3 and 4 are
an argument error). For the Moon, 1 and 2 follow the same rule as for a
planet (a run of mornings, or of evenings, on which it is seen) and say
little; 3 (the young crescent) and 4 (the old one) are the useful ones.
The object is any `prometheia_object` the registry resolves
([REGISTRY.md](REGISTRY.md)); the Sun and the Earth are refused.

## What is computed

For each day from the start date the finder asks whether the object is seen
at the best moment of the morning twilight (events 1 and 4: the 3.5 hours
before the Sun's apparent rise) or of the evening twilight (events 2 and 3:
the 3.5 hours after its set). The best moment is the largest margin, found
from 16 samples and a golden-section search. The days are tested four apart
(one apart for the Moon) and the first (events 1, 3) or last (2, 4) day of a
run is then found a day at a time. The event is the start of the visibility
window on that day, to a second; `optimum` is the best moment and `end` the
window's end. The start date is strict: the event is after it.

Times are TT with UT1 beside them (the engine's Delta T). Positions are
topocentric and apparent, the altitude refracted by Bennett's formula with the
atmosphere's pressure and temperature, the Sun's by its true altitude.
Limits: the 3.5-hour window and the sunrise root-finder mean that at high
latitudes in summer, where the Sun hardly sets, a day may be reported unseen
(no event is invented); the finder gives up after `search_days` (800 by
default) with `NOT_FOUND`; it can miss a run shorter than four days (not
seen in the measurements below).

## The Schaefer model

Source: B. E. Schaefer (1993), "Astronomy and the limits of vision", Vistas in
Astronomy 36, 311-361 (read from the open scan at uai.it; every number below
was read from its page images, and "eq." is the paper's). A star or planet
is seen when its magnitude at the eye, V plus the extinction, is brighter
than the limiting magnitude of the sky behind it, less a margin:

- **Air mass.** eq. 2b (Rozenberg) for the whole atmosphere; eq. 3a for the
  gas (scale height 8.2 km) and aerosol (1.5 km) layers; eq. 3b for the ozone
  shell (20 km, R = 6378 km).
- **Extinction.** k = k_R + k_oz + k_a (eq. 7): k_R = 0.1066 e^(-H/8.2)
  (4b); the ozone depth D = 3.0 + 0.4(phi cos alpha_s - cos 3 phi) and
  k_oz = 0.031 D/3.0 (5a, 5b); the aerosol
  k_a = 0.12 (0.55/lambda)^1.3 e^(-H/1.5) [1 - 0.32/ln S]^(4/3) [1 + 0.33 sin
  alpha_s] (6a), S the relative humidity in percent and alpha_s the Sun's
  right ascension. The object's dimming is eq. 8b, k_R X_g + k_oz X_oz + k_a
  X_a. Night vision (0.51 um) uses the factors the paper gives: Rayleigh
  x1.35, ozone x0.3, aerosol x1.10 (its section 3.17); the sky takes them
  when its brightness is below 1500 nL (the paper's day/night vision boundary,
  Table 6).
- **Brightness.** eq. 10, V = -16.57 - 2.5 log I (I in foot-candles).
- **Sky.** eq. 14 (the scattering function) and eqs. 15a-e: night
  (B_o = 180 nL, "a typical value for a dark site"), moonlight, twilight, day
  and their sum. The Moon's V is the engine's (to 120 deg of phase; beyond it
  there is no Moon term).
- **Threshold.** The observer's pupil from his age (32) against the standard
  observer's (33); the correction factors F_b (31a, b: monocular is a factor
  sqrt 2), F_a (31e), F_sc (31i, 31l) and F_r (31f: S^-2 for a point source),
  combined by eq. 29-30; Hecht's threshold (34 a-c) with c and k by log B
  against 3.17.

`limiting_magnitude(B, observer)` is that chain for a uniform sky of B
nanoLamberts: the faintest V seen at the eye before extinction.

### Where the paper's text is ambiguous or incomplete

Each of these is a decision the code makes, in the order they matter:

1. **The experience correction.** The paper says the thresholds need
   "corrections ... for an observer's experience" and refers to two other
   papers, without a number. The paper's own threshold (50 % detection,
   "highly-experienced young observers") finds events 3 to 9 days early
   against swetest (below). `detection_margin_mag` is the margin by which the
   object must stand above the threshold; **its default 1.2 magnitudes is not
   from the paper**: it is the one constant fitted to swetest's printed
   events (of 1.0, 1.2, 1.4 and 1.6 the one that agreed best over the star
   cases; the fit is coarse). `0` is the paper.
2. **B_day below the horizon.** Eqs. 2b and 3a are not defined for a zenith
   distance past 90 deg, and with the horizon's air mass (40) eq. 15d is a
   hundred times fainter than the paper's own sunset sky (Table 6). With the
   Sun on or below the horizon the twilight term (15c) stands alone; above it
   the paper's min(B_twi, B_day) applies.
3. **Eq. 15c's separation term**, `max{1, dex[(rho_sun/90 deg) - 1.1]}`, is
   implemented as printed. It is 1 for the Sun's neighbourhood and rises
   beyond 99 deg, which is opposite to the sky's actual glow near the Sun;
   it was not changed.
4. **Naked eye.** The telescope factors are 1; the aperture D of eqs. 31e,
   31i, 31l is the observer's pupil (eq. 32); F_p is 1. Eq. 31d as printed
   (`P_st / M P`) is a telescope's exit-pupil factor and has no meaning for
   the unaided eye. The colour factor F_c (31m) needs a [B-V] the engine does
   not give; it is 1, as in the paper's own worked example (5.80 below).
5. **Point sources.** Planets and the Moon are points (the critical visual
   angle of eq. 27, whose scan is garbled, only enters through the source's
   size). Venus and the Moon are bright and extended; their agreement below
   is the worst.
6. **Near the horizon** eq. 15a breaks down (the paper's section 3.17): the
   night sky is evaluated at Z = 80 deg at most, the value the paper uses
   there.
7. **Moon term.** Zero with the Moon below the horizon or with no magnitude.
   The finder's default leaves moonlight out (`moonlight`: off), so that it
   finds the object's own heliacal event rather than a night the Moon
   spares; `visibility_at` takes the same switch.
8. **Table 6** lists "zenith for dark sky 20" and "darkest ever observed 54"
   nL, which cannot both stand beside B_o = 180 nL in the text; B_o = 180 is
   taken, and exposed as `dark_sky_nl`.
9. **Visual range** is not in the paper. A meteorological range VR is
   converted by Koschmieder's beta = 3.912/VR per km: the Rayleigh part is
   taken off and the rest, over the 1.5 km aerosol layer, is the aerosol
   extinction. A given total k is the day-vision total; the aerosol is what
   is left after eqs. 4b and 5b. Humidity below 5 % is held at 5 %, because
   eq. 6a's bracket leaves its range below 1.4 %.
10. **No glare, no city light, no instrument** (eqs. 16, 17, B_city = 0).

## Arcus visionis

The object is seen when the Sun is at least the arc below the geometric
horizon and the object at least `object_altitude_deg` (default 0) above the
apparent one; the margin is the lesser of the two excesses, in degrees. The
**arc is the caller's: `arcus_visionis_deg` has no default** and NaN is
refused. The classical arcs are Ptolemy's, from the *Phases of the Fixed
Stars* and Almagest XIII (Toomer's translation), by planet and by magnitude
class, and Schoch (1924, MNRAS 84, 731), but I could not read those
sources or find a citable table of them in the sources reachable here, so
none is shipped (nor any number from memory). A table can be added when it is
read.

## Validation

All measured (2026-10-28, DE440, Release).

**(a) Schaefer's own numbers** (`tests/test_heliacal.cpp`):

| Check | Paper | Here |
|---|---|---|
| Table 3, air masses 2b, 3a (8.2, 1.5 km), 3b, Z = 0..90 | to 3 figures | worst 0.93 % (the table's rounding) |
| Table 5, V against foot-candles (eq. 10), six rows | | worst 1.8 % in I |
| limiting V at B = 220 nL (section 3.17, eqs. 10, 34) | 5.80 | 5.794 (standard eye, age 25) |
| limiting V at 1500 nL (Table 5/6, day vision limit) | 4.1 | 4.21 |
| Aldebaran: X at 0.2 deg (2b) | 36 | 36.3 |
| Aldebaran: k_R, k_oz at 2940 m, night vision | 0.101, 0.010 | 0.1005, 0.0111 |
| Aldebaran: A_m at 0.6 deg (eq. 51 with the paper's k_a) | 4.95 | 4.976 |
| zenith sky, Sun at 0, -6, -12 deg (Table 6) | 3e7, 1e5, 300 nL | 5.6e7, 2.2e5, 1121 nL |

The sky brightness is within a factor 1.9 at sunset, 2.2 at civil and 3.7 at
nautical twilight (a factor, not a percentage: the table says "zenith" and
the formula has a haze factor that depends on k); the rest agree to the
table's precision. Schaefer 1987 (JHA Suppl. 18, S19) was not read: not
attempted, so none of its tabulated values is reproduced.

**(b) swetest `-hev`** (43 cases, DE440, tools/gen/gen_heliacal_fixtures.py,
atmosphere printed beside each in `tests/heliacal_fixtures.inc`). Differences
are ours less swetest's, in days of the event's UT1; the default margin 1.2
and, in brackets, the paper's 0:

- all 43 cases: 22 within 1 day (1), 33 within 3 days (11);
- the 29 star cases (12 stars, six sites, and Sirius at Cairo in five
  atmospheres): 20 within 1 day (1), 26 within 3 (9);
- the 24 star cases in the default atmosphere: 23 within 3 days, the worst
  Fomalhaut's rising at +4.0; the optimum moment agrees to within 8 minutes
  except Fomalhaut (34, 21);
- the recorded dates: Sirius at Cairo rises on 2026-08-06 (ours -1.0 day;
  -3.0 with margin 0), Venus at Berlin rises on 2026-11-02 (-2.0) and sets on
  2028-05-23 (+3.0);
- Sirius at Cairo, which moves with the atmosphere in swetest by +4 days at
  80 % humidity and +20 at a 10 km visual range: ours moves +1 and +10, and
  is 3.99 and 2.00 days off at 80 % and at 60 km, 11.0 days off at 10 km;
  a total coefficient of 0.35 gives +1.0 here against no change in swetest;
- planets: Mercury, Saturn, Jupiter and Mars are -3..+5 days apart, except
  Mars's setting (-33); Venus's evening first and morning last are -13 and
  +11 days, the Moon's +4 and -3.1. These are the model's extended-source
  and high-latitude weaknesses (Venus at 52 N), not found to have a
  smaller cause.

swetest here is a black box; its algorithm is unknown to me, so "disagrees"
does not say which is right. Where it sits among observation is not
measured.

**(c) Limiting-magnitude spot checks**: the rows of (a) above.

**(d) The historical Sirius rising** (Censorinus, 139 CE at Alexandria,
Julian 20 July): the finder run on DE440 with the DE441 cut
`linux_m3000p1550.441` behind it (docs/DE.md) gives JD 1772027.606, +0.1 days
from 20 July, with margin 0, and +2.1 days with the default 1.2
(`heliacal_censorinus_sirius_139_ce`, skipped by default because the cut is
423 MB and not in every checkout; run with `--no-skip`). The date is a
historical claim and not a calibration: the 1.2 was fitted on swetest's
2026 output alone.

**Cost.** The default test set (selected events, arcus visionis, the C API,
the checks of (a)) runs in 0.8 s in Release.

## Not done

Telescopes, glare and the experience correction (eqs. 16-33 beyond the naked
eye); extended-source thresholds (eqs. 35, 36) for Venus and the Moon; the
1987 paper's tables; Ptolemy's arcs; a default arc. A star's colour index.

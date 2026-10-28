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
  and their sum, with the twilight term as Schaefer revised it in 1998 (item 3
  below). The Moon's V is the engine's to 120 deg of phase and the paper's
  eq. 12 beyond it (-12.73 + 0.026|a| + 4e-9 a^4, at the mean distance; Table 5's
  crescent at a = 135 deg, -7.89, is reproduced), because the young and old
  crescents a heliacal event needs lie past 120 deg.
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
   object must stand above the threshold. **Its default is 0, the paper's
   threshold** (the maintainer's decision, 2026-10-28). A caller who wants
   an experience correction supplies it; 1.2 brings the stars closest to
   swetest (below), a number fitted to swetest, not from any source.
2. **B_day below the horizon.** Eqs. 2b and 3a are not defined for a zenith
   distance past 90 deg, and with the horizon's air mass (40) eq. 15d is a
   hundred times fainter than the paper's own sunset sky (Table 6). With the
   Sun on or below the horizon the twilight term (15c) stands alone; above it
   the paper's min(B_twi, B_day) applies.
3. **Eq. 15c is misprinted, and Schaefer's 1998 form is used.** The page
   image reads `max{1, dex[(rho_sun/90 deg) - 1.1]} 10^(8.45 + 0.4 h_sun)`.
   The separation factor is 1 everywhere within 99 deg of the Sun and grows
   beyond it, so the twilight sky beside the Sun would be no brighter than
   the sky across from it. Schaefer's later model ("To the Visual Limits",
   Sky & Telescope 95(5), 57-60, May 1998: the twilight brightness of its
   published listing, V band) has the sky brighter toward the Sun as
   100/rho_sun and toward the horizon:
   B_twi = dex(8.2307 + 0.4 h_sun + Z/(900 k)) (100/rho_sun) (1 - dex(-0.4 k X))
   nL. Here 8.2307 = -0.4(M_sun - M_0 + 32.5) - log 1.11e-15, with that
   article's V-band M_sun = -26.74 and M_0 = -11.05, and its 1.11e-15 to
   nanoLamberts. This is the term used; rho_sun is held at 1 deg or more.
   Against the 1993 printing, both at margin 1.2, it moved Venus's evening
   first and morning last from -13 and +11 days to +1 and -1, and Mars's
   setting from -33 to -72. Mars's margin falls slowly while it sets, so its
   date answers the margin more than any other case here: +21 at margin 0,
   -72 at 1.2. At the default margin 0 the numbers are in (b).
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
7. **Moon term.** Zero with the Moon below the horizon.
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
| zenith sky, Sun at 0, -6, -12 deg (Table 6) | 3e7, 1e5, 300 nL | 3.7e7, 1.5e5, 798 nL |

The sky brightness is within a factor 1.25 at sunset, 1.5 at civil and 2.7
at nautical twilight (a factor, not a percentage: the table says "zenith" and
the formula has a haze factor that depends on k); the rest agree to the
table's precision. Schaefer 1987 (JHA Suppl. 18, S19) was not read: not
attempted, so none of its tabulated values is reproduced.

**(b) swetest `-hev`** (43 cases, DE440, tools/gen/gen_heliacal_fixtures.py,
atmosphere printed beside each in `tests/heliacal_fixtures.inc`). Differences
are ours less swetest's, in days of the event's UT1, with the default margin
0; in brackets, the spread (below), days before/after:

- all 43 cases: 5 within 1 day, 25 within 3 (margin 1.2: 17 and 35);
- the 24 star cases in the default atmosphere: risings 1 to 6 days early,
  settings 1 to 6 days late, Sirius at Cairo -1.0 and +2.0, the worst
  Fomalhaut's and Capella's risings (-6.0) and Regulus's setting (+6.0); each
  spread 0 to 2 days;
- Sirius at Cairo's rising in other atmospheres: -5.0 at 80 % humidity,
  -14.0 at a 10 km visual range, -2.0 at 60 km, 0.0 with a total
  coefficient of 0.35, -1.0 at 900 hPa and 20 %;
- planets: Mercury -3.0 and +1.0, Jupiter -2.0 and +3.0, Saturn -5.0 and
  +3.0, Venus -1.0 and +1.0 (rising, setting) and -7.0 and +10.0 (evening
  first, morning last; spread 2/1 and 2/2), Mars -11.0 and +21.0 (spread 3/3
  and 5/4); the Moon 0.0 (evening first) and +1.0 (morning last).

Every difference has one sign: our risings come early and our settings late,
so this implementation sees an object at a fainter margin than swetest does.
Dividing each difference by its spread gives the size of that offset in
magnitudes. It is about 0.3 mag for the stars, about 0.8 for Saturn and for
Venus's evening first and morning last, and about 1 for Mars. swetest is a
black box here, and its algorithm is unknown to me, so the offset does not
say which is nearer the sky. The one observation checked (d) fits margin 0.

### How firm the date is

`Found::spread_before_days` and `spread_after_days` (C:
`prometheia_heliacal`, Python: `Heliacal`) give the number of days the event
moves with the threshold `kSpreadMag` = 0.2 magnitude easier or harder, at
most 90 each way:
- for a first sighting, the run of days within 0.2 mag below the threshold
  before it, and the days to the first margin 0.2 mag above it;
- mirrored for a last sighting.
This is a sensitivity of the model, not an observed uncertainty. 0.2 mag is a
round figure: Schaefer gives his sky brightnesses and extinctions as
typically 20 % accurate (1998), and that much in either moves a threshold by
a tenth of a magnitude or more. Sirius's rising at Cairo is firm to a day;
Mars's setting at Athens to five.

### The planet and Moon misses, investigated

Under the printed eq. 15c with margin 1.2, Mars's setting was -33 days and
Venus's evening first and morning last -13 and +11. Each check below varied
one thing and held the rest:
- **A 1-day step instead of 4** (a short run skipped): identical. Ruled out.
- **The window, 3.5 h or 6 h** around sunrise or sunset: identical but for
  Mercury's setting (+0.01 d) and the Moon's (-0.04 d). Ruled out.
- **Magnitude and phase passed in**, ours against swetest's printed V at its
  event: Venus -3.89 and -3.96 (swetest -3.893, -3.960), Mars 1.25 (1.249),
  Jupiter -1.80 (-1.805). Ruled out.
- **The branch of the last-type events**: the margin falls through zero on
  the day found. Ruled out.
- **The Moon: a defect, fixed.** The engine's Moon V is NaN past 120 deg of
  phase, so the young and old crescents were declared unseen: +4.0 and -3.1
  days. With the paper's eq. 12 beyond 120 deg they are 0.0 and +1.0 now.
- **The twilight term: the cause.** The page image confirms the printing of
  eq. 15c (item 3 of the list above). Under it the sky near the Sun carried no
  extra glow, and the margin of a planet low in bright twilight sat near zero
  for weeks: Mars's changed 0.01 mag a day. With Schaefer's 1998 term the
  margins move a few tenths of a magnitude a day, the spreads are days, and
  what is left is the one-signed offset of (b). Mars stays the most
  sensitive case (item 3 of the list above).
- **The colour factor** (eq. 31m) applies only to night vision (B below
  1500 nL); heliacal events are seen in brighter twilight, where the paper
  sets it to 1. Not a cause.

**(c) Limiting-magnitude spot checks**: the rows of (a) above.

**(d) The historical Sirius rising** (Censorinus, 139 CE at Alexandria,
Julian 20 July): the finder run on DE440 with the DE441 cut
`linux_m3000p1550.441` behind it (docs/DE.md) gives JD 1772028.603, +1.1 days
from 20 July, with the default margin 0, and +3.1 days with 1.2
(`heliacal_censorinus_sirius_139_ce`, skipped by default because the cut is
423 MB and not in every checkout; run with `--no-skip`). The date is a
historical claim, one observation, and not a calibration.

**Cost.** The default test set (selected events including the Moon, arcus visionis, the C API,
the checks of (a)) runs in 0.8 s in Release.

## Not done

Telescopes, glare and the experience correction (eqs. 16-33 beyond the naked
eye); extended-source thresholds (eqs. 35, 36) for Venus and the Moon; the
1987 paper's tables; Ptolemy's arcs; a default arc. A star's colour index.

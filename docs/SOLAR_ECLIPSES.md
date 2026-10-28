# Solar eclipses

`include/prometheia/solar_eclipse.hpp`, `src/solar_eclipse.cpp`. Three
calls, A-7 of astromcp's port plan: the next eclipse (global
circumstances), the shadow axis on the ground at an instant (where), and
what one site sees (local). C interface in [C_API.md](C_API.md), Python in
[PYTHON.md](PYTHON.md).

## Method

Everything rests on the Besselian elements x, y, d, mu, l1, l2, tan f1 and
tan f2 (Explanatory Supplement to the Astronomical Almanac, 3rd ed., 2013,
eclipse chapter; Meeus, *Elements of Solar Eclipses 1951-2200*, 1989),
computed at any instant straight from the engine rather than from a
fitted polynomial:

- the geocentric apparent Sun and Moon (`Engine::calc`, true equator and
  equinox of date, light time, deflection and aberration on) give the
  shadow axis, the line from the Moon's centre to the Sun's. Its direction
  is (a, d); x and y are the Moon's coordinates in the fundamental plane
  through the Earth's centre perpendicular to it, in WGS84 equatorial
  radii (6378.137 km); mu is the apparent sidereal time minus a;
- sin f1 = (R_sun + k_p a) / D and sin f2 = (R_sun - k_u a) / D with D the
  Sun-Moon distance, l1 = z tan f1 + k_p / cos f1 and
  l2 = z tan f2 - k_u / cos f2 (z the Moon's height above the plane); l2 < 0
  means the umbra reaches the plane (total);
- **greatest eclipse** is the minimum of sqrt(x^2 + y^2), NASA's
  definition, and **gamma** is that minimum, signed by y;
- **class**: partial, total, annular or hybrid (l2 at the axis' ground point
  changes sign along the central phase, sampled 25 times); **central** or
  **non-central** by whether the axis meets the WGS84 ellipsoid, tested on
  the ellipsoid's outline in the fundamental plane (semi-axes 1 and
  sqrt(sin^2 d + (1-f)^2 cos^2 d));
- **where**: the line parallel to the axis through the plane point (x, y)
  is intersected with the ellipsoid, sunward root; that is the central
  line at the instant. The umbra's limits are the points on the line
  perpendicular to the shadow's motion over the ground at which the
  distance from the axis equals |L2|; the path width is the chord between
  them, the central duration 2|L2| / (relative speed);
- **local**: the observer's xi, eta, zeta from the site's geocentric vector;
  L1 = l1 - zeta tan f1, L2 = l2 - zeta tan f2, m = distance of the axis
  from the site in the plane. Local maximum is the minimum of m; contacts
  C1/C4 are the roots of m = L1, C2/C3 of m = |L2|. Magnitude is
  (L1 - m) / (L1 + L2), the fraction of the Sun's diameter covered;
  obscuration is the overlap area of the two discs, of radii (L1 + L2)/2
  and (L1 - L2)/2, over the Sun's. The Sun's altitude is the geometric
  altitude of its centre from the site (no refraction). A contact with the
  Sun's centre below the horizon is reported as **missing** (NaN, None); the
  maximum is always reported with `visible_max`. `local_next` skips
  eclipses of which no phase is seen with the Sun up;
- **times** are TT and UT1 (Delta T from the engine's model: the observed
  default, or the one installed with `prometheia_engine_set_delta_t`).
  Inputs `jd_start` and `jd_tt` are TT.

Search: each lunation is brought to conjunction in ecliptic longitude
(Newton on the Moon's and Sun's rates), its latitude screened (1.9 deg), the
minimum of x^2 + y^2 found by golden section to 2 ms, and the eclipse kept
if the axis passes within l1 of the Earth's outline. Contacts are found by
Illinois root-finding to 2 ms: P1/P4 penumbral limb contact, U1/U4 umbra
touching the limb, C1/C2 axis entering and leaving the ellipsoid (named
`c1`, `c2` in `Global`; swetest calls U1/U4 its total-phase begin and end).

### The Moon's radius

`Options::k_umbra = 0.272281` and `k_penumbra = 0.272488` are NASA's
canon values (also Meeus'), the Sun's radius is 696000 km. Both are options
(C: `prometheia_solar_eclipse_options`, Python: `k_umbra=`, `k_penumbra=`,
`sun_radius_km=`). swetest uses 0.2725076 throughout, which gives its
magnitudes: with that value we reproduce them (below), with NASA's we are
0.0009 lower on 2026-08-12. **No limb profile**, as in the NASA canon: the
Moon is a circle. Path edges and contact times are therefore good to the
1-2 km that profile ignored (about a second of contact time on a graze);
that is the model's limit, not a measured one.

### Magnitude

Global `magnitude` is the canon's: the fraction of the Sun's diameter
covered for partial and non-central eclipses, the ratio of the Moon's to the
Sun's apparent diameter for central ones (total 1.0386 and not 1.0193 on
2026-08-12). `magnitude_covered` is the fraction covered in every case.
Local `magnitude` is the fraction covered (swetest's number 2). Obscuration
is the true area fraction; swetest's third number is not (it prints 1.0806
for a total eclipse).

### Saros

Van den Bergh's numbering. Members of a series are 223 lunations apart; the
series number advances by 38 per lunation (and 5 per 6 lunations, which is
what moves an eclipse season to the other node) modulo 223:

    saros = (38 k + 112) mod 223,  mapped to -20..202

with k NASA's lunation number (0 at the new moon of 2000-01-06). The offset
is pinned by the total eclipse of 2017-08-21 (k = 218), published as
saros 145 by NASA (Espenak, Five Millennium Canon of Solar Eclipses). The
formula reproduces the saros number of all 11,898 canon eclipses (-13 to
190, inside the -20..202 window the 223 residues are mapped to; a series
outside that window would be reported 223 off).

## Measured agreement

DE440, the whole of 1900-2099 unless said otherwise.

### NASA Besselian elements (the oracle for x and y)

`tools/fetch/solar_eclipse_fetch.py` fetches NASA's CSV of all 11,898
eclipses (5,952,576 bytes, sha256 44460be3...2b44, dated 2024-03-25 by the
server) and the Five Millennium Canon catalog (1,333,311 bytes, sha256
9967c3ae...a344); the tests read them through `PROMETHEIA_SOLAR_BESSELIAN`
and `PROMETHEIA_SOLAR_CANON` and skip when unset. Against the CSV's
polynomials at t0 and t0 +- 3 h for the 454 eclipses of 1900-2100 (1362
samples):

| quantity | max | rms |
| --- | --- | --- |
| sqrt(dx^2 + dy^2) | 3.1e-5 Earth radii | 1.0e-5 |
| d | 4.7e-5 deg | |
| l1, l2 | 6.3e-6 | |
| tan f1, tan f2 | 3.0e-7 | |

The l1, l2 agreement shows that NASA's k and Sun radius are the ones above.
(mu is not compared: NASA's uses its own Delta T.) Target was 1e-4.

### NASA Five Millennium Canon catalog (global circumstances)

All 452 eclipses of 1900-2099 found; none missing, none extra; the kind
(T/A/H/P), the central/non-central flag and the saros number agree on all
452. Greatest eclipse time: max 0.54 s, median 0.24 s (TT vs the catalog's
TD). Gamma: max 0.00006. Magnitude (NASA's convention, NASA's k): max 0.00008.
Greatest-eclipse position: 0.134 deg at worst for central eclipses (the
catalog prints 0.1 deg; the worst cases are near the limb at 60-70 deg
latitude, where the position is ill-conditioned) and 0.209 deg at worst for
partial and non-central ones, whose published latitudes sit about 0.2 deg
equatorward of ours at 61-72 deg latitude (the size of geodetic minus
geocentric latitude there; the canon's partial points may be computed on a
sphere, not verified). The target of 0.1 deg is therefore met for the
central eclipses to the catalog's resolution and not for the grazing ones.

### swetest -solecl and -local (UT)

swetest on the same DE440; `tests/test_solar_eclipse.cpp` holds the numbers.
Global, ours minus swetest, with NASA's k:

| eclipse | max | partial begin | partial end | mag NASA/ours | position |
| --- | --- | --- | --- | --- | --- |
| 2026-02-17 annular | +3.4 s | -20 s | +2 s | 0.9630 / 0.9638 | 0.33 deg lon at 65S |
| 2026-08-12 total | -2.6 s | -18 s | +1.5 s | 1.0386 / 1.0395 | 0.07 deg |
| 2027-08-02 total | -0.25 s | -7 s | +2 s | 1.0790 / 1.0800 | 0.01 deg |

swetest's time of maximum is the least geocentric Sun-Moon angle, not the
least axis distance; its partial phases differ in definition by seconds.
Umbral begin (our U1) agrees with its second time to 0.4-13 s; its central
duration is within 0.3 s of ours with NASA's k (2027-08-02: 382.44 s ours,
382.56 swetest) and 2.7-4.2 s away with swetest's k, so swetest does not
compute it from its own radius. With k = 0.2725076 the magnitudes agree:
0.9638, 1.0395, 1.0799 against 0.9638, 1.0395, 1.0800, and obscuration of
the annular eclipse 0.9288 both. swetest's fraction-covered magnitude
differs from ours by up to 0.002 (it is not defined the same way at the
greatest-eclipse point). Local, 2026-08-12 with swetest's k:

| site | maximum | C1 | magnitude | obscuration |
| --- | --- | --- | --- | --- |
| Berlin | -0.40 s | -0.24 s | 0.8743 = 0.8743 | 0.8488 = 0.8488 |
| Madrid | -0.27 s | -0.20 s | 0.9994 = 0.9994 | 0.9998 / 0.9999 |

C4 is missing at both (the Sun sets first), as swetest prints no C4.
With NASA's k the magnitudes are 0.8739 and 0.9989.

## Limits and what is not done

- No limb profile; no refraction in altitudes or in the horizon test
  (geometric altitude of the Sun's centre above 0).
- Polar motion is neglected (as in `frames`), 0.3 arcsec at most.
- Hybrid eclipses are classed by the sign of l2 along the central phase; the
  canon's sub-types (A+, Tm, Ts, H2...) are not reproduced, only the
  central/non-central distinction.
- The search needs the planetary ephemeris at the instants it visits; DE440
  covers 1550-2650, a DE441 cut earlier.
- The custom Delta T of `prometheia_engine_set_delta_t` is honoured through
  the C interface; through the C++ API it is `Options::delta_t`.

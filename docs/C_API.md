# C interface

`include/prometheia/prometheia.h` (implementation `src/c_api.cpp`) is the
C ABI over [`prometheia::Engine`](ENGINE.md), for C programs and for
language bindings (Python ctypes/cffi, Rust, C#, …). It is a shim: it
translates options and units, checks arguments and contains exceptions,
and adds no behaviour. Every answer is bit-identical to the C++ engine
given the same inputs, and the tests enforce that.

**Cross-repo contract (2026-09-17):** Astrolog's phase-7 Prometheia
plugin is built against this document with pkg-config detection
(trajectory note from the Astrolog ephv4 session; their Status records
the approval). Treat changes to this ABI as a protocol change: they are
relayed to the Astrolog session through the maintainer before they land,
never shipped unannounced.

```c
#include <prometheia/prometheia.h>

prometheia_error err;
prometheia_engine *eph = NULL;
if (prometheia_engine_open("ephe/linux_p1550p2650.440", &eph, &err) != PROMETHEIA_OK) {
    fprintf(stderr, "%s\n", err.message);
    return 1;
}
prometheia_engine_add_ephemeris(eph, "linux_m13000p17000.441", &err); /* optional: DE441 outside 1550-2650 */
prometheia_engine_add_catalog(eph, "sbdb.epm", &err);   /* optional: small bodies */
prometheia_engine_add_perturbers(eph, "sb441-n16.bsp", &err); /* optional: asteroid masses */

double jd_tt;
prometheia_utc_to_tt(2026, 9, 16, 12, 0, 0.0, &jd_tt, &err);

prometheia_options opts;
prometheia_options_init(&opts);             /* apparent, true ecliptic of date */
opts.sidereal = PROMETHEIA_SIDEREAL_LAHIRI;

prometheia_result r;
if (prometheia_calc(eph, PROMETHEIA_MOON, jd_tt, &opts, &r, &err) == PROMETHEIA_OK)
    printf("%.6f %.6f %.9f  (%s)\n", r.lon_deg, r.lat_deg, r.dist_au, r.source);

int ceres;
if (prometheia_engine_lookup(eph, "Ceres", &ceres, &err) == PROMETHEIA_OK &&
    prometheia_calc(eph, ceres, jd_tt, NULL, &r, &err) == PROMETHEIA_OK &&
    (r.flags & PROMETHEIA_HAS_SIGMA)) /* catalog record carries a covariance */
    printf("Ceres sigma %.3f\"\n", r.sigma_arcsec);

/* The Moon's osculating ascending node, and positions seen from Mars. */
prometheia_calc_orbit_point(eph, PROMETHEIA_MOON, PROMETHEIA_ORBIT_ASCENDING_NODE,
                            PROMETHEIA_ELEMENTS_OSCULATING, jd_tt, NULL, &r, &err);
opts.center = PROMETHEIA_CENTER_BODY;
opts.center_body = PROMETHEIA_MARS;
prometheia_calc(eph, PROMETHEIA_EARTH, jd_tt, &opts, &r, &err);

prometheia_engine_close(eph);
```

**Which years the loaded files answer** (added after 0.7.3; ABI version
unchanged, as no layout changed). `prometheia_engine_ephemeris_count` and
`prometheia_engine_ephemeris_span(engine, i, &first_jd_tdb, &last_jd_tdb,
&source, &err)` give each planetary file in the order it answers, with the
span it covers. A cut DE file covers whole 32-day records, so its span can
start or end a few days outside the years in its name. DE440 with
`linux_p1000p1550.441` behind it reports JD 2287184.5 .. 2688976.5 and
2086288.5 .. 2287184.5: the cut starts 19 days before 1000-01-01. The
`source` string lives until the next call of `_span` or
`prometheia_engine_add_ephemeris()`.

**Which catalogs are loaded** (added after 0.7.3; ABI version unchanged).
`prometheia_engine_catalog_count` and `prometheia_engine_catalog_info(engine,
i, &bodies, &path, &err)` give each small-body catalog in the order it was
added, with its record count and the path it was added by. A later catalog
wins for a body both carry. `prometheia_engine_catalog_bodies(engine,
&bodies, &err)` gives the distinct bodies they answer between them. With one
catalog that is its record count. With several it takes one pass over their
indexes, kept until a catalog is added: 0.37 s for the 1,567,167-body
catalog with a 100-body sample stacked on it (2026-10-25). This replaces
parsing the `[N bodies]` text of a result's `source`, whose wording is not
a contract.

**Houses, sidereal time, obliquity, rotations** (added after 0.7.3; ABI
version unchanged, as no existing layout changed; announced to the Astrolog
side first, 2026-10-27). [HOUSES.md](HOUSES.md) has the definitions.

```c
prometheia_houses h;
if (prometheia_calc_houses_ut(eph, PROMETHEIA_HOUSES_PLACIDUS, jd_ut1,
                              8.55, 47.37, &opts, &h, &err) == PROMETHEIA_OK)
    printf("Asc %.4f  MC %.4f  cusp 2 %.4f\n", h.asc_deg, h.mc_deg, h.cusp_deg[1]);
```

- `prometheia_calc_houses` (TT) and `_ut` (UT1) are `Engine::houses` and
  `houses_ut`, bit for bit, and the same code as the JSON `houses` tool and
  protocol v4 kind 6. The system is one of the `PROMETHEIA_HOUSES_*` values,
  which are v4's A.22 numbers.
  - The site's longitude is east positive and its latitude geodetic,
    strictly inside ±90°.
  - Only the zodiac and the precession model are read from the options.
    The frame must stay `PROMETHEIA_FRAME_TRUE_OF_DATE`.
  - The step between TT and UT1 uses the engine's ΔT, including a function
    installed with `prometheia_engine_set_delta_t`, as `calc` and `calc_ut`
    do (`c_api_houses_match_engine` checks the hook is called).
  - A sidereal zodiac shifts every cusp and angle but the ARMC.
- **Refusals** are `PROMETHEIA_ERROR_ARGUMENT`, with a message naming the
  system and the reason. Another system is never put in its place:
  - Placidus and Koch inside a polar circle;
  - Regiomontanus, Campanus and Topocentric where their cusps fall out of
    zodiacal order, near the poles;
  - every system at a pole.
  Every answer's twelve cusps go once round the zodiac.
- **The angles:** `asc_deg` is the rising point at every latitude, and
  `mc_deg` is the upper meridian (hour angle 0), even where it culminates
  below the horizon. Every longitude is in [0, 360).
- `prometheia_houses_from_armc(system, armc_deg, obliquity_deg, lat_deg, …)`
  needs no engine and no instant: an oblique Ascendant, or a chart at a
  reference latitude. It takes the **true** obliquity, and returns every
  angle, the Vertex and equatorial Ascendant included. Given the ARMC and
  obliquity `prometheia_calc_houses` reports, it agrees with that tropical
  answer to 1e-6″. It is not bit for bit, because the ARMC passes through
  degrees.
- `prometheia_sidereal_time_ut(engine, jd_ut1, kind, opts, &deg, &err)` is
  Greenwich mean (GMST, IAU 2006) or apparent (GAST) sidereal time as an
  angle in [0, 360). GAST plus the east longitude is the houses' ARMC, to
  1e-10″. `prometheia_obliquity(engine, jd_tt, kind, opts, &deg, &err)`
  gives the mean or true obliquity. Both read only `opts->precession`.
  Against ERFA over 1600–2200, the agreement is GMST 3e-8″, GAST 0.00015″,
  mean obliquity 1e-11″ and true obliquity 0.00005″.
- `prometheia_ecliptic_to_equatorial` and `_equatorial_to_ecliptic` rotate
  about the equinox by a given obliquity. They are pure functions: no
  engine, no instant, no status.
- **Cusp speeds** are not served. If they come, they come as a separate
  call with its own struct, so `prometheia_houses` keeps its layout.

**The horizon and refraction** (added after 0.7.3; ABI version unchanged;
announced to the Astrolog side first, 2026-10-27). [FRAMES.md](FRAMES.md),
"The horizon", has the definitions and the measurements.
- `prometheia_horizontal_ut(engine, jd_ut1, lon, lat, height_m, ra, dec,
  &atmosphere, opts, &out, &err)` puts a direction on the true equator and
  equinox of date in the site's horizon. For a body that is its topocentric
  apparent equatorial position from the same site. It returns the azimuth,
  the true and apparent altitudes, and the local hour angle.
  - **Azimuth runs from north through east.** pyswisseph's `azalt` counts
    from the south, westward, so a call ported one for one comes out 180°
    off and still looks plausible.
  - The hour angle is positive westward.
  - `height_m` sets only the dip. No parallax is applied here.
- `prometheia_equatorial_from_horizontal_ut` goes back, removing the
  refraction from an apparent altitude first.
- `prometheia_refraction(alt, PROMETHEIA_REFRACTION_FROM_TRUE |
  _FROM_APPARENT, &atmosphere, height_m, &deg, &err)` is Bennett 1982,
  applied down to the dip or −1°.
- `prometheia_atmosphere_init` gives 1013.25 hPa and 15 °C. A `NULL`
  atmosphere means none on the horizon calls, and the standard one on
  `prometheia_refraction`.
- `c_api_horizon_matches_engine` holds every field to `Engine` with `==`,
  with and without an atmosphere, at three latitudes. It round-trips through
  refraction to 2e-10″.

**Rise, set and transit** (added after 0.7.3; ABI version unchanged;
announced to the Astrolog side first, 2026-10-27). [ENGINE.md](ENGINE.md),
"Rise, set and transit", has the method and the measurements.
- `prometheia_rise_set_ut(engine, &object, event, jd_ut1_start, lon, lat,
  height_m, &options, opts, &out, &err)`:
  - the object is a `prometheia_object` naming a body, a star, a
    hypothetical, an orbit point or a registry id;
  - the event is a rise, a set, or an upper or lower transit;
  - the answer is the first event after the start, or the last before it
    with `backward`.
- The options (`prometheia_rise_set_options_init`) are the limb (upper by
  default, centre or lower), refraction (on by default) with its
  atmosphere, the dip's eye height (0 by default: the mathematical horizon,
  whatever the site's height), and the window in days (2 by default).
- `height_m` gives the site's parallax and no dip. An orbit point has none:
  it rises as a direction seen from the Earth's centre (ENGINE.md, "Rise,
  set and transit").
- A window with no such event is `PROMETHEIA_OK`: `out.outcome` says
  ALWAYS_ABOVE, ALWAYS_BELOW or NOT_IN_WINDOW, and `out.jd_ut1` is NaN.
- `c_api_rise_set_matches_engine` holds every outcome and time to `Engine`
  with `==`: a body, a star and a hypothetical, all four events, both
  limbs, both directions.

**Lunar eclipses** (added after 0.7.3; ABI version unchanged; to be announced
to the Astrolog side before it lands). [ECLIPSES.md](ECLIPSES.md) has the
method and the measurements.
- `prometheia_lunar_eclipse_next(engine, jd_start_tt, kinds, backward, &out,
  &err)`: the first eclipse after (before, with `backward`) a TT Julian
  date, of the kinds in the bitmask `PROMETHEIA_LUNAR_ECLIPSE_TOTAL |
  _PARTIAL | _PENUMBRAL` (0: any kind, as for the solar calls; other bits:
  `PROMETHEIA_ERROR_ARGUMENT`). "After" and "before" are strict, so a
  caller stepping through a span passes the last result's `jd_max_tt`.
  None within about 160 years is `PROMETHEIA_ERROR_NOT_FOUND`.
- `prometheia_lunar_eclipse_at(engine, jd_max_tt, &site, &out, &err)`: the
  eclipse whose greatest is within two days, with the Moon's true and
  refracted altitude at the site (`prometheia_site`: lon, lat, height_m) at
  each contact and the maximum; a NULL site gives NaN altitudes.
- `prometheia_lunar_eclipse` and `prometheia_lunar_eclipse_local` hold
  fixed-size members only: ints, doubles, and arrays of 6 or 7 doubles
  indexed by `PROMETHEIA_LUNAR_CONTACT_*`. An absent contact is NaN. On an
  error `*out` is zeroed.

To build the library inside another project (a git submodule, plain make,
no CMake), see [EMBEDDING.md](EMBEDDING.md).

The fixed-star functions (`prometheia_star_find`, `_lookup`, `_info`,
`prometheia_calc_star`, `prometheia_constellation_at`) are described in
[STARS.md](STARS.md).

`prometheia_calc_orbit_point` and `_ut` wrap `Engine::calc_orbit_point`
([ENGINE.md](ENGINE.md), "Nodes and apsides"). The options' corrections
apply to an orbit point as to a body, and all three off give the geometric
point ([ORBIT-POINTS.md](ORBIT-POINTS.md)). An orbit point or elements
selector out of range is `PROMETHEIA_ERROR_ARGUMENT`.
`PROMETHEIA_ELEMENTS_INTERPOLATED` (2, library 0.6; the ABI version is
unchanged, as no layout changed) is the Moon's natural apogee and perigee,
interpolated between its actual passages. Asked for any other body or point,
it is `PROMETHEIA_ERROR_ARGUMENT`.

**Hypothetical bodies (ABI version 5).** Two kinds, as in the ephemeris
protocol ([HYPOTHETICALS.md](HYPOTHETICALS.md)):

```c
/* A body from orbital elements the caller supplies (protocol kind 4). */
prometheia_elements el = {0};
el.epoch_jd_tt = 2415020.0;
el.equinox = PROMETHEIA_EQUINOX_J1900;
el.origin = PROMETHEIA_ELEMENTS_ORIGIN_SUN;
el.n_terms = 1;
el.mean_anomaly[0] = /* ... */;  /* and a, e, w, node, i */
prometheia_calc_elements(eph, &el, jd_tt, &opts, &r, &err);

/* A body by name (protocol kind 3), from the shipped set or a file. */
prometheia_engine_add_hypotheticals(eph, "my-elements.jsonl", &err);
prometheia_calc_hypothetical(eph, "cupido", jd_tt, &opts, &r, &err);
printf("%s\n", r.source);    /* the element set's name */
```

- A client whose user supplies their own element file should fill a
  `prometheia_elements` from it and call `prometheia_calc_elements`, so that
  what is computed is exactly the user's definition. `prometheia_calc_hypothetical`
  is for Prometheia's own named set, or a set loaded with
  `prometheia_engine_add_hypotheticals`.
- `prometheia_hypothetical_count` and `_token` enumerate the tokens, and
  `prometheia_hypothetical_get` returns a token's name, set, citation and
  elements. Tokens match ASCII case-insensitively. The strings stay valid
  until the engine is closed or another element file is added.
- An undefined token is `PROMETHEIA_ERROR_NOT_FOUND`. Elements out of range,
  or not a bound orbit at the instant, give `PROMETHEIA_ERROR_ARGUMENT`. A
  malformed element file gives `PROMETHEIA_ERROR_FORMAT`, with the file and
  line in the message.

**Phenomena and magnitudes (after 0.7.3; ABI version unchanged).**
`prometheia_calc_phenomena(_ut)(engine, object, jd, opts, out, err)` takes
any `prometheia_object`. It fills a `prometheia_phenomena` with the phase
angle, illuminated fraction, elongation, apparent diameter and V magnitude,
and names the published model that gave the magnitude. Quantities that do
not apply are NaN. [PHENOMENA.md](PHENOMENA.md) has the formulas and the
measured agreement with `swetest`.

**The object registry (after 0.7.3; ABI version unchanged).** One id scheme
for every object, and lookup, description and listing across all of them
([REGISTRY.md](REGISTRY.md)):

```c
prometheia_object_match m[20];
int n = 0;
prometheia_object_lookup(eph, "2003 VB12", 0, m, 20, &n, &err); /* m[0].id: "sb:90377" */
prometheia_object_entry info;
prometheia_object_info(eph, m[0].id, &info, &err);  /* name, number, class, mean motion */
const prometheia_object obj = {info.object_kind, info.object_id, info.object_token};
prometheia_calc_object(eph, &obj, jd_tt, &opts, &r, &err);
```

- `prometheia_object_lookup` writes up to `max` matches, best first. Several
  of the best quality (`PROMETHEIA_MATCH_EXACT` or `_OLD_DESIGNATION`) mean
  the query is ambiguous, and nothing is chosen. `PROMETHEIA_LOOKUP_PREFIX`
  adds prefix matches.
- `prometheia_object_info` fills a `prometheia_object_entry`. Absent numbers
  are NaN, absent strings are empty, and strings are truncated to fit.
- `prometheia_object_list` pages through the objects that pass a
  `prometheia_object_filter` (start from `prometheia_object_filter_init`).
  `*cursor` starts at 0, and is 0 again after the last page.
- `prometheia_calc_object(_ut)` computes any object by the call that
  computes it. `prometheia_object` gains two kinds:
  `PROMETHEIA_OBJECT_ORBIT_POINT` (`id` the body, `token`
  `"<point>:<method>"`) and `PROMETHEIA_OBJECT_ID` (`token` a registry id).
  `prometheia_rise_set_ut` takes both as well.
- The registry's indexes are built on first use and kept by the engine
  handle until a catalog is added. REGISTRY.md has the measured cost.

## Solar eclipses

Method, validation and limits: [SOLAR_ECLIPSES.md](SOLAR_ECLIPSES.md).
Additive: no existing struct or function changed (ABI version unchanged).

- `prometheia_solar_eclipse_next(engine, jd_start, kinds, backward, out, err)`:
  the next eclipse after `jd_start` (a **TT** Julian date; the last before it
  with `backward`) whose kind is among `kinds` (`PROMETHEIA_SOLAR_PARTIAL`,
  `_ANNULAR`, `_TOTAL`, `_HYBRID`, or'ed; 0 or `_ALL_KINDS` for any).
  `prometheia_solar_eclipse` carries the type bits (one kind plus
  `_CENTRAL` or `_NONCENTRAL` for umbral kinds), TT and UT1 of greatest
  eclipse, gamma, magnitude (NASA's convention) and `magnitude_covered`,
  obscuration, the greatest-eclipse point and the Sun's altitude and azimuth
  there, path width and central duration, the six first/last contacts
  (`p1 u1 c1 c2 u4 p4`, TT, NaN where absent), saros, lunation and the
  Besselian elements (`prometheia_besselian`). `NOT_FOUND` if none within
  about 240 years.
- `prometheia_solar_eclipse_where(engine, jd_tt, out, err)`: the shadow axis
  on the ground at an instant: `axis_hits`, the centre line, the umbra's
  north and south limits, path width, duration, Sun altitude, magnitude and
  obscuration there. NaN throughout when the axis misses the Earth.
- `prometheia_solar_eclipse_local_next(engine, jd_start, site, backward, out,
  err)` with `prometheia_site {lon_deg, lat_deg, height_m}`: the next
  eclipse seen from the site with the Sun up in some phase.
  `prometheia_solar_local` has C1-C4 and the maximum in TT and UT1, their
  altitudes and azimuths, `visible_max`, magnitude, obscuration, the Moon/Sun
  diameter ratio, the C2-C3 duration, saros. A contact below the horizon or
  not happening has NaN times.
- Each has an `_ex` form taking a `const prometheia_solar_eclipse_options*`
  (`k_umbra`, `k_penumbra`, `sun_radius_km`; NULL or
  `prometheia_solar_eclipse_options_init` for NASA's 0.272281, 0.272488,
  696000 km; swetest's is 0.2725076 for both). Delta T is the engine's,
  including one installed with `prometheia_engine_set_delta_t`.

## Heliacal visibility

Method, validation and limits: [HELIACAL.md](HELIACAL.md). Additive: no
existing struct or function changed (ABI version unchanged). The atmosphere
struct is `prometheia_air`, because `prometheia_atmosphere` (pressure and
temperature, for refraction) already exists.

- `prometheia_heliacal_next(engine, object, jd_start_tt, site, air,
  observer, model, event, out, err)`: the first heliacal event strictly
  after `jd_start_tt` (TT). `event` is `PROMETHEIA_HELIACAL_RISING`,
  `_SETTING`, `_EVENING_FIRST` or `_MORNING_LAST` (1..4, swetest's numbers; a
  star has the first two). `site` is a `prometheia_site`; `air`
  (`prometheia_air`: pressure, temperature, relative humidity, and NaN-able
  `extinction_coefficient` or `visual_range_km`, not both), `observer`
  (`prometheia_observer`: age, Snellen ratio, binocular) and `model`
  (`prometheia_visibility_model`: `kind` `PROMETHEIA_VISIBILITY_SCHAEFER` or
  `_ARCUS_VISIONIS`, the arc and object altitude, the detection margin,
  moonlight, the dark-sky brightness, `search_days`) may be NULL for the
  defaults of their `_init` functions. The result `prometheia_heliacal` has
  the event (the start of the visibility window), the optimum and the end in
  TT and UT1, and the circumstances at the optimum (V, altitude, azimuth, the
  Sun's depression, sky brightness, extinction, limiting magnitude, margin),
  with the inputs used echoed. `NOT_FOUND` when none falls within
  `search_days`; `ARGUMENT` for a bad input, including arcus visionis without
  an arc.
- `prometheia_visibility_at(engine, object, jd_tt, site, air, observer,
  model, out, err)`: one moment, `prometheia_visibility`: the limiting
  magnitude outside the atmosphere and at the eye, the extinction by
  component, the sky brightness, the margin and `visible`.
- `prometheia_limiting_magnitude(sky_brightness_nl, observer)`: Schaefer's
  naked-eye limiting V for a point source on a uniform sky; NaN for bad input.

## Segments

Additive: no existing struct or function changes, so the ABI version stays
6. The signatures were reviewed by the Astrolog side before they landed.
[SEGMENTS.md](SEGMENTS.md) has the fitter, its residuals and its costs.

A segment set is a Chebyshev fit of one object's position over a TT span,
placed by the options exactly as `prometheia_calc_object` places it. Once
fitted, it answers any instant in the span without the engine. This is the
bulk interface for scanning: a caller fits each object once per window,
then evaluates the polynomials itself, inline and without the GIL.

```c
typedef struct prometheia_segments prometheia_segments;
#define PROMETHEIA_SEGMENTS_MAX_DEGREE 16   /* today's; index with layout->stride */

typedef struct prometheia_segments_layout {
    int count, stride;
    double jd_tt_start, jd_tt_end;
    const double *bounds_jd_tt;   /* count + 1 */
    const double *mid_jd_tt, *half_span_days; /* count */
    const int *degree;            /* count */
    const double *coef, *dcoef;   /* 3 * stride * count each */
    double tolerance_arcsec, err_arcsec, err_rel_dist, err_rate_arcsec_per_day;
    int coords;
} prometheia_segments_layout;

typedef struct prometheia_segments_point {
    double lon_deg, lat_deg, dist_au, lon_speed, lat_speed, dist_speed;
    double xyz_au[3], vel_au_day[3];
} prometheia_segments_point;

prometheia_status prometheia_segments_fit(prometheia_engine *engine,
    const prometheia_object *object, double jd_tt_start, double jd_tt_end,
    const prometheia_options *opts, double tolerance_arcsec,
    prometheia_segments **out, prometheia_error *err);
prometheia_status prometheia_segments_eval(const prometheia_segments *segments,
    double jd_tt, prometheia_segments_point *out, prometheia_error *err);
const prometheia_segments_layout *prometheia_segments_get_layout(
    const prometheia_segments *segments);
void prometheia_segments_close(prometheia_segments *segments);
```

- **What is fitted.** The rectangular vector `xyz_au` that
  `prometheia_calc_object` returns for the same object and options. The
  observer, frame, coordinates, corrections and zodiac are all inside it. A
  sidereal zodiac is a rotation already applied to that vector, so no
  separate ayanamsa series is needed. Every object kind is accepted:
  bodies, catalog bodies, stars, hypotheticals, orbit points (the mean and
  osculating nodes and apsides) and registry ids. `speed` and `sigma` in
  the options are not read.
- **Tolerance and residuals.** `tolerance_arcsec` is the largest direction
  error the caller accepts; the astromcp scan kernel asks for 0.001″. The
  fit aims at 0.8 × the tolerance, because a fit can land slightly worse
  between its check points than on them (Venus by 1% at 0.01″).
  - The layout reports the worst residuals measured against
    `prometheia_calc_object` on each segment's check set. The check set is
    both ends of the segment plus 4 × (degree + 1) + 1 interior points
    offset from the nodes the fit interpolated.
  - `err_arcsec` is the angular separation of the fitted and computed
    directions, not a longitude difference. It is at most 0.8 × the
    tolerance on those points: a measured bound there, not a guarantee
    everywhere. 10,000 random instants per case measured the error
    between the check points (SEGMENTS.md).
  - `err_rel_dist` is the relative error in |r|. `err_rate_arcsec_per_day`
    is the derivative's disagreement with the engine's rates, seen from the
    observer: the velocity is the derivative of the fitted position, not a
    separate fit.
- **Steps in the source.** Where the engine's answer jumps, a segment
  bound is placed, so no polynomial is fitted across a step:
  - **A second planetary file taking over** (DE440 then DE441,
    `prometheia_engine_add_ephemeris`). The observer's reads hand over at
    each edge of a file's span, and the body's reads one light time later,
    so both instants become bounds. The edges are TDB and converted to TT.
  - **No other step is known.** A catalog body is integrated from the one
    element record the newest catalog holds for it (FORMAT.md), and the
    perturber kernel ends in a coverage refusal, not a step. Stars,
    hypotheticals and mean elements are analytic. The Moon's interpolated
    apsides are a cubic Hermite between passages, continuous in value and
    slope.
- **Refusals.** `*out` is `NULL` on any failure.
  - `PROMETHEIA_ERROR_ARGUMENT`:
    - a span that is not finite or does not increase;
    - a tolerance that is not positive and finite;
    - an object within 1e-9 AU of the observer, which has no direction (the
      heliocentric Sun, say);
    - a tolerance out of reach. That is a segment of 0.1 day at degree 16
      still missing 0.8 × the tolerance, or a span needing more than 4096
      segments. A handle is therefore at most 4096 × 844 bytes, 3.46 MB.
  - **The engine's own errors** come back unchanged. Every instant the fit
    samples must be answered, so one refusal anywhere in the span (coverage,
    an unknown body) fails the whole fit with the engine's status. A fit
    never stops short.
- **Handles.** A handle is immutable once `_fit` returns. Any number of
  threads may call `_eval` and `_get_layout` on one handle at once. It
  holds no reference to the engine and may outlive it. `_close(NULL)` is a
  no-op.
- **The layout.** `layout->stride` is authoritative; index with it.
  `PROMETHEIA_SEGMENTS_MAX_DEGREE` (16, so a stride of 17) describes today's
  value only.
  - Segment `i` covers `[bounds_jd_tt[i], bounds_jd_tt[i + 1])`. A bound
    belongs to the segment after it, and the last segment includes the end.
  - **Position is not continuous across a bound.** Each side is within the
    tolerance of the engine, so the two may differ by up to twice it, plus
    the engine's own step at a file handover.
- **Evaluation.** `_eval` takes an instant in `[jd_tt_start, jd_tt_end]`;
  outside it, the status is `PROMETHEIA_ERROR_COVERAGE` and `*out` is
  zeroed. It does not allocate or lock. It finds the segment `i` that owns
  the instant, then computes, for each component `c` (0 x, 1 y, 2 z):
  - `u = (jd_tt - mid_jd_tt[i]) / half_span_days[i]`;
  - the position, the Chebyshev sum of `coef[(3*i + c)*stride + k]` at
    `u`, by Clenshaw's recurrence, in AU;
  - the velocity, the same sum over `dcoef`, divided by
    `half_span_days[i]`, in AU per day.

  Coefficients past `degree[i]` are zero, so a loop over all `stride`
  terms gives the same sums. From the vector `(x, y, z)` and velocity
  `(vx, vy, vz)`, with `R = 180/π`:
  - `rho2 = x*x + y*y`, `r = sqrt(rho2 + z*z)`;
  - `lon_deg = atan2(y, x) * R`, plus 360 when negative;
  - `lat_deg = atan2(z, sqrt(rho2)) * R`, and `dist_au = r`;
  - `lon_speed = (x*vy - y*vx) / rho2 * R`;
  - `lat_speed = (vz*rho2 - z*(x*vx + y*vy)) / (r*r*sqrt(rho2)) * R`;
  - `dist_speed = (x*vx + y*vy + z*vz) / r`.

  With equatorial coordinates, `lon_deg` is the right ascension and
  `lat_deg` the declination. The angles are in degrees and the rates per
  day, with `dist_speed` in AU per day. A caller's own loop that does the
  same arithmetic in the same order gets the same doubles, unless its
  compiler contracts multiply-adds differently
  (`c_segments_layout_is_eval` holds such a loop to `_eval` with `==`).
- **Against `calc`.** The direction differs from `prometheia_calc_object`
  by up to the tolerance; segments are never bit-identical to direct calls.

## Roots on segments

Additive, so the ABI version stays 6. astromcp asked for it, and astromcp
and the Astrolog side reviewed the signatures before they landed.
[SEGMENTS.md](SEGMENTS.md), "Roots", has the method and the measurements.

```c
#define PROMETHEIA_SEGMENTS_LON       0  /* longitude, or right ascension; degrees */
#define PROMETHEIA_SEGMENTS_LAT       1  /* latitude, or declination; degrees */
#define PROMETHEIA_SEGMENTS_LON_SPEED 2  /* degrees per day */
#define PROMETHEIA_SEGMENTS_LAT_SPEED 3

typedef struct prometheia_segments_function {
    const prometheia_segments *const *a; int na; /* handles back to back */
    const prometheia_segments *const *b; int nb; /* nb 0: no second term */
    double ca, cb;
    int component;
    double target, target_rate, target_jd_tt;
    double modulus;
} prometheia_segments_function;

typedef struct prometheia_segments_root {
    double jd_tt, slope, err_days;
    int tangent;
} prometheia_segments_root;

prometheia_status prometheia_segments_roots(const prometheia_segments_function *f,
    double jd_tt_start, double jd_tt_end, double tol_days,
    prometheia_segments_root *out, int capacity, int *count,
    double *jd_tt_resume, int *unresolved, prometheia_error *err);
```

- **The function.** It is
  `f(t) = ca·Xa(t) + cb·Xb(t) − (target + target_rate·(t − target_jd_tt))`,
  where X is the chosen component of each term. With equatorial fits, the
  longitude is the right ascension and the latitude the declination.
  - **Typical uses:**
    - an aspect or phase: `ca, cb` 1 and −1;
    - a midpoint: 0.5 and 0.5;
    - a crossing of a fixed point: 1 and 0, or `nb` 0;
    - a station: `LON_SPEED` with target 0;
    - out-of-bounds: `LAT` with the obliquity as a moving target.
  - **The terms.** `nb` is 0 for a function of one object.
  - **Handles and coordinates.** The two terms may come from different
    fits, with bounds that never coincide. Every handle must have been
    fitted in the same centre, frame, coords and zodiac. The corrections
    (light time, deflection, aberration) may differ.
- **Handles back to back.** A term may be several handles of one object,
  each ending exactly where the next starts. A gap or an overlap is
  refused. astromcp, for example, fits
  each object in month chunks. The term is then one continuous function
  across the joins, as across its own segment bounds.
- **The modulus.**
  - **Longitude only.** A longitude is taken modulo M in (−M/2, M/2]. M is
    `modulus`, and 0 means 360. Roots are where that passes 0, never where
    it wraps. The other components take no modulus (it must be 0).
  - **Whole numbers.** `360·ca/M` and `360·cb/M` must be whole numbers.
    Then a full turn of either longitude moves f by whole moduli, and its
    wrap at 0° is never seen.
  - **Midpoints.** A midpoint is asked at M = 180, its axis, or at 180/H
    on a harmonic dial. Both midpoints come back, and the caller tells the
    near one from the far. At M = 360 a midpoint is refused. Taking it
    would mean picking a turn for each longitude, and the answer would
    then depend on where the window starts.
- **No double roots at bounds.** Each term is first made continuous: half
  the step at each bound is spread linearly over the segments on either
  side.
  - **Why.** A fit's two sides may differ by up to twice its tolerance at a
    bound. Without this, a root near a bound could be found twice or not
    at all. A root placed exactly on each of the 15 bounds of a 1″ Moon
    fit, in three components, is found all 45 times with the correction
    and none without it.
  - **What it costs.** Near a bound, f differs from `_eval` by at most half
    the step there, which is within the fits' tolerance.
- **The method.** The window is searched in pieces; a piece is where one
  segment of each term overlaps.
  - On each piece, f is interpolated at Chebyshev points, halving until
    the series' tail is negligible.
  - Bounds on the interpolant's first and second derivatives cut f into
    monotone runs, with at most one extremum between two runs.
  - Each run's roots are counted from f at its two ends, so a run never
    yields more roots than f really crosses. Each root is then solved for
    on f itself, bracketed, to `tol_days` or 1e-6 day, whichever is finer.
    `slope`, `err_days` and `tangent` are read at the root, so a coarse
    `tol_days` never changes them.
  - No grid is stepped. `tol_days` is only the precision of each root; it
    does not set how finely f is examined.
  - **Every call ends.** A cell is halved at most 40 times, never below
    1e-6 day, and a piece holds at most 16,384 cells.
  - **Every root, or a count of where one may be lost.** A cell that ends
    at one of those limits (a pole crossing, say) has its roots counted from
    its two ends. A pair inside it that cancels may then be lost.
    `*unresolved` (NULL allowed) counts such cells meeting the window, so 0
    means every root was found. None of the measured searches hit a
    limit.
  - **Roots strictly increase,** so a resume never writes one twice.
  - Two roots a few minutes apart, either side of a station, are both
    found.
- **Tangent.**
  - **The band** is the fits' own error in f: |ca|·tol_a + |cb|·tol_b.
    For an angle, tol is the term's largest `tolerance_arcsec`. For a
    speed it is the largest declared `err_rate_arcsec_per_day`. Within the
    band of 0, the fits cannot tell one root from two or none.
  - **Flagged roots.** `tangent` is 1 in two cases:
    - a crossing whose turn-back, slope²/(2|f″|), is within the band. Both
      crossings of a graze are flagged;
    - an extremum within the band of 0 that does not reach it. It is
      reported as a root of its own, at the extremum, with slope about 0.
  - **No root.** An extremum farther from 0 than the band gives nothing.
  - **What the caller decides.** Grazes are reported, not merged.
- **The window and resume.**
  - **The window** is `[jd_tt_start, jd_tt_end)`, and must lie inside each
    term's span (`PROMETHEIA_ERROR_COVERAGE` otherwise).
  - **Same roots from any window.** Every piece is searched whole, whatever
    the window, so a root is the same double from any window that holds
    it. Back-to-back windows give each root once.
  - **When the buffer fills.** At most `capacity` roots are written.
    `*jd_tt_resume` is then the instant of the first root not written: the
    start of a call that carries on. It is NaN when every root was
    written. A loop of `capacity` 1 gives the same doubles as one call.
  - **Counting only.** `capacity` 0 writes nothing (`out` may be NULL) and
    counts every root.
- **Accuracy.** The time error is about band/|slope|, plus the precision
  solved to.
  `err_days` gives that estimate per root, and sqrt(2·band/|f″|) for a
  tangent root. Measured against bisection on `prometheia_calc_object`,
  the worst case was under half of `err_days` (SEGMENTS.md).
  - **Near a pole.** Near a pole of the coordinates (latitude or
    declination close to ±90°), the longitude and its rate are
    ill-conditioned, and so are their roots.
  - **The units of `slope`** are f's per day: degrees per day, or degrees
    per day squared for a speed.
- **Threads.** Nothing is read but the handles: no engine, no lock, no
  thread, no allocation. Any number of threads may search the same handles
  at once.
- **Refusals.** `PROMETHEIA_ERROR_ARGUMENT` for:
  - a NULL `f`, handle, `count` or `jd_tt_resume`, or `out` NULL with
    `capacity` above 0, or a negative capacity;
  - a window that is not finite or does not increase;
  - a `tol_days` that is not positive and finite;
  - a component out of range;
  - handles fitted in different coordinates, or not back to back;
  - `ca` and `cb` both 0, or a field that is not finite;
  - a modulus the rules above refuse.

### Sums of more than two terms

```c
#define PROMETHEIA_SEGMENTS_MAX_TERMS 8
typedef struct prometheia_segments_term {
    const prometheia_segments *const *handles; int count; double c;
} prometheia_segments_term;
typedef struct prometheia_segments_sum {
    const prometheia_segments_term *terms; int nterms;
    int component;
    double target, target_rate, target_jd_tt, modulus;
} prometheia_segments_sum;
prometheia_status prometheia_segments_roots_sum(const prometheia_segments_sum *f,
    double jd_tt_start, double jd_tt_end, double tol_days,
    prometheia_segments_root *out, int capacity, int *count,
    double *jd_tt_resume, int *unresolved, prometheia_error *err);
```

f = Σ cᵢ·Xᵢ − (target + target_rate·(t − target_jd_tt)), for 1 to 8 terms.
astromcp asked for it for a midpoint against a body (3 terms) and a midpoint
against a midpoint (4). `prometheia_segments_roots` is this call with one or
two terms. Everything above holds per term:
- each term's handles back to back, covering the window;
- every handle fitted in the same coordinates;
- 360·cᵢ/M whole for a longitude;
- the band Σ|cᵢ|·tolᵢ, so it widens, and the tangent zone with it, with
  every term added.

What is new:
- **Repeated terms combine.** Terms naming the same handles (same count,
  same pointers) are one term, their coefficients summed. A term whose
  coefficient is then 0 is dropped, so midpoint(a, b) − a is ½b − ½a. If
  nothing is left, f is identically flat, and the call is refused before
  any search.
- **The branches multiply.** Modulo 180 a midpoint is both of its points.
  A midpoint against a body therefore gives the roots of both, and a
  midpoint against a midpoint gives those of all four pairings. A caller
  wanting one branch evaluates it at each root and keeps what it wants.
- **The term limit.** `PROMETHEIA_SEGMENTS_MAX_TERMS` is a limit the call
  checks, not a layout, so it may grow without a new ABI version.
- **Refusals.** `PROMETHEIA_ERROR_ARGUMENT` also covers `terms` NULL,
  `nterms` outside 1 to the limit, a term with `count` below 1, and every
  coefficient 0 once terms are combined.

## Coverage

```c
prometheia_status prometheia_object_coverage(prometheia_engine *engine,
    const prometheia_object *object, const prometheia_options *opts,
    double *first_jd_tt, double *last_jd_tt, prometheia_error *err);
```

The TT interval in which `prometheia_calc_object` answers the object with
these options (NULL: the defaults), each end to adjacent doubles: at both ends
it answers, and one double outside either it refuses. A fit clipped to the
interval never meets a refusal. Additive; the ABI version stays 6.

- **Why not the files' span.** It is narrower than the planetary files'
  span (`prometheia_engine_ephemeris_span`, in TDB):
  - the body is read one light time before the instant;
  - an orbit point reads its focus;
  - a catalog body also needs the perturber kernel.
- **Options.** A fit takes rates, so ask with the options the fit will use.
- **How.** The engine is asked at 65 instants across the files' span, and
  each edge is bisected: about a hundred position calls.
  - For an ephemeris body, an orbit point or a star that takes 1–6 ms.
  - A catalog body integrates across the whole span, 1.8 s for Ceres over
    DE440 (measured 2026-10-28).
  - Cache the answer; it changes only when files are added.
- **Refusals.**
  - `PROMETHEIA_ERROR_COVERAGE` when the object is answered nowhere, or not
    across one interval because the loaded files leave a gap. A gap
    narrower than the probes' spacing is not seen.
  - The engine's own error for an unknown object.
  - `PROMETHEIA_ERROR_ARGUMENT` for a NULL argument.
  - Both outputs are NaN on any failure.

## Datasets and threads

One dataset holds the files, loaded once, and any number of engines,
one per thread, compute from it ([ENGINE.md](ENGINE.md), "Threads").
Additive: no existing struct or function changed (ABI version unchanged).
The signatures were sent to the Astrolog side before they landed.

```c
typedef struct prometheia_dataset prometheia_dataset;

prometheia_status prometheia_dataset_open(const char *ephemeris_path,
                                          prometheia_dataset **out, prometheia_error *err);
prometheia_status prometheia_dataset_add_ephemeris(prometheia_dataset *dataset,
                                                   const char *ephemeris_path, prometheia_error *err);
prometheia_status prometheia_dataset_add_catalog(prometheia_dataset *dataset,
                                                 const char *catalog_path, prometheia_error *err);
prometheia_status prometheia_dataset_add_perturbers(prometheia_dataset *dataset,
                                                    const char *spk_path, prometheia_error *err);
prometheia_status prometheia_dataset_add_hypotheticals(prometheia_dataset *dataset,
                                                       const char *element_file_path,
                                                       prometheia_error *err);
prometheia_status prometheia_dataset_freeze(prometheia_dataset *dataset, prometheia_error *err);
void prometheia_dataset_close(prometheia_dataset *dataset);

prometheia_status prometheia_context_open(const prometheia_dataset *dataset,
                                          prometheia_engine **out, prometheia_error *err);
```

- `prometheia_dataset_open` takes the same files, detected the same way,
  as `prometheia_engine_open`, and the `_add_*` calls behave as the
  engine's do. They are for one thread, before the freeze.
- **Failures.** A refused file leaves the dataset exactly as it was, and
  usable. A failed `prometheia_dataset_open` or `prometheia_context_open`
  sets `*out` to `NULL`, as `prometheia_engine_open` does. `NULL` for the
  dataset is `PROMETHEIA_ERROR_ARGUMENT` everywhere but
  `prometheia_dataset_close`.
- `prometheia_dataset_freeze` ends the adding: an `_add_*` call after it
  is `PROMETHEIA_ERROR_ARGUMENT`. Freezing twice is fine.
- `prometheia_context_open` gives an ordinary `prometheia_engine`: every
  call that takes an engine takes it, and closes with
  `prometheia_engine_close`. It refuses a dataset not yet frozen
  (`PROMETHEIA_ERROR_ARGUMENT`). Several threads may open contexts from
  one dataset at once.
- The engine `_add_*` calls on a context are `PROMETHEIA_ERROR_ARGUMENT`:
  its files are the dataset's.
- **Reference-counted.** `prometheia_dataset_close` releases the handle;
  the files stay loaded until the last context opened from it is closed as
  well, so the handle may be closed as soon as the contexts are open.
  `NULL` is a no-op. The handle itself is gone once closed: opening a
  context from it afterwards is a use after free, not an error the library
  can report.
- **Threads.** A context, like an engine, serves one thread at a time;
  each thread opens its own. What is computed from the files once for
  every context is the dataset's, built by whichever context first needs
  it, under the library's own locks:
  - the catalogs' name indexes, and the distinct-body count
    `prometheia_engine_catalog_bodies` returns;
  - the planetary tables a catalog body is integrated through, in blocks
    of a year;
  - the Moon's apogee and perigee passages behind the interpolated
    apsides, in blocks of 50 days.

  Everything else a context computes is its own: the small bodies'
  trajectories, its record cache, its frames and its Delta T. Every
  answer is bit-identical to one engine's, whatever the order of the
  questions and the number of threads. `tests/test_c_dataset.cpp` checks
  that with six contexts asking the same 240 questions at once, starting
  together on the shared caches, against an engine opened the ordinary
  way. With every block sampled through one context's read scratch
  instead of the caller's, all six threads fail it.
- **Strings are per context.** The strings valid "until the next call"
  (`prometheia_engine_ephemeris_span`'s source,
  `prometheia_engine_catalog_info`'s path) are kept by each context, so a
  sibling's call never overwrites them. `result.source`,
  `prometheia_engine_source()` and the hypothetical tokens are the
  dataset's and never change: they stay valid until the context is
  closed.
- `prometheia_engine_set_delta_t` is per context.

## Conventions

- **Handles.** `prometheia_engine_open` creates an opaque engine and
  `prometheia_engine_close` frees it (`NULL` is a no-op). An engine is not
  safe for concurrent use; give each thread its own, opened from one shared
  dataset ("Datasets and threads"). The library keeps no process-wide
  state.
- **Status.** Fallible calls return `prometheia_status` (`PROMETHEIA_OK`
  = 0). The last parameter is an optional `prometheia_error*` (`NULL` is
  allowed) that receives the same code and a NUL-terminated message,
  truncated to 255 bytes. Success clears the message.
  | status | meaning |
  |--------|---------|
  | `PROMETHEIA_ERROR_IO` | file cannot be opened or read |
  | `PROMETHEIA_ERROR_FORMAT` | not a recognised or valid ephemeris/catalog |
  | `PROMETHEIA_ERROR_CORRUPTION` | checksum mismatch or truncation |
  | `PROMETHEIA_ERROR_ARGUMENT` | invalid input: `NULL`, out-of-range option, non-finite time, the observer as the body |
  | `PROMETHEIA_ERROR_NOT_FOUND` | no ephemeris or catalog answers the body or name |
  | `PROMETHEIA_ERROR_INTERNAL` | allocation failure or other internal fault (no C++ exception ever crosses the interface) |
  | `PROMETHEIA_ERROR_COVERAGE` | an instant outside the loaded data's time span: the ephemeris, the perturber kernel, or the natural apsides' three passages on each side. Appended after 0.7.0 (2026-09-29, announced to the Astrolog side first); those inputs returned `PROMETHEIA_ERROR_ARGUMENT` before, and the message text is unchanged |
- **Outputs on failure** are zeroed (`*result`, `*utc`, `*body`, `*jd_tt`),
  and `*out` from open is set to `NULL`.
- **A second ephemeris** (library 0.8, ABI unchanged: a function was
  added, no layout moved; announced to the Astrolog side before it
  landed). `prometheia_engine_add_ephemeris` puts a file behind the one
  opened; each read goes to the first whose span holds the instant, so
  DE440 then DE441 answers 1550–2650 from DE440 and −13000..17000 from
  DE441 ([DE.md](DE.md), "DE441"). `result.source` names the file that
  answered.
- **Precession** defaults to `PROMETHEIA_PRECESSION_VONDRAK2011` from
  library 0.8 (IAU 2006 before; [FRAMES.md](FRAMES.md)). Answers that took
  the default move by ≤ 0.01″ within 1550–2650; `IAU2006` stays selectable.
- **Options.** Start from `prometheia_options_init()`, not from a zeroed
  struct. Zero is not the default for every field: frame 0 is ICRF and
  sidereal mode 0 is Fagan/Bradley. `sidereal_plane` (ABI version 6)
  takes the protocol's A.8 values through `PROMETHEIA_SIDEREAL_PLANE_*`:
  the ecliptic of date (default), the ecliptic of the zodiac's anchor
  epoch, or the invariable plane. The two fixed planes apply only with a
  sidereal zodiac, need ecliptic coordinates (`PROMETHEIA_ERROR_ARGUMENT`
  otherwise), ignore `frame`, and report A0 as `ayanamsa_deg`
  ([FRAMES.md](FRAMES.md), "Sidereal planes").
- **Zodiacs defined at the instant** (library 0.4, ABI still 6: enumerated
  values are only appended).
  - The modes are `PROMETHEIA_SIDEREAL_TRUE_CITRA`, `_TRUE_REVATI`,
    `_TRUE_PUSHYA`, `_TRUE_MULA`, `_GALCENT_0SAG`, `_GALCENT_COCHRANE`,
    `_GALCENT_RGILBRAND`, `_GALCENT_MULA_WILHELM`, `_GALEQU_IAU1958`,
    `_GALEQU_TRUE` and `_GALEQU_MULA`, numbered as the protocol's A.11
    tokens.
  - They have no anchor epoch, so `PROMETHEIA_SIDEREAL_PLANE_ANCHOR` is
    refused for them (`PROMETHEIA_ERROR_ARGUMENT`). On the invariable
    plane their zero point is the instant's
    ([FRAMES.md](FRAMES.md), "Zodiacs defined at the instant").
  - A library older than 0.4 refuses these values with
    `PROMETHEIA_ERROR_ARGUMENT`.
- **`ayanamsa_deg` depends on the frame**, for every zodiac, as the
  protocol's does (§3.5a).
  - The true ayanamsha in the true frame, the mean one in the mean frame,
    and the constant mean value at J2000.0 in the J2000 and ICRF frames.
  - On a fixed plane it is the anchor's A0, or the instant's mean ayanamsha
    for a zodiac defined at the instant.
  - The same zodiac at the same instant therefore reports different values
    in different frames. Compare the column only within one frame.
- Selector fields are plain `int`s,
  range-checked on every call. Boolean switches treat any nonzero value as
  true. `NULL` options mean the defaults.
- **Units.** The C struct takes the topocentric site in **degrees**
  (geodetic, east longitude positive, height above WGS84 in metres). The
  C++ `GeoSite` takes radians. Outputs match the C++ `Position`: degrees,
  AU, per day.
- **Optional results.** `sigma_arcsec` and `ayanamsa_deg` are valid only
  when `flags` has `PROMETHEIA_HAS_SIGMA` or `PROMETHEIA_HAS_AYANAMSA`.
  This preserves the C++ `std::optional` semantics: an absent sigma means
  the source publishes none, which is different from 0.
- **Strings.** `result.source` and `prometheia_engine_source()` point into
  the engine. They stay valid until the engine is closed. Adding a catalog
  or an ephemeris also invalidates them, because the descriptions are
  rebuilt.
- **Delta T.** `prometheia_engine_set_delta_t(engine, fn, user)` installs
  `double fn(void *user, double jd_tt)` (seconds of TT−UT1) for
  `prometheia_calc_ut` and for topocentric Earth rotation. `fn = NULL`
  restores the observed USNO model. The engine does not own `user`.
- **Bodies.** NAIF IDs (`PROMETHEIA_SUN`, `PROMETHEIA_MOON`,
  `PROMETHEIA_MARS`, … as system barycentres, as in [ENGINE.md](ENGINE.md)).
  Catalog bodies use their SPK-ID, which `prometheia_engine_lookup`
  resolves from a designation or proper name, and, from an EPM1 1.2
  catalog, from a numbered body's old provisional designation.
- **Time helpers.** Calendar ↔ JD, UTC ↔ TT across leap seconds
  (1972 onward), the default ΔT and TDB−TT, all as in [TIME.md](TIME.md).

## ABI stability

`PROMETHEIA_ABI_VERSION` (currently 6; version 6 appended `prometheia_options.sidereal_plane` and the `PROMETHEIA_SIDEREAL_PLANE_*` constants, reviewed by the Astrolog side before it landed; version 2 added `prometheia_options.sigma`, version 3 `prometheia_options.precession`, version 4 `PROMETHEIA_CENTER_BODY` and `prometheia_options.center_body`, version 5 the hypothetical-body functions and the `prometheia_elements` and `prometheia_hypothetical` structs, purely additive) names the struct layouts and
function signatures. Any change to them bumps the version, and bindings
compare against `prometheia_abi_version()` at load time. **The check is
equality, not `>=`, while we are on 0.x.** Versions 2 to 4 each appended
fields to `prometheia_options`, and a caller passes its own struct, so a
newer library would read past the end of an older caller's struct. A
binding refuses to bind when `prometheia_abi_version() !=
PROMETHEIA_ABI_VERSION`. Before 1.0,
expect the version to move as the engine grows (houses, fixed stars,
transports). Enumerated values and error codes are only ever appended.

`ephem` ([EPHEM.md](EPHEM.md)) is a complete C99 program written against
this header alone.

Two pkg-config files are generated. The build-tree `build/prometheia.pc`
points at the uninstalled artifacts, so
`PKG_CONFIG_PATH=build pkg-config --cflags --libs prometheia` links
against them as they sit — the library is static-only, so zstd and the
C++ runtime are in the public sections and that plain link line works for
a C or a C++ consumer alike. Outside the source tree,
`cmake --install build --prefix <dir>` installs `libprometheia`, the
headers and a relocatable `lib/pkgconfig/prometheia.pc` (paths relative
to itself): `cc -std=c99 app.c $(pkg-config --cflags --libs prometheia)`.

Exported symbols carry default visibility, so a shared build
(`-DBUILD_SHARED_LIBS=ON`) exports the `prometheia_*` functions.

## Validation

`tests/test_c_api.cpp` runs everywhere on the synthetic kernel
and the in-tree `tests/data/sample-100.epm`:

- **Bit-identity.** Twelve option combinations (all four centres and
  frames, both coordinate kinds, the three sidereal modes, corrections
  and rates switched independently, topocentric site conversion) at two
  epochs, through both `calc` and `calc_ut`. Every double is compared
  with `==` against `Engine`. The same holds for a catalog body with its
  sigma and for a callback ΔT against the equivalent C++ `DeltaTModel`.
- **Boundary.** Defaults equal `CalcOptions{}` field by field, and the
  constants equal the C++ enums. `NULL` is accepted on every pointer.
  Out-of-range selectors, non-finite user anchors, coverage and observer
  errors all return the right status, a message and a zeroed result.
  Over-long messages are truncated. Time helpers equal `prometheia::time`.
- **Houses** (`c_api_houses_match_engine`): all eleven systems at four
  sites (two of them inside the Arctic circle), two instants, tropical and
  Lahiri, TT and UT1, compared with `==` against `Engine`, refusals
  included. `prometheia_houses_from_armc` reproduces each answer from its
  ARMC and obliquity. Sidereal time and obliquity are checked against ERFA,
  and the rotations against fixed points and round trips.
- **Segments** (`tests/test_c_segments.cpp`). A caller's own loop over the
  layout equals `_eval` with `==` at every bound and 300 random instants,
  ecliptic and equatorial. 19 cases (planets, the Moon, catalog bodies,
  nodes, Lahiri, topocentric) are fitted over 12 days at 0.001″ and 0.1″
  and held to the tolerance against `calc_object`. Six contexts fitting
  on six threads give identical coefficients, and six threads evaluating
  one handle give identical answers. The DE441-to-DE440 handover is cut
  and holds 0.001″ on both sides, and the refusals are checked.
  Fault-injected twice: with the cuts disabled the handover fit is
  refused, and aiming at 3 × the tolerance fails three cases.
- **Real C.** `tests/c_api_smoke.c` is compiled as C99 with
  `-Wall -Wextra -pedantic -Werror`. It opens, calcs, handles errors,
  installs a ΔT function pointer and converts leap-second UTC, all without
  any C++ in the caller.

A mutation check, dropping the degree conversion of the site latitude or
ignoring the deflection switch, fails the bit-identity case, as it should.

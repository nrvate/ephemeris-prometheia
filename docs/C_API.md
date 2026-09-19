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

The fixed-star functions (`prometheia_star_find`, `_lookup`, `_info`,
`prometheia_calc_star`, `prometheia_constellation_at`) are described in
[STARS.md](STARS.md).

`prometheia_calc_orbit_point` and `_ut` wrap `Engine::calc_orbit_point`
([ENGINE.md](ENGINE.md), "Nodes and apsides"). The options' corrections
apply to an orbit point as to a body, and all three off give the geometric
point ([ORBIT-POINTS.md](ORBIT-POINTS.md)). An orbit point or elements
selector out of range is `PROMETHEIA_ERROR_ARGUMENT`.

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

## Conventions

- **Handles.** `prometheia_engine_open` creates an opaque engine and
  `prometheia_engine_close` frees it (`NULL` is a no-op). An engine is not
  safe for concurrent use; give each thread its own. The library keeps no
  process-wide state.
- **Status.** Fallible calls return `prometheia_status` (`PROMETHEIA_OK`
  = 0). The last parameter is an optional `prometheia_error*` (`NULL` is
  allowed) that receives the same code and a NUL-terminated message,
  truncated to 255 bytes. Success clears the message.
  | status | meaning |
  |--------|---------|
  | `PROMETHEIA_ERROR_IO` | file cannot be opened or read |
  | `PROMETHEIA_ERROR_FORMAT` | not a recognised or valid ephemeris/catalog |
  | `PROMETHEIA_ERROR_CORRUPTION` | checksum mismatch or truncation |
  | `PROMETHEIA_ERROR_ARGUMENT` | invalid input: `NULL`, out-of-range option, non-finite time, time outside the ephemeris, the observer as the body |
  | `PROMETHEIA_ERROR_NOT_FOUND` | no ephemeris or catalog answers the body or name |
  | `PROMETHEIA_ERROR_INTERNAL` | allocation failure or other internal fault (no C++ exception ever crosses the interface) |
- **Outputs on failure** are zeroed (`*result`, `*utc`, `*body`, `*jd_tt`),
  and `*out` from open is set to `NULL`.
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
  also invalidates the result strings, because the overlay description is
  rebuilt.
- **Delta T.** `prometheia_engine_set_delta_t(engine, fn, user)` installs
  `double fn(void *user, double jd_tt)` (seconds of TT−UT1) for
  `prometheia_calc_ut` and for topocentric Earth rotation. `fn = NULL`
  restores the observed USNO model. The engine does not own `user`.
- **Bodies.** NAIF IDs (`PROMETHEIA_SUN`, `PROMETHEIA_MOON`,
  `PROMETHEIA_MARS`, … as system barycentres, as in [ENGINE.md](ENGINE.md)).
  Catalog bodies use their SPK-ID, which `prometheia_engine_lookup`
  resolves from a designation or proper name.
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

`tests/test_c_api.cpp`, 8 cases, runs everywhere on the synthetic kernel
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
- **Real C.** `tests/c_api_smoke.c` is compiled as C99 with
  `-Wall -Wextra -pedantic -Werror`. It opens, calcs, handles errors,
  installs a ΔT function pointer and converts leap-second UTC, all without
  any C++ in the caller.

A mutation check, dropping the degree conversion of the site latitude or
ignoring the deflection switch, fails the bit-identity case, as it should.

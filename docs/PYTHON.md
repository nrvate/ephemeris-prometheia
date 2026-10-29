# The Python package

`python/prometheia` gives Python two ways in:
- **A native binding** (`prometheia.Dataset`, `Context`, `Options`) runs the
  engine in process through the [C interface](C_API.md). It is a Cython
  extension built against an installed `libprometheia`, and it publishes
  `cprometheia.pxd` for Cython callers ("Native", below).
- **A pure-Python client.** **`prometheia.Client`** exposes the JSON tools as
  they are ([JSON_API.md](JSON_API.md)): a whole chart or a series in one
  call, every answer with its provenance. It talks to a Prometheia server, or
  to a local `prometheia-json`, and needs no compiler.

```python
import prometheia

ds = prometheia.Dataset("ephe/linux_p1550p2650.440", catalogs=["sbdb.epm"])
ctx = ds.local()                                   # this thread's engine
mars = ctx.calc(4, 2451545.0, prometheia.Options(zodiac="lahiri"))  # NAIF 4, JD(TT)
print(mars.lon_deg, mars.source)

with prometheia.Client(ephemeris="ephe") as c:     # the JSON tools, no compiler
    out = c.positions(time="2000-01-01T12:00:00Z", objects=["Mars", "Moon"])
```

**State (2026-09-29):**
- **Done:** all five phases: houses in the engine, the JSON `houses` tool,
  this package over stdio and HTTP, the binary transport (protocol v4 to
  `prometheiad`), and houses as v4 object kind 6 (Astrolog's drop `6235abd`).
- **Not done:** time conversion over v4, which Astrolog and we agreed
  follows houses.

## Decisions (maintainer, 2026-09-29)

- **Houses are computed by the engine** ([HOUSES.md](HOUSES.md)) and served
  to every client, not computed in Python.
- **Three transports, one meaning:**
  - the binary protocol (v4, `prometheiad`) when a server is named in the
    configuration or environment;
  - otherwise a local `prometheia-json` started as a child over stdio;
  - or plain HTTP to a `prometheia-json --http`.
- **Where v4 lacks a call, v4 is extended**, by agreement with Astrolog,
  whose specification it is.
  - Houses are object kind 6 (their drop `6235abd`).
  - Time conversion follows later, and narrower.
- **Polar latitudes:** a system with no defined cusps there refuses; it never
  substitutes another. That is Placidus and Koch inside a polar circle.
  Topocentric, which the first version of this plan listed too, is built
  from pole heights and defined at every latitude short of the pole. Since
  2026-10-27 it, Regiomontanus and Campanus are refused where their cusps
  fall out of zodiacal order ([HOUSES.md](HOUSES.md)).
- **Home:** `python/` in this repository, versioned with the releases and
  covered by `tools/gate.sh`. Publishing to PyPI is a later, separate
  decision.

## Where the answers come from

`Client(server=…)` or `Client(ephemeris=…)` choose directly. A `Client()`
with neither follows the environment, and the first match wins. There is no
module-level state. The `set_server`, `set_ephe_path`, `default_client` and
`close` functions, which configured one process-wide client for
`prometheia.swe`, were removed on 2026-10-27.

1. **`$PROMETHEIA_SERVER`**, with `$PROMETHEIA_TOKEN` for a token:
   - a `ws://` or `wss://` address of a `prometheiad` (protocol v4, binary);
   - or the `http(s)://` address of a running `prometheia-json --http`.
2. **`$PROMETHEIA_EPHEMERIS`**, or `Client(ephemeris=…)`: a JPL DE file, or a
   directory. In a directory it takes DE440, DE441 (behind DE440), every
   `*.epm` small-body catalog, and the SB441 perturber kernel. Several paths
   join with `os.pathsep`, so a catalog kept elsewhere can be named beside
   the directory.
   - A local `prometheia-json` is started over stdio, found through
     `$PROMETHEIA_JSON`, then `PATH`, then this repository's `build/`.
   - `Client.close()` stops it.

**The binary transport** (`_binary.py`, `_v4.py`, `_ws.py`):
- It speaks protocol v4 over a minimal RFC 6455 client, written here with
  the standard library.
- It takes the same tool calls as the JSON transports and answers in their
  shape, so a caller does not know which transport it is on.
- **Held to stdio:** every field it carries is identical, bit for bit, to the
  stdio transport's on the same DE440 (`python/tests/test_binary_live.py`).
  - The test compares the fields the binary answer carries (the rows, not
    the provenance) for the planets and lunar points, the frames,
    topocentric, heliocentric and sidereal output, all eleven house systems,
    stars, nodes and apsides.
- **What v4 does not carry is refused by name:**
  - A clock time (`convert_time`, an ISO time): it needs the leap-second
    table, and the v4 time message agreed with Astrolog follows houses.
    Rows still carry ΔT.
  - A bare name that is not a planet or lunar point, on `Client`. v4 names
    objects by kind, so a star or asteroid is named as `{"star": …}` or
    `{"asteroid": …}`.
- **Checked offline:** the messages it encodes pass our independent v4
  reader, and Astrolog's houses conformance messages decode
  (`python/tests/test_v4_offline.py`).

**Speed** (measured, this machine, loopback):
- Once running, a call costs 0.22 ms over stdio: ten position calls took
  2.2 ms. Over the binary protocol it costs 0.245 ms.
- Startup is 0.18 s with DE440 alone, 3.0 s with DE441 behind it, and 4.8 s
  with DE441, the full SBDB catalog and the perturber kernel.
- `Client.positions` answers many objects and instants in one call.

## Native

The extension `prometheia._native` (`python/prometheia/_native.pyx`) is a
thin layer over `prometheia.h`. Every answer is the C interface's, and so
the C++ engine's, bit for bit.

**Shape** (Prometheia's own; nothing Swiss-shaped):
- `Dataset(ephemeris, *, ephemerides=(), catalogs=(), perturbers=(),
  hypotheticals=())` names the files, in the order they are added.
  - `ephemerides` are planetary files behind the first, such as DE441
    behind DE440.
  - `dataset.context()` opens a new engine over them.
  - `dataset.local()` keeps one engine per thread.
- A `Context` serves the thread that made it, and refuses any other thread
  by name. The calls on it:
  - `calc(object, jd, options, scale="tt"|"ut1")`. The object is a NAIF id
    (catalog bodies go by SPK-ID, and `lookup(name)` finds it),
    `("star", index or name)`, `("hypothetical", token)`,
    `("orbit-point", body, point, method)`, or a registry id string
    (`"sb:136199"`). An int takes the plain `prometheia_calc` path;
  - `phenomena(object, jd, options, scale)` gives a `Phenomena`: the phase
    angle, illuminated fraction, elongation, diameter and V magnitude with
    its model ([PHENOMENA.md](PHENOMENA.md)). A quantity that does not apply
    is `None`;
  - the object registry ([REGISTRY.md](REGISTRY.md)):
    - `object_lookup(query, *, prefix=False, limit=20)` gives `Match`es
      (`id`, `kind`, `quality`, `name`, `matched`, `ambiguous`) across every
      namespace;
    - `object_info(id)` gives an `ObjectInfo`, with the kind, orbit class,
      name and its standing, number, designations, H, V, mean motion and
      period, source, and `object` (the form `calc` and `rise_set` take);
    - `object_list(*, kind=, orbit_class=, named_only=, numbered_only=,
      h_max=, vmag_max=, cursor=, limit=500)` gives `(ids, next_cursor)`,
      and `next_cursor` is `None` after the last page. `KINDS` names the
      kinds;
  - `calc_star(index or name, …)` and `calc_hypothetical(token, …)`;
    `hypothetical(token)` gives a token's definition, a `Hypothetical`
    (`token`, `name`, `set`, `citation`, `elements`);
  - `calc_elements(Elements(epoch_jd_tt, mean_anomaly=, semi_major_axis=,
    eccentricity=, arg_perihelion=, ascending_node=, inclination=,
    equinox="j2000", origin="sun"), jd, options)`: a body from two-body
    elements ([HYPOTHETICALS.md](HYPOTHETICALS.md)). Each element is a number
    or up to five polynomial coefficients in Julian centuries from the
    epoch; `equinox` is a name in `EQUINOXES` or a TT Julian date. A token's
    `hypothetical(token).elements` computes exactly what the token does;
  - `calc_orbit_point(body, point, method, …)`;
  - `fit_segments(object, jd_tt_start, jd_tt_end, options,
    tolerance_arcsec=0.001)` returns `Segments`: Chebyshev fits of any
    object `calc` takes over a TT span ([C_API.md](C_API.md), "Segments").
    `segments.eval(jd_tt)` gives a `SegmentPoint` (Position's fields, from
    the fit), `segments.layout()` the arrays as lists, and `err_arcsec` the
    worst residual measured while fitting. A `Segments` is immutable: any
    thread may evaluate it, and it outlives its context. Cython code
    `cimport`s the layout from `cprometheia.pxd` and evaluates it without
    the GIL;
  - `object_coverage(object, options)` returns `(first_jd_tt, last_jd_tt)`:
    the interval in which `calc` answers the object with those options,
    each end to adjacent doubles, so a fit clipped to it never meets a
    refusal ([C_API.md](C_API.md), "Coverage");
  - `segment_roots(a, b=None, *, component="lon", ca=1, cb=None,
    target=0, target_rate=0, target_jd_tt=None, modulus=0, start=None,
    end=None, tol_days=1e-6)` (module level) returns, in time order, every
    TT instant in [start, end) where `ca a + cb b - (target + target_rate
    (t - target_jd_tt))` passes 0, as `SegmentRoot`s (`jd_tt`, `slope`,
    `err_days`, `tangent`) ([C_API.md](C_API.md), "Roots on segments"). A term
    is a `Segments` or a list of them back to back, all fitted with the same
    options; `component` is a name in `SEGMENT_COMPONENTS`. `cb` is -1 with
    `b` (a difference: phases, aspects) and 0 without; for `"lon"`, `f` is
    taken modulo `modulus` (0 means 360). The window defaults to what both
    terms cover; a moving target needs `target_jd_tt`. The C call runs
    without the GIL and is resumed until done, so the list is whole. The list
    is a `SegmentRoots`: its `unresolved` counts the cells the search left at
    a halving limit, where a pair of roots that cancel may be lost, and a
    `RuntimeWarning` says so when it is above 0;
  - `houses(system, jd, lon, lat, options)`;
  - `sidereal_time(jd_ut1, "mean"|"apparent")` and
    `obliquity(jd_tt, "mean"|"true")`;
  - `rise_set(object, event, jd_ut1_start, lon, lat, height_m=0, *,
    limb=, refraction=, atmosphere=, dip_height_m=, window_days=,
    backward=)` returns an `Event` (`jd_ut1`, NaN unless `outcome` is
    `"found"`). The object takes any form `calc` takes;
  - `lunar_eclipse_next(jd_tt, *, kinds=None, backward=False)` returns a
    `LunarEclipse` (kind, saros series and member, greatest eclipse in TT
    and UT1, gamma, magnitudes, contacts P1 U1 U2 U3 U4 P4 as tuples with
    `None` for a contact that does not occur); `kinds` is a name or a
    sequence of `"total"`, `"partial"`, `"penumbral"`.
    `lunar_eclipse_at(jd_max_tt, site=None)` returns a `LunarEclipseLocal`
    (the eclipse and the Moon's altitude, true and refracted, at each
    contact and the maximum from `site=(lon, lat[, height_m])`). See
    [ECLIPSES.md](ECLIPSES.md);
  - `horizontal(jd_ut1, lon, lat, ra, dec, atmosphere=None, height_m=0)`
    and `equatorial_from_horizontal(…)`. The azimuth runs from north
    through east. An atmosphere is `"standard"`, `(hPa, °C)` or a dict, and
    `None` means none;
  - `ephemeris_spans()`, `catalogs()`, `catalog_bodies()` (the distinct
    bodies between them), `hypotheticals()` and `source`;
  - `set_delta_t(fn)` gives the context a Delta T of its own,
    `fn(jd_tt) -> seconds`, for its UT1 calls and the Earth's rotation;
    `None` restores the observed model. The engine calls `fn` with the GIL
    released and takes the GIL back for it. What `fn` raises goes to
    `sys.unraisablehook`, and the call that needed Delta T fails.
- `Options(observer=, site=, center=, frame=, coordinates=, corrections=,
  zodiac=, sidereal_plane=, precession=, rates=, sigma=)` takes the JSON
  tools' words ([JSON_API.md](JSON_API.md), "Words"). It is built once and
  passed to any number of calls. A word not served is refused by name.
- Results are `Position` and `Houses` objects. `sigma_arcsec` and
  `ayanamsa_deg` are `None` where they do not apply, and `source` says
  what answered.
- Refusals raise `prometheia.Error` with the engine's sentence. Its `code`
  is the C status: `argument`, `coverage`, `not-found`, `io`, `format`,
  `corruption` or `internal`.
- Engine-free functions: `houses_from_armc`, `refraction`,
  `ecliptic_to_equatorial` and `equatorial_to_ecliptic`, the time scales (`utc_to_tt`, `tt_to_utc`,
  `delta_t`, `tdb_minus_tt`, `jd_from_ymdhms`, `civil_from_jd`), and the
  star catalog (`star_find`, `star_lookup`, `star_info`,
  `constellation_at`).
- Every engine call releases the GIL while the engine works.
- **Every public name is at the top of the package:** `prometheia.Dataset`,
  `prometheia.refraction`, `prometheia.ZODIACS`, and the rest. The
  extension module `prometheia._native` is an implementation detail.
- **Typed.** `_native.pyi` and `py.typed` ship with the package. A test
  holds the stub to the module: every public name and method is in it, and
  nothing else.

**Threads and memory** (measured 2026-10-28, this machine). A `Dataset`
loads its files once, through the C dataset ([C_API.md](C_API.md),
"Datasets and threads"), when its first context opens; every context after
that is an engine on the same files:
- The first context opens in 20 ms with DE440 alone, and in 206–233 ms
  with DE440, the full 1,580,028-body catalog and SB441.
- Each further context opens in about 0.01 ms and costs about 8 KB
  either way, measured over 20 contexts each opened on its own thread.
- After two catalog bodies at two instants each, a context costs 0.75 MB.
  Their trajectories are the context's; the perturber tables they were
  integrated through are the dataset's, built once.
- Until 2026-10-28 each context read the files itself: 13.4 MB and 227 ms
  per context with the full catalog and SB441, and 42.5 MB after two
  catalog bodies.

**For Cython callers.** `prometheia/cprometheia.pxd` is published with the
package. It declares all of `prometheia.h`, every function `nogil` and
`noexcept`:

```cython
from prometheia cimport cprometheia as c
cdef c.prometheia_result r
cdef c.prometheia_error err
with nogil:
    st = c.prometheia_calc(engine, 4, jd_tt, &opts, &r, &err)
```

A test holds the `.pxd` to the header, so every function and constant the
header declares is in it.

**Building.** `pkg-config prometheia` finds the library, and nothing is
bundled. The library is static and position-independent, so it links into
the extension. PIC was measured on the engine bench at a geometric mean of
+0.15%, within the noise.
- From the build tree:
  `cd python && PKG_CONFIG_PATH=../build python setup.py build_ext --inplace`,
  with Cython 3 installed.
- Installed: `cmake --install build --prefix <dir>`, then `pip install
  ./python` with `PKG_CONFIG_PATH` naming `<dir>/lib/pkgconfig`.
- Without Cython, or without the library, the package builds as the client
  alone and says so. `PROMETHEIA_NATIVE=1` makes that an error.
- `tools/gate.sh` builds the extension with the Cython in `.venv-build`:
  `python3 -m venv .venv-build && .venv-build/bin/pip install
  Cython==3.0.11 setuptools==75.3.0 wheel==0.44.0`. Then it runs the
  tests.
- **A wheel against an installed library**, checked 2026-10-27:
  `cmake --install` to a prefix, then `PROMETHEIA_NATIVE=1 pip wheel
  --no-build-isolation ./python` with that prefix's `pkg-config` path. The
  wheel installed into a clean venv, answered, and carried
  `cprometheia.pxd`.
- Linux is built and tested. macOS and Windows are untested.
- Not on PyPI. Publishing there is a separate decision.

## Solar eclipses

`Context.solar_eclipse_next(jd_start, kinds=None, *, backward=False,
k_umbra=None, k_penumbra=None, sun_radius_km=None)` returns a
`SolarEclipse` (kind, central, saros, TT and UT1 of greatest eclipse, gamma,
magnitude, obscuration, the greatest-eclipse point, path width, contacts,
`elements` as a `Besselian`); `solar_eclipse_where(jd_tt)` a `SolarWhere`
(centre line and the umbra's limits, `axis_hits` False when it misses);
`solar_eclipse_local_next(jd_start, lon_deg, lat_deg, height_m=0.0)` a
`SolarLocal` (C1-C4 as lists, None for a contact that is missing or below the
horizon). `jd_start` and `jd_tt` are TT; `kinds` are `SOLAR_KINDS` names.
Method and validation: [SOLAR_ECLIPSES.md](SOLAR_ECLIPSES.md).

## Heliacal visibility

`Context.heliacal_next(object, jd_tt, site, *, event="rising",
model="schaefer", ...)` returns a `Heliacal`: the first event strictly after
`jd_tt` (TT), as `jd_tt`/`jd_ut1` (the start of the visibility window),
`optimum_*`, `end_*`, and the circumstances at the optimum. `event` is a
`HELIACAL_EVENTS` name (`rising`, `setting`, `evening-first`, `morning-last`;
a star has the first two), `model` a `VISIBILITY_MODELS` name (`schaefer`, or
`arcus-visionis` with `arc_deg`, which has no default). `site` is
`(lon_deg, lat_deg[, height_m])` or a dict. The air is `pressure_hpa`,
`temperature_c`, `humidity_pct` and at most one of `extinction_coefficient`
and `visual_range_km`; the eye `age_years`, `snellen_ratio`, `binocular`;
also `object_altitude_deg`, `detection_margin_mag`, `moonlight`,
`dark_sky_nl`, `search_days`. `visibility_at(object, jd_tt, site, ...)` is
one moment (`Visibility`), and `limiting_magnitude(sky_brightness_nl, *,
age_years, snellen_ratio, binocular)` the sky's limit alone. Method and
validation: [HELIACAL.md](HELIACAL.md).

## Tests

- **`python/tests`**, run by `tools/gate.sh` with ResourceWarnings as errors:
  - the binary transport against a `prometheiad` it starts, held to stdio's
    answers, with the refusals each transport gives and a batched
    `positions` call against `build/prometheia-json` on DE440;
  - the v4 codec against our independent reader;
  - the native binding (`test_native.py`), held bit for bit to
    `prometheia-json`'s stdio answers for positions over nine option sets
    (planets, the Moon, a catalog body, a star, a hypothetical and a lunar
    node) and for all eleven house systems, refusals included. It also checks
    the `.pxd` against the header and the words against the v4 registries,
    and runs six threads each with its own context. Fault-injected: a
    binding that ignores the aberration switch fails it.
  - segments (`test_segments_native.py`): `Segments.eval` against a Python
    loop over `layout()` in the order C_API.md documents, equal to the last
    bit, in the three coordinate and zodiac cases. Then the fit against
    `calc` for five objects at two tolerances, the refusals, and one handle
    evaluated by four threads after its context closed. Fault-injected: the
    conversion constant one ulp off fails the bit-for-bit case.
    Then `segment_roots` against `Segments.eval` and `calc` (ingresses
    within `err_days`), the same list for capacities 1, 2 and 7, twelve
    month handles against one, the defaults, the refusals, and four threads
    at once.
  - The live tests skip when the binaries or DE440 are absent.

## Phases

1. **Houses in the engine** (done, [HOUSES.md](HOUSES.md)).
2. **`houses` in the JSON API and MCP** (done, [JSON_API.md](JSON_API.md),
   "Houses").
3. **This package over stdio and HTTP** (done).
4. **The binary transport** (done).
5. **Protocol v4 houses (kind 6)** (done: `prometheiad`,
   [SERVER.md](SERVER.md) "House points", and the binary transport).
   Time conversion over v4 follows, by agreement with Astrolog.

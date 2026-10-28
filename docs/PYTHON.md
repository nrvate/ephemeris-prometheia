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

The first match wins:

`Client(server=…)` or `Client(ephemeris=…)` choose directly. The module-level
default client (`prometheia.default_client()`) follows this order:

1. **`set_server(url)`** or `$PROMETHEIA_SERVER`, with `$PROMETHEIA_TOKEN`
   for a token:
   - a `ws://` or `wss://` address of a `prometheiad` (protocol v4, binary);
   - or the `http(s)://` address of a running `prometheia-json --http`.
2. **`set_ephe_path(path)`** or `$PROMETHEIA_EPHEMERIS`: a JPL DE file, or a
   directory. In a directory it takes DE440, DE441 (behind DE440), every
   `*.epm` small-body catalog, and the SB441 perturber kernel. Several paths
   join with `os.pathsep`, so a catalog kept elsewhere can be named beside
   the directory.
   - A local `prometheia-json` is started over stdio, found through
     `$PROMETHEIA_JSON`, then `PATH`, then this repository's `build/`.
   - `Client.close()` (or `prometheia.close()` for the default client) stops it.

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
  - `calc(naif_id, jd, options, scale="tt"|"ut1")`; catalog bodies go by
    SPK-ID, and `lookup(name)` finds it;
  - `calc_star(index or name, …)` and `calc_hypothetical(token, …)`;
  - `calc_orbit_point(body, point, method, …)`;
  - `houses(system, jd, lon, lat, options)`;
  - `sidereal_time(jd_ut1, "mean"|"apparent")` and
    `obliquity(jd_tt, "mean"|"true")`;
  - `ephemeris_spans()`, `catalogs()`, `hypotheticals()` and `source`.
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
- Engine-free functions: `houses_from_armc`, `ecliptic_to_equatorial` and
  `equatorial_to_ecliptic`, the time scales (`utc_to_tt`, `tt_to_utc`,
  `delta_t`, `tdb_minus_tt`, `jd_from_ymdhms`, `civil_from_jd`), and the
  star catalog (`star_find`, `star_lookup`, `star_info`,
  `constellation_at`).
- Every engine call releases the GIL while the engine works.

**Threads and memory** (measured 2026-10-27, this machine). Each context is
an engine of its own until the shared dataset lands (A-14/A-15 of the
astromcp port plan). So each context reads its files itself:
- With DE440 alone, a context costs 0.5 MB and opens in 0.2 ms.
- With DE440, the full 1,567,167-body catalog and SB441, a context costs
  13.4 MB and opens in 227 ms. It grows to 42.5 MB once two catalog bodies
  have been integrated (their trajectories and the perturber tables).
- The Python shape, `Dataset` → `Context`, is the one the shared dataset
  will keep.

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

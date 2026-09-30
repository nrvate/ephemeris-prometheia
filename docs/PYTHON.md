# A Python library: plan (2026-09-29)

A pure-Python package that talks to Prometheia and exposes an API close
enough to pyswisseph's that basic chart code migrates by changing one
import. No C extension, no wrapper around `libprometheia`: the package
speaks to a Prometheia server, local or remote. This is a plan; nothing
here is built yet, and each phase updates this file as it lands.

## Decisions (maintainer, 2026-09-29)

- **Houses are computed by the engine**, from published formulas, and
  served to every client (JSON, MCP, the binary protocol), not
  computed in Python.
- **Three transports, one meaning:**
  - the binary protocol (v4, `prometheiad`) when a server is named in the
    configuration or environment;
  - otherwise a local `prometheia-json` started as a child over stdio;
  - or plain HTTP to a `prometheia-json --http`.
- **Where v4 lacks a call** (houses, UTC↔JD conversion), **v4 is
  extended**, by agreement with Astrolog, whose specification it is. Until
  that lands, those calls in binary mode need a JSON route.
- **House systems, first set:** Placidus, Koch, Porphyry, Regiomontanus,
  Campanus, Equal (from the Ascendant), Whole Sign, Alcabitius, Morinus,
  Meridian (Axial) and Topocentric. Any other letter raises by name.
- **Polar latitudes:** a system with no defined cusps there **refuses**,
  naming the reason and the systems that do answer there. It never
  substitutes another system. That is Placidus and Koch inside a polar
  circle, where part of the ecliptic never rises or sets. Topocentric, first
  listed here too, is built from pole heights and is defined at every
  latitude short of the pole; it answers ([HOUSES.md](HOUSES.md)).
- **Cleanroom:** the API's shape (names, signatures, constant values) comes
  from pyswisseph's published documentation and from introspecting an
  installed pyswisseph (`dir()`, constant values) in a separate oracle venv,
  which also serves as an output oracle the way `swetest` does.
  pyswisseph's C source is never read: it bundles Swiss Ephemeris's.
- **Home:** `python/` in this repository, versioned with the releases and
  covered by `tools/gate.sh`. Publishing to PyPI is a later, separate
  decision.

## The package

```
python/
  prometheia/
    __init__.py      # the native API: positions(), houses(), convert_time()
    swe.py           # the pyswisseph-shaped module: import prometheia.swe as swe
    _transport.py    # stdio child, HTTP, binary v4 (chosen by configuration)
    _v4.py           # the v4 codec and a minimal RFC 6455 WebSocket client
    _flags.py        # pyswisseph flags -> request fields
    _calendar.py     # julday, revjul, day_of_week: local arithmetic
  tests/
```

- **Standard library only.** The WebSocket client (RFC 6455, client side,
  no extensions) is written here; it is a few hundred lines.
- **Transport choice:** `set_server("ws://host:47190")` or
  `PROMETHEIA_SERVER` selects v4; an `http(s)://` URL selects JSON over
  HTTP; with neither, `set_ephe_path(path)` (or `PROMETHEIA_EPHEMERIS`)
  starts `prometheia-json --ephemeris path` over stdio. A request that
  can't be served by the chosen transport raises and says which one would
  serve it; it is never silently rerouted.

## The pyswisseph-shaped surface (`prometheia.swe`)

- **Time:** `julday`, `revjul`, `day_of_week`, `utc_time_zone` (local
  arithmetic); `deltat`, `deltat_ex`, `utc_to_jd`, `jdet_to_utc`,
  `jdut1_to_utc` (the server's ΔT and leap-second table, `convert_time`).
- **Positions:** `calc_ut`, `calc`, `get_planet_name`, returning
  `((lon, lat, dist, dlon, dlat, ddist), retflag)`. Flags map onto request
  fields:
  - `FLG_SPEED` → rates;
  - `FLG_EQUATORIAL` → coordinates;
  - `FLG_TOPOCTR` with `set_topo` → the topocentric observer;
  - `FLG_HELCTR`, `FLG_BARYCTR` → observer;
  - `FLG_TRUEPOS`, `FLG_NOABERR`, `FLG_NOGDEFL` → corrections;
  - `FLG_J2000`, `FLG_NONUT`, `FLG_ICRS` → frame;
  - `FLG_SIDEREAL` with `set_sid_mode` → zodiac;
  - `FLG_XYZ`, `FLG_RADIANS` → converted locally;
  - `FLG_SWIEPH`, `FLG_JPLEPH`, `FLG_MOSEPH` → accepted; the answer is
    always from the JPL ephemeris loaded.
- **Points and catalogues:** the lunar nodes and apogees by their numbers,
  `fixstar2_ut`, catalog bodies as `AST_OFFSET + n`.
- **Sidereal:** `set_sid_mode`, `get_ayanamsa_ut`, `get_ayanamsa_ex_ut`,
  `get_ayanamsa_name`, for the zodiacs the server serves; any other mode
  raises `swe.Error`.
- **Houses:** `houses`, `houses_ex`, `house_name`, `house_pos` for the
  systems above.
- **Local utilities:** `degnorm`, `split_deg`, `difdeg2n`, `cotrans`,
  `azalt`.
- **Errors:** a refused object raises `swe.Error` carrying the server's
  sentence, never a zero-filled tuple.
- **Not in the first release:** eclipses, `rise_trans`, `pheno`, heliacal
  events. Each exists as a function that raises and says so.
- **Drop-in means the calls, shapes and flags, not identical numbers.** Our
  answers differ from Swiss Ephemeris at the milliarcsecond level for
  reasons already measured and explained ([VALIDATION.md](VALIDATION.md),
  [DE.md](DE.md)); the package README says so with the numbers.
- **Speed:** one round trip per `calc_ut`. Estimate, to be measured: under
  a millisecond over stdio or loopback. A batch call,
  `prometheia.positions(...)`, serves loops over bodies and dates in one
  round trip.

## Phases

1. **Houses in the engine** (done, [HOUSES.md](HOUSES.md)). `include/prometheia/houses.hpp`,
   `src/houses.cpp`: cusps and angles (Ascendant, MC, ARMC, Vertex,
   equatorial Ascendant, co-Ascendants, polar Ascendant) for the eleven
   systems, from the true obliquity and apparent sidereal time of date,
   tropical and sidereal. Validated against `swetest`'s house output
   (output only) over latitudes to ±66° and epochs 1600–2500, plus the
   polar refusals. The C ABI is not extended in this phase; an ABI
   addition is relayed to Astrolog before it lands.
2. **`houses` in the JSON API and MCP.** A new tool, same provenance rules.
3. **The Python package over stdio and HTTP.** `prometheia.swe` as above;
   unit tests against recorded JSON (in the gate), an integration test
   against a live `prometheia-json` (skipped when absent), and the
   pyswisseph output oracle in `.venv-oracle`.
4. **The binary transport.** WebSocket client and v4 codec in Python,
   checked against Astrolog's v4 conformance fixtures (the set
   `tests/test_ephproto4.cpp` pins).
5. **The v4 extension for houses and time conversion,** once agreed with
   Astrolog: served by `prometheiad`, spoken by the Python client.

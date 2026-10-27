# The Python package

`python/prometheia` is a pure-Python package with no C extension that
talks to a Prometheia server. **`prometheia.Client`** exposes the JSON tools
as they are ([JSON_API.md](JSON_API.md)): a whole chart or a series in one
call, every answer with its provenance.

```python
import prometheia

with prometheia.Client(ephemeris="ephe") as c:   # a directory with DE440, or a file
    out = c.positions(time="2000-01-01T12:00:00Z", objects=["Mars", "Moon"])
    h = c.houses(time="2000-01-01T12:00:00Z",
                 site={"lon_deg": 8.55, "lat_deg": 47.37}, systems=["placidus"])
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
  from pole heights, is defined at every latitude short of the pole, and
  answers.
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

## Tests

- **`python/tests`**, run by `tools/gate.sh` with ResourceWarnings as errors:
  - the binary transport against a `prometheiad` it starts, held to stdio's
    answers, with the refusals each transport gives and a batched
    `positions` call against `build/prometheia-json` on DE440;
  - the v4 codec against our independent reader.
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

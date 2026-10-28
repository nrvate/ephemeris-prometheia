# prometheia (Python)

Ephemeris Prometheia from Python. `prometheia.Dataset` runs the engine in
process through a native extension, built against an installed
`libprometheia` (`pkg-config prometheia`) and Cython 3. `prometheia.Client`
needs no compiler: it talks to a Prometheia server and gives you its JSON
tools as methods.

```python
import prometheia

ctx = prometheia.Dataset("ephe/linux_p1550p2650.440").local()   # this thread's engine
mars = ctx.calc(4, 2451545.0, prometheia.Options(zodiac="lahiri"))
```

The client:

```python
import prometheia

with prometheia.Client(ephemeris="ephe") as c:   # a directory with DE440, or a file
    out = c.positions(time="2000-01-01T12:00:00Z", objects=["Mars", "Moon"])
    h = c.houses(time="2000-01-01T12:00:00Z", site={"lon_deg": 8.55, "lat_deg": 47.37},
                 systems=["placidus"])
```

`Client(ephemeris=...)` starts a local `prometheia-json` (found through
`$PROMETHEIA_JSON`, `PATH`, or this repository's `build/`).
`Client(server="http://...")` uses a running `prometheia-json --http`, and
`Client(server="ws://...")` a `prometheiad` over protocol v4. A whole chart or
a series is one call, and each answer carries its provenance.

The transports and what each carries are in
[docs/PYTHON.md](../docs/PYTHON.md).

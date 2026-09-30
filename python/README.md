# prometheia (Python)

Ephemeris Prometheia from Python, with no C extension: the package talks to a
Prometheia server and offers an interface shaped like pyswisseph's, so chart
code migrates by changing one import.

```python
import prometheia.swe as swe          # in place of: import swisseph as swe
swe.set_ephe_path("ephe")             # a directory with DE440, or a file
xx, ret = swe.calc_ut(2451545.0, swe.MARS)
cusps, ascmc = swe.houses(2451545.0, 47.37, 8.55, b"P")
```

`set_ephe_path` starts a local `prometheia-json` (found through
`$PROMETHEIA_JSON`, `PATH`, or this repository's `build/`). `set_server`
uses a running `prometheia-json --http` instead.

`prometheia.Client` gives you the JSON tools directly: a whole chart or a
series in one call, with each answer's provenance.

What differs from pyswisseph, and by how much, is in
[docs/PYTHON.md](../docs/PYTHON.md).

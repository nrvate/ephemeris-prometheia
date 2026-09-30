# The Python package

`python/prometheia` is a pure-Python package with no C extension that
talks to a Prometheia server. It has two faces:

- **`prometheia.swe`** is shaped like pyswisseph, so basic chart code
  migrates by changing its import.
- **`prometheia.Client`** exposes the JSON tools as they are
  ([JSON_API.md](JSON_API.md)).

```python
import prometheia.swe as swe          # in place of: import swisseph as swe
swe.set_ephe_path("ephe")             # a directory with DE440, or a file
(lon, lat, dist, dlon, dlat, ddist), ret = swe.calc_ut(jd, swe.MARS)
cusps, ascmc = swe.houses(jd, 47.37, 8.55, b"P")
```

**State (2026-09-29):**
- **Done:** phases 1–3 (houses in the engine, the JSON `houses` tool, this
  package over stdio and HTTP).
- **Not done:** the binary transport and the protocol v4 houses kind.
  Astrolog's houses drop, `6235abd` on their `qt`, is to be vendored.

## Decisions (maintainer, 2026-09-29)

- **Houses are computed by the engine** ([HOUSES.md](HOUSES.md)) and served
  to every client, not computed in Python.
- **Three transports, one meaning:**
  - the binary protocol (v4, `prometheiad`) when a server is named in the
    configuration or environment;
  - otherwise a local `prometheia-json` started as a child over stdio;
  - or plain HTTP to a `prometheia-json --http`.
  - The binary one is not written yet: a `ws://` address raises and says so.
- **Where v4 lacks a call, v4 is extended**, by agreement with Astrolog,
  whose specification it is.
  - Houses are object kind 6 (their drop `6235abd`).
  - Time conversion follows later, and narrower.
- **Polar latitudes:** a system with no defined cusps there refuses; it never
  substitutes another. That is Placidus and Koch inside a polar circle.
  Topocentric, which the first version of this plan listed too, is built
  from pole heights, is defined at every latitude short of the pole, and
  answers.
- **Cleanroom:** the API's shape (names, signatures, constant values) comes
  from pyswisseph's published documentation (its docstrings) and from
  introspecting an installed pyswisseph in `.venv-oracle`. That install is
  also an output oracle, the way `swetest` is. pyswisseph's C source is
  never read: it bundles the Swiss Ephemeris'.
- **Home:** `python/` in this repository, versioned with the releases and
  covered by `tools/gate.sh`. Publishing to PyPI is a later, separate
  decision.

## Where the answers come from

The first match wins:

1. **`set_server(url)`** or `$PROMETHEIA_SERVER`: the `http(s)://` address
   of a running `prometheia-json --http` (`$PROMETHEIA_TOKEN` for a bearer
   token).
2. **`set_ephe_path(path)`** or `$PROMETHEIA_EPHEMERIS`: a JPL DE file, or a
   directory. In a directory it takes DE440, DE441 (behind DE440), every
   `*.epm` small-body catalog, and the SB441 perturber kernel. Several paths
   join with `os.pathsep`, so a catalog kept elsewhere can be named beside
   the directory.
   - A local `prometheia-json` is started over stdio, found through
     `$PROMETHEIA_JSON`, then `PATH`, then this repository's `build/`.
   - `swe.close()` stops it.

**Speed** (measured, this machine, loopback stdio):
- Once running, a call costs 0.22 ms: ten `calc_ut` calls took 2.2 ms.
- Startup is 0.18 s with DE440 alone, 3.0 s with DE441 behind it, and 4.8 s
  with DE441, the full SBDB catalog and the perturber kernel.
- `Client.positions` answers many objects and instants in one call.

## The pyswisseph-shaped surface

**Served:**

| area | calls |
|---|---|
| positions | `calc_ut`, `calc`, `fixstar2_ut`/`fixstar2` (and `fixstar_ut`/`fixstar`), `nod_aps_ut`/`nod_aps` (mean and osculating), `get_planet_name` |
| sidereal | `set_sid_mode` (the served zodiacs; `SIDBIT_ECL_T0` → the anchor plane, `SIDBIT_SSY_PLANE` → the invariable plane, `SIDBIT_USER_UT`), `get_ayanamsa_ut`/`get_ayanamsa` (mean), `get_ayanamsa_ex_ut`/`get_ayanamsa_ex` (true; mean with `FLG_NONUT`), `get_ayanamsa_name` |
| houses | `houses`, `houses_ex` (`FLG_SIDEREAL`), `house_name`, `sidtime` |
| time | `julday`, `revjul`, `day_of_week`, `utc_time_zone` (all local); `utc_to_jd`, `jdet_to_utc`, `jdut1_to_utc`, `deltat`, `deltat_ex` (the server's ΔT and leap seconds) |
| arithmetic | `degnorm`, `radnorm`, `difdegn`, `difdeg2n`, `deg_midp`, `cotrans`, `split_deg` |
| setup | `set_ephe_path`, `set_topo`, `set_jpl_file` (accepted, ignored), `close` |

**How the flags translate:**
- `FLG_SPEED` (or `FLG_SPEED3`) → rates.
- `FLG_EQUATORIAL` → coordinates.
- `FLG_TOPOCTR` with `set_topo` → topocentric (without `set_topo` it
  raises).
- `FLG_HELCTR`, `FLG_BARYCTR` → observer.
- `FLG_TRUEPOS`, `FLG_NOABERR`, `FLG_NOGDEFL` → corrections.
- `FLG_ICRS`, `FLG_J2000`, `FLG_NONUT` → frame.
- `FLG_SIDEREAL` → the `set_sid_mode` zodiac.
- `FLG_XYZ` and `FLG_RADIANS` → converted here.
- `FLG_SWIEPH`, `FLG_MOSEPH` and `FLG_JPLEPH` are accepted, and the answer is
  always from the JPL file loaded. The returned flags say `FLG_JPLEPH`.
- `FLG_CENTER_BODY` (JPL's files hold Mars to Pluto as system barycentres),
  `FLG_JPLHOR` and `FLG_JPLHOR_APPROX` raise.

**Bodies:**
- 0–22 as pyswisseph numbers them. Chiron, Pholus, Ceres, Pallas, Juno and
  Vesta are catalog bodies, so they need a catalog.
- `AST_OFFSET + n` is catalog body n.
- `FICT_OFFSET + i` is protocol v4's named hypothetical i. The served ones
  are the Hamburg points and Le Verrier's Neptune ([HYPOTHETICALS.md](HYPOTHETICALS.md)).

**Refusals raise `swe.Error`** with the server's sentence and code, and never
answer with zeros. An asteroid with no catalog loaded, Placidus inside a
polar circle and a body not served all raise.

**Different on purpose:**
- `houses` returns `ascmc[5:8]` (the co-Ascendants and the polar Ascendant)
  as NaN. They are not served, since we have adopted no published definition.
- `get_ayanamsa_name` gives our zodiac token (`"lahiri"`).
- `house_name` gives our system names.
- `fixstar2_ut` gives the resolved star name without a designation suffix.

**Not in this release** (each raises by name): eclipses and occultations,
`rise_trans`, `pheno`, heliacal events, `houses_armc`, `houses_ex2` (cusp
speeds), `house_pos`, Gauquelin sectors, `azalt`, `refrac`, crossings,
orbital elements, star magnitudes, `set_delta_t_userdef`.

## Against pyswisseph

`tools/check/pyswe_oracle.py` makes the same calls through pyswisseph
2.10.03 and this package, both on the same DE440 file (`FLG_JPLEPH`). It
runs from `tools/scheduled.sh`. Measured 2026-09-29, 1800–2100 unless
stated.

| what | worst difference | why |
|---|---|---|
| Sun, Moon, planets at TT: apparent, equatorial, J2000, geometric, sidereal | **0.0027″**; distance 1e-8 relative | the engines' known differences ([VALIDATION.md](VALIDATION.md)) |
| heliocentric apparent, at TT | 0.77″ (Mercury, 1800); geometric agrees to 0.0005″ | Swiss's heliocentric light time ([CROSS-TEST.md](CROSS-TEST.md)) |
| the Moon's mean apogee, at TT | 0.50″ at 1800, 0.007″ at 2000 | different mean-element expressions (ours from USNO Circular 179) |
| `calc_ut`, 1900–2026 | 0.44″ (the Moon, 1900) | ΔT: 0.65 s apart at 1900 |
| `calc_ut` at 2100 | about 1′ on the Moon | ΔT predictions 110 s apart |
| topocentric position, 1900–2026 | 0.14″ (the Moon) | ΔT turns the site |
| rates | 0.25″/day (the Moon); topocentric 10″/day | pyswisseph's reported rates depart from its own positions |
| houses at 47.37°N | 2.1″ at 2100 | pyswisseph's sidereal time departs from IAU 2006/2000A ([HOUSES.md](HOUSES.md)) |
| ayanamsha (Lahiri) | 0.0015″ mean, 0.0025″ true | |
| `deltat` | 109.6 s at 2100; 0.3 s at 2026 | models (ours observed to date, then a trend) |
| `utc_to_jd` | TT 4e-5 s; UT1 0.10 s (2024) | UT1 − UTC: ours observed |
| `jdut1_to_utc` | 0.29 s (2026) | the same |
| `julday`, `revjul`, `cotrans` | 0 | |
| `split_deg` | 160 of 78,592 flag and value cases | all combine `NAKSHATRA` with `KEEP_DEG` or rounding across 360° |

**The rates.** Each side's reported rate was compared with the central
difference of its own positions (h = 1e-3 day).
- **Ours** agree to 0.008″/day, and 0.047″/day topocentric, which is the
  difference formula's own floor.
- **pyswisseph's** depart by up to 0.11″/day on the Moon, and 9.9″/day for
  the topocentric Moon.
  - Two examples: the Sun's latitude rate at J2000 reads −5.8e-7°/day
    against −6.49e-6°/day from its positions.
  - The topocentric Moon at J2000 reads 10.37272°/day against 10.36934.

## Tests

- **`python/tests`**, run by `tools/gate.sh` with ResourceWarnings as errors:
  - the flag translation and answer shapes against a scripted client;
  - calendar arithmetic;
  - `split_deg` against observed pyswisseph outputs;
  - a live run against `build/prometheia-json` on DE440, which skips when
    either is absent.
- **`tools/check/pyswe_oracle.py`**, run by `tools/scheduled.sh` when
  pyswisseph is installed in `.venv-oracle`.

## Phases

1. **Houses in the engine** (done, [HOUSES.md](HOUSES.md)).
2. **`houses` in the JSON API and MCP** (done, [JSON_API.md](JSON_API.md),
   "Houses").
3. **This package over stdio and HTTP** (done).
4. **The binary transport:** a WebSocket client and v4 codec in Python,
   checked against Astrolog's conformance fixtures.
5. **Protocol v4 houses (kind 6)** in `prometheiad` and the Python client.
   Astrolog's drop is `6235abd`; time conversion follows.

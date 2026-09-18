# ephem: command-line positions

`ephem` (`tools/ephem/ephem.c`) prints positions from a planetary ephemeris
and any number of small-body catalogs. It is the project's debugging tool and
the first real consumer of the [C interface](C_API.md). It is written in strict
C99 against `prometheia.h` alone, so everything it shows can be reached from
C. Built by default: `build/ephem`.

```sh
export PROMETHEIA_EPHEMERIS=ephe/linux_p1550p2650.440

ephem                                   # Sun, Moon, Mercury..Pluto now (UTC)
ephem -t 2026-09-16T12:00 moon mars     # apparent, ecliptic of date
ephem -t 2000-01-01T12:00 --equatorial --dms moon
ephem -t 2026-01-01 -n 31 -s 1d -f csv venus > venus.csv
ephem --site 8.55,47.37,500 --sidereal lahiri sun moon
ephem -c sbdb.epm ceres @433 "2004 MN4" --frame j2000 --geometric
ephem --scale ut1 -t 1900-01-01T12:00 --delta-t -2.7 moon   # before 1972
```

```
# JPL DE440 binary
# geocentric, apparent, ecliptic and true equinox of date, tropical
# 2026-09-16 12:00:00.000 UTC, JD 2461300.000801 TT, Delta T 69.117 s
body              longitude      latitude     distance AU       lon/day       lat/day       dist/day  sigma
Sun            173.64818020   -0.00012896    1.0053982237    0.97515987   -0.00000294  -0.0002750910
Moon           237.62718531   -5.22939194    0.0026733470   12.16904601   -0.03728206   0.0000211734
```

## Input

**Bodies**
- `sun moon mercury venus earth mars jupiter saturn uranus neptune pluto emb ssb`, in any case.
- A plain integer is a NAIF ID or SPK-ID. As in the engine, Mars through Pluto are the system barycentres 4–9.
- Any other text is looked up in the loaded catalogs by proper name or designation.
- `@TEXT` forces a catalog lookup, so `@1` is Ceres while `1` is NAIF 1.
- With no bodies, `ephem` prints the Sun, Moon and Mercury through Pluto.
- `--` ends option parsing, for body arguments that begin with `-`.

**Data**
- `-e FILE` or `$PROMETHEIA_EPHEMERIS` names the planetary ephemeris: a DE binary or an SPK kernel, detected by content.
- `-p FILE` or `$PROMETHEIA_PERTURBERS` adds an asteroid perturber kernel (JPL's `sb441-n16.bsp`) to the small-body force model; see [ENGINE.md](ENGINE.md).
- `-c FILE` adds an EPM1 catalog and can be repeated. Catalogs in `$PROMETHEIA_CATALOGS` (colon-separated) load first, and later catalogs win.
- `--hypotheticals FILE` adds an element file of named hypothetical bodies (JSON Lines; [HYPOTHETICALS.md](HYPOTHETICALS.md)) and can be repeated. Files in `$PROMETHEIA_HYPOTHETICALS` (colon-separated) load first, and a later file's definition of a token wins. A malformed file stops `ephem` before any output, naming the file and line.

**Time** (default: now)
- `-t YYYY-MM-DD[THH:MM[:SS.s]][Z]` is read as UTC by default. Negative years are allowed on the TT or UT1 scales.
- `-j JD` is read as TT by default.
- `--scale utc|tt|ut1` states which scale the input is in.
- UTC runs from 1972 onward (the integer-leap-second era) and accepts `23:59:60.x` in an inserted leap second. Earlier epochs use `--scale tt` or `--scale ut1`.
- A Julian date is never UTC.
- `-n N -s STEP` prints N rows. STEP is in days, or takes a unit: `d`, `h`, `m`, `s`. It may be negative. Steps are uniform in TT, or in UT1 with `--scale ut1`.
- `--delta-t SECONDS` replaces the observed USNO ΔT with a fixed value. It affects UT1 input and topocentric Earth rotation, and is useful for matching other programs.

**Observer, frame, corrections**
- `star:NAME` as a body: a fixed star or Messier object by any name or designation (docs/STARS.md).
- `hyp:TOKEN` as a body: a named hypothetical body (`hyp:cupido`), from the element set the library ships or a `--hypotheticals` file, matched case-insensitively; `hyp:all` is every body defined, in definition order. Corrections, observers and frames apply as to any body ([HYPOTHETICALS.md](HYPOTHETICALS.md)).
- `--orbit-point asc|desc|peri|apo[:mean|:osc]`: a node or apsis of each body's orbit instead of the body (osculating unless `:mean`; docs/ENGINE.md, "Nodes and apsides").
- `--center geo|topo|helio|bary|BODY`: BODY is a built-in name (`mars`) or a NAIF ID, and positions are seen from that body's centre with light time, deflection and aberration for an observer moving with it.
- `--site LON,LAT[,H]`: geodetic degrees east and north, metres above WGS84. It implies `topo`.
- `--frame true|mean|j2000|icrf`, `--equatorial`.
- `--precession iau2006|vondrak2011`: the long-term model for epochs centuries or more from J2000.
- `--sidereal fagan-bradley|fb|lahiri|user:JD:DEG|tropical`. For `user`, DEG is the mean ayanamsha at the TT Julian date JD.
- Apparent place is the default. `--astrometric` applies light time only and `--geometric` applies no corrections. `--no-light-time`, `--no-deflection`, `--no-aberration` and `--no-speed` switch off one correction each. `--no-sigma` skips catalog uncertainties, which cost twelve extra integrations per small body.

The option meanings, models and accuracy are the engine's: see [ENGINE.md](ENGINE.md).

## Output

`-f table` (the default) is for reading:
- The header carries the ephemeris, the options in words, and the instant with its ΔT.
- A series adds UTC and JD (TT) columns to every row.
- `--dms` prints angles as degrees, minutes and seconds, or hours, minutes and seconds for RA.
- Catalog bodies whose record carries a full orbit covariance show their 1σ direction uncertainty.

`-f csv` and `-f json` are for programs. Each has one row per body per instant, with every double printed to 17 significant digits so it round-trips exactly:

| field | |
|-------|-|
| `utc` | UTC label to the millisecond, leap seconds as `:60`; empty/`null` before 1972 |
| `jd_tt` | the instant |
| `body`, `id` | the name as given (or the known planet name) and the NAIF/SPK-ID |
| `lon_deg lat_deg` or `ra_deg dec_deg` | degrees |
| `dist_au` | AU |
| `…_speed` | per day (zero with `--no-speed`) |
| `x_au y_au z_au`, `vx…` | the rectangular vector in the output frame (JSON: `xyz_au`, `vel_au_day`) |
| `light_time_days` | τ applied |
| `sigma_arcsec` | empty/`null` unless the catalog record carries a full covariance |
| `ayanamsa_deg` | empty/`null` unless sidereal |
| `source` | provenance (the ephemeris, plus the catalog overlay for small bodies) |

## Exit status

- **0**: every requested row was printed.
- **1**: some body failed, for example an unknown name, time outside coverage, or the observer requested as the body. Rows that succeeded are still printed. Each failure is reported on stderr.
- **2**: usage or setup error: a bad option or value, no ephemeris, or a file that cannot be opened. Nothing goes to stdout.

## Validation

`tests/test_ephem.cpp` (ctest `ephem`) runs the built binary as a
subprocess on the synthetic kernel and `tests/data/sample-100.epm`.

**Exact agreement with the engine.** Every value `ephem` prints in CSV must equal, bit for bit, what `prometheia::Engine` answers for the same request. The checks cover:
- JD, UTC and leap-second input, and stepped series;
- nine option combinations, including `--opt=value` syntax and the site/topocentric implication;
- UT1 with a fixed ΔT, and TT calendar dates before 1972;
- catalog bodies by name, `@designation` and SPK-ID, from `-c` or `$PROMETHEIA_CATALOGS`.

**Structure.** The table header and the `--dms` output are checked, along with the JSON keys, nulls and closing, and the version and help text.

**Errors.** There are sixteen usage and setup errors, each checked for status 2, stderr only. Partial failure is checked for status 1 with the good rows still printed.

A mutation check fails the suite, as it should: negating the site latitude, or leaving deflection on under `--astrometric`.

On DE440, `ephem -t 2026-09-16T12:00` agrees with `swetest -ut12:00` run on the same file. The residuals are what the ΔT difference predicts: the Sun differs by 0.015″ and the Moon by 0.18″, because swetest uses 68.82 s against our 69.12 s ([TIME.md](TIME.md)).

# The object registry

What an engine can answer, and what each object is. One id scheme covers
every namespace: ephemeris bodies, catalog bodies, orbit points, hypothetical
bodies, stars and deep-sky objects. Lookup searches all of them at once, info
describes one, and listing pages through them. The C++ API is
`prometheia::registry::Registry` (`include/prometheia/registry.hpp`). The C
API is `prometheia_object_*` ([C_API.md](C_API.md)), and Python has
`Context.object_lookup`, `object_info` and `object_list`
([PYTHON.md](PYTHON.md)).

## Ids

| Form | Example | Names |
|---|---|---|
| `naif:<id>` | `naif:10`, `naif:301`, `naif:5` | the Sun, the Moon, the planets (Mars to Pluto: their system barycentres), the Earth, the Earth–Moon barycentre (3) |
| `sb:<designation>` | `sb:136199`, `sb:2017 OF201`, `sb:1P` | a catalog body by SBDB's primary designation |
| `orbit:<body id>:<point>:<method>` | `orbit:naif:301:ascending-node:mean` | a node or apsis; the point is `ascending-node`, `descending-node`, `perihelion` or `aphelion` (`perigee` and `apogee` are read as these), and the method is `mean`, `osculating` or `interpolated` |
| `hyp:<token>` | `hyp:cupido` | a hypothetical body |
| `star:<designation>` | `star:HR 3982`, `star:M 44` | a star or deep-sky object by HR, else HIP, else Messier number, else its first designation |

**Stability.**
- A numbered body's id is its number (`sb:136199`), which does not change
  between catalog releases.
- An unnumbered body's id is its provisional designation. When the body is
  numbered, a 1.2 catalog still resolves that designation through the old
  one it carries (`sb:2003 VB12` answers Sedna as `sb:90377`). The old id
  keeps resolving, and info gives the new canonical one.
- A star's HR or HIP number is fixed. Its catalog index is not, and no id
  uses it.

`info` accepts any spelling that resolves, such as `sb:ceres`,
`star:Regulus` or `orbit:naif:301:apogee:mean`. Its `id` is always the
canonical one. Every star's id resolves back to that star (tested over the
whole catalog).

## Lookup

`lookup(query, prefix, max)` searches every namespace and returns the matches
best first. Several matches of the best quality mean the query is ambiguous,
and **nothing is chosen**. The Python `Match.ambiguous` marks them. On the
full catalog, "Pluto" is both `naif:9` and `sb:134340`, "Lilith" both the
Moon's mean apogee and `sb:1181`, and "cupido" both the Hamburg point and
`sb:763`. Each of these is reported, and the caller decides.

- **Matching:**
  - ASCII case, accents and punctuation do not count. `fold()` lowercases
    the query, takes the marks off Latin letters (U+00C0–U+017F) and drops
    everything but letters and digits. So "Gǃkúnǁʼhòmdímà" finds SBDB's
    "G!kun||'homdima" (229762).
  - A provisional designation typed without its space works too:
    "2003VB12".
- **Quality**, best first:
  1. exact, or a numbered body's old designation (`old-designation`:
     "2003 VB12" finds 90377);
  2. alias (a star's Bayer component left out, "Beta Sco");
  3. prefix, with `prefix` set: the start of a name or of any word in it,
     at least two characters. Prefixes reach catalog bodies by proper name
     only, not by designation.
- **Within a quality**, the order is ephemeris bodies, lunar points,
  hypotheticals, stars (brighter first), then catalog bodies.

## Info

| Field | Meaning |
|---|---|
| `kind` | `planet` (the Sun, the planets, the Earth, the barycentre), `moon`, `lunar-point` (an orbit point of the Moon), `orbit-point`, `hypothetical`, `star`, `deep-sky`, `asteroid`, `comet` |
| `orbit_class` | SBDB's code from a 1.2 catalog (`MBA`, `CEN`, `TNO`, `JFc`…) |
| `name`, `standing`, `standing_source` | the proper name, and where it comes from: `iau` (the planets: "IAU"; small bodies: "IAU WGSBN"; stars: "IAU WGSN"), `traditional` (a star's curated name: "Allen (1899)"), `defined` (a lunar point's conventional name, a hypothetical's element set with its citation), or `none` |
| `number` | the MPC number: a numbered asteroid's, or a periodic comet's ("1P" is 1) |
| `designation`, `provisional_designation` | the primary designation, and a numbered body's old one |
| `h_mag`, `g_slope`, `diameter_km` | from the catalog record, at the decimal the record was written from |
| `vmag` | stars and deep sky |
| `elements_epoch_jd` | TDB: catalog bodies and hypotheticals |
| `mean_motion_deg_day` | the mean longitude's rate in the ecliptic of date, signed (retrograde negative) |
| `sidereal_mean_motion_deg_day`, `period_days` | the same against a fixed equinox, and the sidereal period |
| `source` | what answers for it: the ephemeris, the catalog file, the star catalog, or the element set |
| `ref` | how to compute it: the `Engine::ObjectRef` that `calc_object` and `rise_set_ut` take |

A field the object does not have is NaN or empty.

**Mean motion.** The tropical rate is the sidereal rate plus general
precession, 5028.796195″ per century (IAU 2006 at J2000). Each kind takes
its sidereal rate from its own model:

- **The planets:** n = √(GM/a³) from the engine's mean elements, its DE440
  fit, at J2000. GM is the Sun's plus the body's. The Sun's rate is the
  Earth–Moon barycentre's.
- **The Moon:** the rate of its mean longitude F + Ω, from the fundamental
  arguments, less general precession.
- **Lunar points:**
  - the mean nodes: Ω's rate;
  - the mean apsides: ϖ = L − l's rate;
  - a natural apsis: the mean apsis's rate, since it follows that on
    average;
  - an osculating point: none.
- **A planet's mean points:** the rate of the fit's angles at J2000.
- **Catalog bodies:** Kepler with the Gaussian constant, the body massless.
  Open orbits have none.
- **Hypotheticals:** the mean anomaly's rate (its polynomial's, or the Kepler
  motion when M is quoted at the epoch, [HYPOTHETICALS.md](HYPOTHETICALS.md))
  plus the secular rates of ω and Ω. Elements of date already count from the
  moving equinox.

Measured with DE440 and `tests/data/sample-100.epm`, 2026-10-27:

| Object | Tropical, °/day | Sidereal period |
|---|---|---|
| Sun | 0.9856459 | 365.2569 d (the sidereal year is 365.2564 d; the fit's mean a gives the rest) |
| Moon | 13.1763965 | 27.3217 d |
| Mean node | −0.0529538 | 18.600 yr |
| Mean apogee | 0.1114035 | 8.850 yr |
| Jupiter | 0.0831312 | 4332.50 d |
| Ceres | 0.2143427 | 1679.85 d |

## Listing

`list(filter, cursor, max)` returns one page and the cursor of the next (0 at
the start, and 0 after the last page). The order is fixed: ephemeris bodies,
lunar points (the ten canonical ones), hypotheticals, stars and deep sky by
catalog index, then catalog bodies by SPK-ID. Orbit points of other bodies
are not listed, since every body has eight; ask for them by id.

The filter's conditions all hold at once:
- **kinds:** a bitmask of `1 << kind`, or 0 for every kind;
- **orbit_class:** an SBDB code (an unknown one is an ArgumentError);
- **named_only**, **numbered_only**;
- **h_max**, **vmag_max**: limits, NaN for none.

A condition on a property passes only objects that have the property. An H
limit passes no star, and a V limit passes no asteroid.

## Cost

Measured on the full catalog `sbdb-20261027.epm` (1,580,028 bodies) with
DE440 (2026-10-27):

| | Time | Memory |
|---|---|---|
| The engine's name index, on the first lookup (shared with `Engine::lookup`) | 0.49 s | +48 MB |
| The registry's folded names, on the first lookup | 0.26 s | small (the named bodies only) |
| A lookup after that | 5 µs–1 ms | |
| The listing summary, on the first page that reaches catalog bodies | 0.40 s | +36 MB at the peak, 25 MB kept |
| A listing page after that (named centaurs) | 3 ms | |
| `info` on a catalog body | 0.17 ms | |
| `calc` by id string, after its first resolution | 4 µs (3 µs by SPK-ID) | |

The registry keeps its indexes, and the ids it has resolved, until a catalog
is added. A scan that computes the same objects every step should resolve
once and pass `ref` (in Python, `ObjectInfo.object`), although the id string
costs almost nothing once it is resolved.

## Not yet

- **Facies (M 22), Aculeus (M 6), Acumen (M 7) and Capulus (NGC 869/884).**
  Allen (1899), which every curated star name must occur in, has none of
  these four as a name. The objects themselves are served by designation:
  `star:M 22`, `star:M 6`, `star:M 7`, and since 2026-10-28 `star:NGC 869`
  and `star:NGC 884` ([STARS.md](STARS.md)).
- **Orbit-point lookup by name for other bodies** ("Jupiter north node"). The
  id is the way to ask.

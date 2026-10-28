# Changelog

Notable changes to Ephemeris Prometheia. Accuracy and performance figures in
this file are measured, not estimated; where a number is an estimate it says
so. Each entry points at the document carrying the method.

**Versioning.** `0.x` releases may change the C ABI. The C interface described
in [docs/C_API.md](docs/C_API.md) is a cross-repo contract — the Astrolog
project's ephemeris plugin builds against it — so any ABI change is
communicated to known consumers *before* it lands, never shipped unannounced.
That policy is what `0.x` means here: the interface is deliberate and
documented, not that it is frozen.

## Unreleased

- **NGC 869 and 884, the Double Cluster, in the star catalog.** Found by
  designation ("NGC 869", `star:NGC 869`), with the source NGC 2000.0
  (Sinnott 1988, NASA HEASARC; positions to 0.1 min and 1′), because SIMBAD
  could not be reached. `PROMETHEIA_ASTROMETRY_NGC2000` (3) is appended in
  C; no struct changed (docs/STARS.md).
- **Segments through the C API** (A-16; docs/C_API.md, "Segments";
  docs/SEGMENTS.md). `prometheia_segments_fit` fits any object, with the
  options of a position call, to a tolerance in arcseconds. `_eval`
  answers an instant without the engine, `_get_layout` hands a kernel the
  coefficients as a documented struct of arrays, and `_close` frees them.
  The ABI version is unchanged; the signatures were reviewed by the
  Astrolog side first.
  - A handle is immutable: any thread may evaluate it, and it outlives the
    engine that fitted it.
  - The fit aims at 0.8 × the tolerance, and refuses a tolerance it cannot
    reach rather than returning a handle that misses it. One refused
    instant fails the whole fit.
  - Where a second planetary file takes over (0.34″ for the Moon from
    DE441 to DE440 at 1550), the span is cut and each side fitted on its
    own.
  - Measured at 0.001″ over a year, 10,000 random instants per case: the
    planets, the Moon, four catalog bodies, the mean and osculating nodes,
    Lahiri and topocentric cases all within the tolerance (worst 0.000766″,
    the Sun). Fits cost 11–43 ms a year geocentric and 130–180 ms
    topocentric (257 segments, for the diurnal term). Evaluation costs
    150–230 ns.
  - Python: `Context.fit_segments` returns `Segments` (`eval`, `layout`),
    and `cprometheia.pxd` declares the layout for nogil kernels.
- **Python: the last C calls** (A-17). `Context.calc_elements` with
  `Elements`, `Context.hypothetical` (a token's definition),
  `Context.catalog_bodies` and `Context.set_delta_t`. Every function in
  `prometheia.h` is now reached from Python, except the engine open and
  `_add_*` calls, which `Dataset` replaces (docs/PYTHON.md, "Native").
- **Fixed: orbit points rise without parallax.** `rise_set_ut` (and C's
  `prometheia_rise_set_ut`, Python's `Context.rise_set`) now takes an orbit
  point as a direction from the Earth's centre. It applied the site
  parallax at the Moon's distance to the Moon's nodes and apsides, which
  moved their rise and set by minutes: astromcp measured the true node 233
  s late against libswe. The Moon's true and mean nodes and its osculating
  apogee now match `swetest` to 0.04 s, disc centre without refraction
  (docs/ENGINE.md, "Rise, set and transit"). Bodies, stars and
  hypotheticals are unchanged.
- **Tests: test_catalog's temporary files carry the process id**, as every
  other test's do, so two gates at once no longer write each other's files.
- **Datasets in C, and shared perturber and apsis tables (A-15).**
  `prometheia_dataset_open`, `_add_*`, `_freeze` and `_close`, with
  `prometheia_context_open`, load the files once for an engine per thread
  (docs/C_API.md, "Datasets and threads"). Additive: the ABI version stays
  6, and the signatures went to the Astrolog side before they landed. The
  perturber tables a catalog body is integrated through, and the Moon's
  apsis passages, are now built once per corpus and shared by its engines
  (docs/ENGINE.md, "Threads"). Answers are bit-identical to before: 3,728
  catalog-body and apsis answers compared. A catalog-body run is 4% faster
  in cycles, best of five. `test_c_dataset` runs six contexts on six
  threads against one engine, and catches a block sampled through the
  wrong scratch. Python's `Dataset` now loads its files once, through the
  C dataset, and its contexts share them. A refused perturber kernel no
  longer uses up a read slot.
- **prometheiad: one corpus for every event loop**
  ([docs/SERVER.md](docs/SERVER.md), "Event loops"), prometheiad's part of
  A-15 in astromcp's port plan. `main` opens the files once, into a frozen
  `Corpus`, and each loop's engine is a context on it. Until now every
  loop opened the files itself, and the startup probe opened them once
  more. Measured with DE440, `sb441-n16.bsp` and the 147 MB SBDB catalog
  at `--threads 8`:
  - resident memory: 247 → 139 MB (live heap 202 → 41 MB);
  - start to listening: 1.49–1.59 → 1.02–1.06 s;
  - single-loop throughput: no lower (charts 13,819–13,967 → 14,262–14,366
    requests a second).
  No answer and nothing in the protocol changes.
- **Fixed: about 95 MB held after startup by the dataset digest**
  ([docs/SERVER.md](docs/SERVER.md), "Event loops"). Each digest thread,
  one per core, read a whole 8 MiB piece into its own buffer, and glibc
  kept those buffers resident; `malloc_trim(0)` did not return them. A
  piece is now read in 64 KiB blocks. The dataset id is unchanged.
  Measured for `prometheiad` at `--threads 8`, resident memory after
  startup:
  - DE440 with the perturbers and the SBDB catalog: 135 → 40 MB;
  - DE440 with the perturbers and sample-100: 113 → 18 MB;
  - startup is no slower.
  `prometheia-json` uses the same digest.
- **Fixed: non-ASCII paths on Windows, for every open.** The catalog
  writer, `de::trim`, `spk::write_spk`, the engine's sniff of an
  ephemeris's kind and `add_hypotheticals` opened their files in the ANSI
  code page. A cut could not be written to a non-ASCII path, and
  `Engine::open` could not open one. They now use `io::open_file` and
  `io::native_path`, which widen the UTF-8 path on Windows, and
  `RandomAccessFile`. Checked under Wine (docs/EMBEDDING.md, "Platforms").
  Astrolog's MSVC run of the readers (A-12) passed: all sources compiled,
  6,477 checks, and 71 numeric lines equal to Linux.
- **Fixed: the build for macOS below 26.** The object registry (A-9)
  called floating-point `std::to_chars` and `std::from_chars`, which Apple's
  libc++ marks unavailable below macOS 13.3 and 26.0. Astrolog's macOS 12
  build failed on them. They are now behind the `__cpp_lib_to_chars` guard
  `src/hypotheticals.cpp` already uses, with a classic-locale fallback equal
  to `to_chars` bit for bit (docs/EMBEDDING.md, "Platforms").
- **Corpus and engines** ([docs/ENGINE.md](docs/ENGINE.md), "Threads"),
  A-14 of astromcp's port plan. What an engine reads is a `Corpus`
  (`Corpus::open`, `add_*`, `freeze`). A frozen corpus is read-only and
  shared by any number of engines, one per thread
  (`Engine::open_context`). Each engine keeps its own caches and integrated
  trajectories; `Engine::open` builds a private corpus as before.
  Measured per engine: 8.0 KB when opened, 241 KB after the planets,
  417 KB after a catalog body. The nutation interpolator's 229 KB node
  cache is now allocated on first use. A six-thread test asks 1200 mixed
  questions of six engines on one corpus, and every answer matches a single
  engine's bit for bit. `tools/tsan.sh` runs that test and the readers'
  under ThreadSanitizer, clean; it is outside the gate. C++ only: the C API
  is unchanged (its dataset calls are A-15).
- **Read cursors**, A-13 of astromcp's port plan. The DE, SPK and catalog
  readers hold no mutable state. The record or chunk a read decodes goes
  into the caller's cursor (`de::DeCursor`, `spk::SpkCursor`,
  `catalog::ChunkCursor`), so one open file serves several threads at once,
  each with its own cursor. Tests run two threads through one file of each
  kind; with a cursor shared between them (fault-injected) the DE test
  fails. The forms without a cursor still work, reading into a cursor of
  their own on each call. The engine reads through one `ReadScratch`, and
  its sources and catalogs are const once loaded; its own caches are
  unchanged, and the engine bench is unchanged within run-to-run noise
  (one sitting, one pinned core). `catalog::Reader::stats()` is gone: the
  cursor counts its reads and hits. ABI unchanged.
- **Positional file reads** (`prometheia/io.hpp`), A-12 of astromcp's port
  plan. The DE, SPK and catalog readers read through one
  `io::RandomAccessFile`. On POSIX it uses `pread`, retried on EINTR, at
  64-bit offsets. On Windows it uses `ReadFile` with an `OVERLAPPED` offset,
  and opens the UTF-8 path widened for `CreateFileW`, so a path outside the
  ANSI code page opens. Reads carry their own offset, and no file position
  is shared. Tests read one file from two threads, and read past 4 GiB in a
  sparse file; with a shared position (seek, then read) the two-thread test
  fails. The catalog
  reader no longer leaks its file handle. ABI unchanged.
- **Heliacal visibility** ([docs/HELIACAL.md](docs/HELIACAL.md)), A-8 of
  astromcp's port plan: the heliacal rising and setting of stars and, for
  planets and the Moon, also the evening first and morning last, under
  Schaefer's 1993 visibility model (air masses, layered extinction, sky
  brightness, Hecht's threshold, the observer's age and Snellen ratio) or an
  arcus visionis with the caller's arc. C: `prometheia_heliacal_next`,
  `prometheia_visibility_at`, `prometheia_limiting_magnitude` with
  `prometheia_air`, `prometheia_observer` and `prometheia_visibility_model`
  and their `_init` functions (additive; the ABI version is unchanged);
  Python: `Context.heliacal_next`, `Context.visibility_at`,
  `limiting_magnitude`. Eq. 15c's twilight term is misprinted in the 1993
  paper: as printed, the sky beside the Sun is no brighter than the sky far
  from it. The term used is Schaefer's 1998 one (Sky & Telescope, May 1998).
  The default detection margin is the paper's threshold, 0. Each event
  carries a spread: how many days it moves with the threshold 0.2 magnitude
  easier or harder. Against swetest's `-hev` on DE440 (43 cases), our
  risings come early and our settings late in every case:
  - stars by 1 to 6 days, Sirius at Cairo -1 and +2, with spreads of 0 to 2
    days;
  - Mercury, Jupiter, Saturn and Venus's rising and setting by 1 to 5;
  - Venus's evening first and morning last by 7 and 10;
  - Mars by 11 and 21 (spreads 3 and 5);
  - the Moon by 0 and 1.
  The 139 CE Sirius rising at Alexandria comes out 1.1 days after Censorinus's
  date. The paper's Table 3 is reproduced to 0.9 %, and its 5.80 limiting
  magnitude at 220 nL to 0.006.
- **`prometheia_engine_delta_t(engine, jd_tt)`** gives an engine's own Delta T
  (its model, or the installed callback), so that a result given in TT can be
  turned into UT1 as the engine would (Python: `Context.delta_t`). The ABI is
  unchanged.
- **Phenomena and magnitudes** ([docs/PHENOMENA.md](docs/PHENOMENA.md)),
  A-4 of astromcp's port plan. For any object: the phase angle (at the body,
  at emission), the illuminated fraction, the elongation, the apparent
  diameter of the IAU mean radius, and V with its model named. The models:
  - the planets: Mallama & Hilton 2018, eqs. 2–17, Saturn's ring and
    Uranus's latitude terms from the IAU poles included;
  - the Moon: Krisciunas & Schaefer 1991, to 120° of phase, where the
    observations behind it end (Lane & Irvine 1973); none beyond;
  - the Sun: Willmer 2018;
  - catalog bodies: H, G;
  - stars: the catalog's V.

  Against `swetest` on DE440, V agrees to 0.0006 for Mercury, Venus, Mars
  and Jupiter, to 0.0016 for Neptune, and to 0.014 and 0.022 for Saturn and
  Uranus. The elongations agree to 0.0001″. The engine gains
  `Engine::phenomena(_ut)`, the C API `prometheia_calc_phenomena(_ut)` (ABI
  unchanged), and Python `Context.phenomena`. The radius table moves to
  `prometheia::phenomena`, and gains the Earth.
- **Lunar eclipses** ([docs/ECLIPSES.md](docs/ECLIPSES.md)), A-6 of
  astromcp's port plan. The next or previous total, partial or penumbral
  eclipse: contacts P1 U1 U2 U3 U4 P4, umbral and penumbral magnitudes,
  gamma, and van den Bergh saros series and member; Danjon's shadow radii,
  computed in TT, with UT1 alongside; the Moon's altitude at a site at each
  contact. Over 1900-2100 all 459 eclipses of NASA's canon are found, with
  class and saros identical, greatest eclipse within 2.73 s (in TD) and
  magnitudes within 0.0007. The C API gains `prometheia_lunar_eclipse_next`
  and `_at` (ABI unchanged), Python `Context.lunar_eclipse_next` and
  `lunar_eclipse_at`, and the engine `Engine::delta_t_seconds`.
  `tools/fetch/eclipse_fetch.py` fetches the canon for the comparison.
- **Solar eclipses** ([docs/SOLAR_ECLIPSES.md](docs/SOLAR_ECLIPSES.md)), A-7
  of astromcp's port plan: global circumstances, the shadow axis on the
  ground at an instant (centre line and umbral limits), and local
  circumstances with contacts C1-C4, magnitude and obscuration, from
  Besselian elements computed from the engine's Sun and Moon. Measured
  against NASA's canon over 1900-2099: all 452 eclipses found, kind,
  central flag and saros agree on all, greatest eclipse within 0.54 s, gamma
  within 0.00006, magnitude within 0.00008, Besselian x, y within 3.1e-5
  Earth radii; against swetest 2026-2027 (with its lunar radius) magnitudes
  to 0.0001 and local maxima to 0.4 s. The lunar radius is an option (NASA's
  by default). C API: `prometheia_solar_eclipse_next`, `_where`,
  `_local_next` (and `_ex` forms), purely additive (ABI unchanged). Python:
  `Context.solar_eclipse_next`, `solar_eclipse_where`,
  `solar_eclipse_local_next`. `tools/fetch/solar_eclipse_fetch.py` fetches
  NASA's tables for the env-gated tests.

- **The object registry** ([docs/REGISTRY.md](docs/REGISTRY.md)), A-9 of
  astromcp's port plan. One id scheme covers every object: `naif:`, `sb:`,
  `orbit:`, `hyp:` and `star:`. Numbered bodies keep their id across
  catalog releases, and old designations still resolve. Three calls:
  - **lookup** across every namespace, case, accent and punctuation
    folded, with ambiguity reported rather than resolved;
  - **info**: kind, SBDB orbit class, name and its standing, number,
    designations, H, V, mean motion and period, source, and how to
    compute the object;
  - **listing** by kind, class, name, number, H and V, a page at a time.

  The engine gains `calc_object(_ut)`, and `ObjectRef` gains orbit points.
  The C API gains `prometheia_object_lookup`, `_info`, `_list`,
  `prometheia_calc_object(_ut)` and the object kinds `ORBIT_POINT` and
  `ID` (ABI unchanged). Python gains `Context.object_lookup`,
  `object_info` and `object_list`, and `calc()` and `rise_set()` accept
  every object form. The measured costs are in REGISTRY.md: on the full
  catalog, the indexes take 0.75 s and 48 MB on first use.
- **The star catalog** marks which names are the IAU's (`stars::Object::iau_name`),
  and β Cnc answers to Altarf (Allen 1899's "Al Tarf").
- **The Sun's nodes and apsides** ([docs/ENGINE.md](docs/ENGINE.md), "Nodes
  and apsides"). `calc_orbit_point` now answers the Sun with its geocentric
  orbit, which is the Earth's reflected, where it used to refuse it: the
  mean and osculating methods, in the engine, the C API (ABI unchanged), the
  JSON tools and the binding. Geometric points are the Earth's
  heliocentric ones turned through 180°, exactly; apparent points are
  within 0.002″ of them. v4 serves them under methods 0 and 1, rows and
  segments, as §3.5a now says; methods 2-4 for the Sun are error 2.
- **Models named in source strings** (§3.4, [docs/SERVER.md](docs/SERVER.md)).
  `Provenance` gains `model` (C++ only; the C ABI is unchanged), set for
  mean orbit points (`mean elements: DE440 secular fit, 1550-2650`; the
  Moon's, `Moon: Simon et al. 1994`) and natural apsides. v4 writes such
  an object's source entry as `<engine> | <ephemeris> | <model>`. An
  ephemeris-only object's entry is unchanged, and so is a named
  hypothetical's, which already names its set. The JSON tools' provenance carries `model` where there is one
  ([docs/JSON_API.md](docs/JSON_API.md)).
- **The Raman zodiac** ([docs/ENGINE.md](docs/ENGINE.md), "Sidereal output"):
  Raman's own linear rule, (Y − 397) × 50⅓″ (*Hindu Predictive Astrology*,
  1938), reproduced to 0.001″. It is 19″ from `swetest`'s Raman at J2000,
  a difference of definition, recorded.
  - In the engine (`SiderealMode::Raman`), the C API
    (`PROMETHEIA_SIDEREAL_RAMAN` = 3, the A.11 number; ABI unchanged), the
    JSON tools and v4 (token `raman`), and the binding.
- **EPM1 1.2** ([docs/FORMAT.md](docs/FORMAT.md)): a record may carry SBDB's
  orbit class, one byte from a fixed table of the 22 classes, and a numbered
  body's old provisional designation in the string pool.
  - Readers still read 1.0 and 1.1, with both fields absent.
  - The converter, `prometheia-catalog-subset` and the refresh fold carry
    the new fields.
  - Lookup, and a subset list line, resolve the old designation ("2013
    FZ27" finds 523671; maintainer, 2026-10-27, reversing the deferral of
    2026-10-25).
  - `sbdb_fetch.py` asks SBDB for `full_name` from now on. The query API
    has no `prov_des` field. The converter takes a numbered asteroid's
    provisional designation from the final parentheses of `full_name`, so a
    catalog carries it from the next full pull. Each record costs under a
    byte more.
- **Rise, set and transit** ([docs/ENGINE.md](docs/ENGINE.md), "Rise, set and
  transit"):
  - of a body, star or hypothetical at a site, with the limb, refraction
    and dip as options;
  - transits are found first and the altitude extrema next to them, so no
    short day is stepped over (a Sun up for 1.8 s is found);
  - no event in the window is an outcome with a NaN time, not an error.
  - Against `swetest`'s printed times: transits and the unrefracted centre
    to 0.03 s, the refracted upper limb within 2.8 s.
  - In the engine, the C API (`prometheia_rise_set_ut`; ABI unchanged,
    announced to Astrolog first) and the binding.
- **The horizon and refraction** ([docs/FRAMES.md](docs/FRAMES.md), "The
  horizon"):
  - azimuth (from north through east) and altitude from a direction of date,
    and back;
  - Bennett 1982 refraction, scaled by pressure and temperature and applied
    down to the dip of the eye's height or −1°.
  - In the engine, the C API (`prometheia_horizontal_ut`,
    `_equatorial_from_horizontal_ut`, `prometheia_refraction`; ABI
    unchanged, announced to Astrolog first) and the Python binding.
  - Checked against ERFA: the geometry to 3e-11″, and refraction within 1.9″
    above 15°.
- **Python: no module-level client state.** `set_server`, `set_ephe_path`,
  `default_client` and `close` existed for `prometheia.swe` and are removed.
  `Client()` with no arguments follows `$PROMETHEIA_SERVER` and
  `$PROMETHEIA_EPHEMERIS`. The native names are re-exported at
  `prometheia.*`, and a type stub (`_native.pyi`, `py.typed`) ships.
- **Python: a native binding** ([docs/PYTHON.md](docs/PYTHON.md), "Native").
  `prometheia.Dataset`, a `Context` per thread, and `Options` in the JSON
  tools' words run the engine in process through the C interface, bit for
  bit. Every call releases the GIL. `prometheia/cprometheia.pxd` declares
  all of `prometheia.h` nogil and noexcept for Cython callers. The library is
  built position-independent (bench cost +0.15%, within noise). `tools/gate.sh`
  builds and tests the extension, and now runs the Python tests after the
  Release build rather than before it.
- **C API: houses, sidereal time, obliquity, rotations**
  ([docs/C_API.md](docs/C_API.md), "Houses"): `prometheia_calc_houses`,
  `_ut`, `prometheia_houses_from_armc`, `prometheia_sidereal_time_ut`,
  `prometheia_obliquity`, `prometheia_ecliptic_to_equatorial` and
  `_equatorial_to_ecliptic`, with the `prometheia_houses` struct. The ABI
  version is unchanged; the shape was announced to Astrolog first.
- **Houses whose cusps fall out of zodiacal order are refused**
  ([docs/HOUSES.md](docs/HOUSES.md), "Inside a polar circle"). Near the
  poles, Regiomontanus, Campanus and Topocentric gave cusps whose houses
  overlapped (the gaps summed to 720–3960°). They are now refused there with
  a message naming the system, and never reordered or replaced. The change
  reaches every surface: the engine, the C API, the JSON tool and v4 kind 6.
  Topocentric is affected from about 1.6° short of a polar circle.
- **Catalog answers no longer depend on request order**
  ([docs/ENGINE.md](docs/ENGINE.md), "Memo"). The perturber tables were
  anchored at the first instant an engine was asked for, so asking for a
  comet first moved Eros by up to 159 m. They now sit on one fixed grid.
  Every catalog answer moves once, by 0–6 m in 2026 and below 0.001″ for
  the asteroids measured 1600–2600. Comets and chaotic orbits move more,
  such as Apophis past 2029 ([docs/VALIDATION.md](docs/VALIDATION.md),
  "Small bodies").
- **Removed: `prometheia.swe`**, the pyswisseph-shaped Python module, with
  its tests and the pyswisseph oracle (`tools/check/pyswe_oracle.py`).
  `prometheia.Client` stays, on every transport ([docs/PYTHON.md](docs/PYTHON.md)).
- **C API: which years the loaded files answer**
  ([docs/C_API.md](docs/C_API.md)): `prometheia_engine_ephemeris_count` and
  `prometheia_engine_ephemeris_span`, asked for by Astrolog for their
  Ephemeris Settings dialog. The ABI version is unchanged, as no layout
  changed.
- **C API: which catalogs are loaded** ([docs/C_API.md](docs/C_API.md)):
  `prometheia_engine_catalog_count`, `_catalog_info` (record count and path,
  in load order) and `_catalog_bodies` (distinct bodies across them), asked
  for by Astrolog. The ABI version is unchanged.
- **`prometheia-catalog-subset --skip-missing`** names a list line that
  matches nothing and goes on.
- **Fixed: GCC 13 builds** (found by Astrolog on Ubuntu 24.04):
  - `src/frames.cpp` used `uint8_t` without `<cstdint>`, which GCC 13's
    libstdc++ no longer includes transitively.
  - Every standard header the library uses is now included directly.
  - Also fixed: a GCC 13 `-Werror` false positive and a redundant move.
  - All 14 sources compile with GCC 13.3 `-Wall -Wextra -Werror`, with
    identical answers.
- **Embedding without CMake** ([docs/EMBEDDING.md](docs/EMBEDDING.md)):
  - `embed/sources.txt` (contract 1) lists the library's sources, and
    CMake builds from the same list.
  - `embed/prometheia.mk` builds the archive inside a parent's GNU
    makefiles. The parent's `CXXFLAGS` never reach the library, and the
    parent's default goal is left alone.
  - `tools/check/embedtest.py` (scheduled) builds `ephem` that way. Its
    answers equal CMake's build on 94 CSV rows at `%.17g` with GCC 11.4 and
    Clang 14 (`-O2` against `-O3`).
  - The shipped hypothetical set is now a committed
    `src/hypotheticals_shipped.inc`, no longer generated by CMake.
- **A refresh since the base** ([docs/INGESTION.md](docs/INGESTION.md),
  "Freshness"; asked for by Astrolog):
  - `sbdb_fetch.py --refresh-since DATE --numbered-after N` makes one
    full-precision sweep: orbits re-solved since DATE, OR named, OR
    numbered after N. SBDB does filter on `soln_date`, which INGESTION.md
    had said it did not.
  - On 2026-10-25 it took two requests and 10 s, giving a 2.95 MB overlay
    of 30,840 bodies that brings Geyagugv's 2026-10-19 name.
  - `prometheia-catalog-subset` now cuts from a base plus overlays, the
    newest record winning. `--all` folds them into one up-to-date
    catalog: `sbdb-20260916-refresh-20261025.epm` (1,567,167 records)
    replaced `sbdb-full-20260916.epm` on this machine.
- **Fixed: one body no longer stops a covariance fetch**
  ([docs/INGESTION.md](docs/INGESTION.md), "Orbit covariance"; found by
  Astrolog):
  - `sbdb_fetch.py --covariance` died at the first `A/` object, whose
    covariance JPL publishes with no elements.
  - Such a covariance is now written as none, with the reason in the
    manifest. A refused designation is skipped and reported (exit 1).
- **Fixed: a catalog body near a file's edge** ([docs/ENGINE.md](docs/ENGINE.md),
  "Memo"; found by Astrolog):
  - A request within an integration window of the planetary file's first
    epoch was refused well inside the file (115 days after 1550 for Chiron).
  - The failure was sticky: every catalog body then failed at every later
    instant for the life of the engine, in `prometheiad` as in the library.
  - Windows and perturber blocks now stop at the files' span, and a failure
    is refused for that request alone. The answers far from the edges are
    unchanged to the bit.
- **A catalog of named bodies** ([docs/INGESTION.md](docs/INGESTION.md)):
  - `prometheia-catalog-subset` cuts an EPM1 catalog to a list of numbers,
    designations or names, and copies each record exactly.
  - It refuses any line that matches no body or several, and the same list
    gives the same bytes.
  - 42 named bodies from the full catalog came to 4,629 bytes, with answers
    identical to the full catalog's.
- **DE441 cut to a span** ([docs/DE.md](docs/DE.md), "Cutting a span"):
  - `prometheia-de-trim` copies whole records verbatim.
  - `de_fetch.py --only de441` makes two pinned cuts that end where DE440
    begins: 1000–1550 (51.1 MB) and −3000–1550 (423.0 MB).
  - Behind DE440, each answers bit for bit as the whole of DE441 does:
    19,500 and 160,000 apparent rows.
  - The DE reader now picks a record by exact comparison. Within an ulp of
    a record boundary, the choice no longer depends on where the file
    starts. DE440 is unchanged. In DE441, 52 of 6,276 boundary instants
    moved, by up to 3.4 m.
- **Portability toward MSVC and Apple clang** (none built yet;
  [docs/EMBEDDING.md](docs/EMBEDDING.md), "Platforms"):
  - no `M_PI`;
  - a portable two-node nutation kernel;
  - a `from_chars` fallback;
  - 64-bit catalog offsets on Windows;
  - fast-math and FP contraction refused at compile time.
  Both fallbacks are built on Linux and give the same 94 rows.
- **Houses in the engine** ([docs/HOUSES.md](docs/HOUSES.md)):
  - `Engine::houses` and `houses_ut` answer eleven systems: Placidus, Koch,
    Porphyry, Regiomontanus, Campanus, Equal, Whole Sign, Alcabitius,
    Morinus, Meridian and Topocentric.
  - Each returns the cusps with the Ascendant, MC, ARMC, Vertex and
    equatorial Ascendant, tropical or sidereal on the ecliptic of date.
  - Checked against `swetest` at its own ARMC: 18,214 values, worst 0.0056″
    (0.033″ for Koch within a degree of a polar circle).
  - The sidereal time is within 0.00075″ of ERFA's `gst06a`.
  - Placidus and Koch are refused inside a polar circle, never replaced.
  - Served by the JSON/MCP `houses` tool (per-system errors, including
    `undefined-at-latitude`), with `house_systems` in `capabilities`.
  - Not yet in the C ABI or the protocol.
- **Protocol v4 houses:** Astrolog's drop `6235abd` is vendored.
  - What it adds: object kind 6, the ARMC and obliquity columns, error 9 and
    WELCOME 0x0015/0x0016.
  - 124/124 fixtures agree through both readers.
  - `prometheiad` serves kind 6 for all eleven systems, with stencil rates
    and per-row polar refusals.
  - Astrolog's 2,244 reference rows (35,616 cells) agree with our house
    code to 0.000002″ ([docs/SERVER.md](docs/SERVER.md), "House points").
- **A Python package, `python/prometheia`** ([docs/PYTHON.md](docs/PYTHON.md)):
  - Pure Python with no C extension. It talks to a local `prometheia-json`
    over stdio or to one over HTTP.
  - `prometheia.swe` is shaped like pyswisseph (`calc_ut`, `houses`,
    `set_sid_mode`, `utc_to_jd` and more), and `prometheia.Client` exposes
    the JSON tools as they are.
  - Against pyswisseph on the same DE440 file: the Sun, Moon and planets
    agree to 0.0027″ at TT over 1800–2100.
  - Our reported rates match our positions' derivative to 0.008″/day;
    pyswisseph's depart from its own by up to 0.11″/day, and 9.9″/day
    topocentric.
  - It speaks protocol v4 to `prometheiad` directly (`ws://`, `wss://`),
    with its own WebSocket client. Its answers are identical to stdio's on
    the same DE440.

## 0.7.3 — 2026-09-29

The library and tools are 0.7.3, and the C ABI is unchanged (6).
`prometheiad` is 0.11.0, because its answers change in two ways: TDB−TT is
the full Fairhead & Bretagnon series, and a catalog body more than 100 years
from its element epoch now needs an asteroid perturber kernel.

**What moves for anyone pinning values:**
- date and position answers, by ≤ 5 µas within 1600–2200 and up to ~0.15
  mas far from J2000;
- such catalog bodies, which are now refused when no kernel is loaded;
- the dataset id's digest (`…#c029261c` for DE440 alone);
- the DE440-span perturber cut's hash (e7b67e32…; same records as the old
  a31b839a…).

The USNO ΔT and leap-second tables were not re-fetched, since 0.7.1
refreshed them the same day (observed through 2026-04).

- **Without an asteroid perturber kernel, a catalog body is served within 100
  years of its element epoch** and refused beyond, with a coverage error
  naming how to get the kernel (maintainer). The error there is 8.7″;
  without the kernel it reaches 75″ by 400 years and arcminutes beyond.
  `tools/fetch/de_fetch.py --only sb441` fetches JPL's SB441-N16 and makes
  the DE440-span cut, both checked against pinned SHA-256s. It replaces the
  documented `curl` line. `prometheia-spk-trim` no longer writes its version
  into the cut, so the cut's hash holds across releases (e7b67e32…; the old
  a31b839a… is the same records).
- **Small bodies far from their element epoch, measured and stated**
  (docs/VALIDATION.md, "Centuries and millennia"). With JPL's perturbers,
  Ceres, Pallas and Vesta stay within 0.9″ of Horizons over 1600–2500, the
  span Horizons itself serves; without them, 75″. The JSON accuracy
  statement now says so. An instant outside the perturber kernel is
  refused with a message naming the kernel's span, where it named whichever
  of its bodies ran out.
- **TDB−TT is the full Fairhead & Bretagnon series** (787 terms, from ERFA's
  BSD-3 table; docs/TIME.md, "TDB"), where it was the seven-term Circular 179
  series. The full series follows the decline of Earth's eccentricity:
  0.2 ms apart at year 9000, ≤ 10 µs within 1600–2200. The engine
  interpolates it from one-day nodes to within 1.55 µs, at the old speed.
  **Moves answers** by ≤ 5 µas within 1600–2200 and up to ~0.15 mas far
  from J2000. Against Horizons, the DE441-era planets now agree to 3.4 µas
  on Horizons' own time argument.

## 0.7.2 — 2026-09-29

The library and tools are 0.7.2. The C ABI stays 6: one function is appended,
`prometheia_engine_add_ephemeris`, and nothing moved; it was announced to the
Astrolog side before it landed. `prometheiad` is 0.10.0, because its answers
change: the default precession, and a WELCOME that states its coverage.
**What moves for anyone pinning values:**
- every date-frame answer that took the old IAU 2006 default, by up to
  0.01″ (≤ 2 mas over 1800–2200);
- the dataset id's digest, which carries the engine version (`…#395d0135`
  for DE440 alone);
- JSON rows, which gain three `time` fields.

The USNO ΔT and leap-second tables were not re-fetched: 0.7.1 refreshed
them the same day (observed through 2026-04).

- **Validated beyond DE440's span.** Against JPL Horizons, which also
  answers from DE441, 10 bodies at −3000 to 9000 agree to ≤ 8.3 µas (the
  Moon to 124 µas at 9000, a 0.2 ms time-argument offset) (docs/DE.md).
  The JSON accuracy statement now names the spans it was measured over.
- **Protocol v4 re-pinned** to Astrolog's coverage drop: `ephproto.h`
  `91c2fa6a…` with `Capabilities::coverage`, registries `5fed5d64…`, and
  110/110 conformance fixtures through both readers.
- **Every JSON row says what its ΔT rests on** (docs/JSON_API.md, "Time"):
  `time` gains `delta_t_s` and `delta_t_basis` (observed, reconstructed,
  before the eclipse record, or predicted). With DE441 a clock time reaches
  millennia back, where ΔT, not the ephemeris, bounds the answer.
- **`capabilities` says which dates it serves** (`dates`: each ephemeris
  file's span, in answering order) and `precession_default`; the C++
  `Engine::ephemeris_spans()` is what it reads.
- **UTC after the last leap second is TAI − 37 s, said out loud**
  (maintainer): no further leap seconds assumed, as CGPM 2022 implies; UTC
  rows carry `ut1_minus_utc_s` (−134 s by 2100) (docs/TIME.md).
- **`prometheiad` states its coverage** (A.3 0x000A, agreed with the
  Astrolog side): each ephemeris file, in consulting order, with its TDB
  span. It is for routing only. `prometheia-wire-client` prints any it receives.
- **DE441 behind DE440** (docs/DE.md, "DE441"). `Engine::add_ephemeris`
  (C `prometheia_engine_add_ephemeris`; `--ephemeris` twice to the servers,
  `-e` twice to `ephem`) answers the instants the first file does not
  cover: DE440 inside 1550–2650, to the bit as before, DE441 over
  −13000..17000 outside it. DE441 passes all 360,001 of JPL's test points
  in its span; it agrees with DE440 on the planets to 0.013 mas and on the
  Moon to 0.36″ at 1550. Fetched and pinned by the new
  `tools/fetch/de_fetch.py`. ABI unchanged (6); announced to the Astrolog
  side before it landed.
- **Precession defaults to Vondrák 2011 everywhere** (maintainer, with the
  Astrolog side; protocol §3.5a for a REQUEST without the TLV). IAU 2006's
  polynomials part from it by degrees at ±10,000 years, and within
  1550–2650 the two agree to ≤ 0.01″, so one model serves every date with
  no seam (docs/FRAMES.md). **Moves every answer that took the old default
  by up to 0.01″** (≤ 2 mas over 1800–2200). IAU 2006 stays selectable, and
  an explicit `iau2006` TLV now selects it rather than the default (a fault
  the change would otherwise have introduced; a test holds it).
- **The planets' mean orbit points are refused outside 1550–2650**, the
  span their quadratics were fitted over, rather than extrapolated.

## 0.7.1 — 2026-09-29

The library and tools are 0.7.1; the C ABI is unchanged (6: one status value
appended, no layout moved), announced to the Astrolog side before it landed.
`prometheiad` is 0.9.0, because two of its answers change: a fixed star's
distance rate, and a site at or below the Earth's centre, now ERROR 1. **Two
things move for anyone pinning values:** the star distance-rate column (by up
to 2.3e-5 AU/day on Polaris), and the dataset id's digest, which carries the
engine version (`…#2cc1eee3` for DE440 alone). The USNO ΔT and leap-second
tables were refreshed for the release and are unchanged (observed through
2026-04).

- **A fixed star's distance rate is computed in closed form**
  (docs/ENGINE.md, "Rates"). It was a central difference of positions
  1e5–1e10 AU long, which missed by one f64 ulp of the position: 2.3e-5
  AU/day on Polaris, 1e-7 on the nearer stars. It now matches differenced
  distances to the difference's own floor. **Moves a value on the wire:**
  the star distance-rate column, and the radial part of a star's velocity
  vector. Positions and angular rates are unchanged. The C ABI is unchanged.
- **A coverage error has its own status**, `PROMETHEIA_ERROR_COVERAGE`
  (7) in the C API and `ErrorCode::CoverageError` in C++ (docs/C_API.md).
  An instant outside the ephemeris, the perturber kernel or the natural
  apsides' passages returned `PROMETHEIA_ERROR_ARGUMENT` (4); callers that
  matched the message text still can, since it is unchanged. The C ABI stays
  6 (no layout changed). Announced to the Astrolog side, whose plugin maps 7
  from their `aa83946`, before it landed. `prometheiad` and
  `prometheia-json` now classify coverage by the error's type, not its words.
- **The rate bound's distance figure is read per max(1 AU, r)** (protocol
  §3.5a and A.3 tag 19, Astrolog `5f11726`; `registries.json` re-vendored,
  two payload strings). `tools/check/ratesweep.py` grades it that way. Under
  it `prometheiad` meets the A.3 default everywhere measured (3.6e-6 °/day,
  1.7e-10 per AU), so it still sends no `0x0013`, now truthfully
  (docs/CROSS-TEST.md).
- **Protocol v4 re-pinned to Astrolog `7cbf0d8`, the site drop**
  (third_party/README.md). `prometheiad` now answers a REQUEST whose site
  height is at or below the WGS-84 polar radius with ERROR 1 (malformed),
  where it failed that object's rows. Conformance set 109/109 on both
  readers. The drop was eleven days old: the drift check only runs with
  `$PROMETHEIA_ASTROLOG` set, and nothing set it; `tools/scheduled.sh` now
  does.

## 0.7.0 — 2026-09-19

The library and tools are 0.7.0; the C ABI is unchanged (6), and no
interface in [docs/C_API.md](docs/C_API.md) changed, so nothing was owed to
consumers before this landed. `prometheiad` is 0.8.0, because it answers a
request it used to refuse. **Two things move for anyone pinning values:**
the Moon's natural apsides by up to 0.03 mas, and the dataset id's digest
once (`…#70cddb41` → `…#7ddcde20` for DE440 alone), because the id is now
computed in parallel pieces. Everything else in the performance work is
byte for byte what 0.6.0 answered.

- **A planet's own orbit points, seen from its centre, are answered.**
  `prometheiad` refused Mars's node and perihelion from Mars's centre as "the
  observer is the object". A point is a place in space, so only the body
  itself is refused now (docs/ORBIT-POINTS.md, "From any observer";
  docs/SERVER.md, per-object errors).
- **Performance** (docs/ENGINE.md, "Performance"; the new
  `prometheia-engine-bench`). Same outputs, byte for byte:
  - a small body no longer decodes its catalogue record on every call:
    2,712 → 14 µs;
  - walking backward in time is no longer quadratic (the trajectory memo
    and the perturbers' table);
  - CRC-32 by slicing-by-8: loading the full catalogue 634 → 315 ms;
  - the dataset id is digested in 8 MiB pieces on every core, and SHA-256
    takes whole blocks: `prometheia-json` starts in 0.10 s with DE440
    (was 0.66 s), 0.57 s with the full catalogue (was 2.19 s). The id's
    digest changes once as a result.
  - A JSON call for three distant small bodies at 1,000 instants: 19.2 →
    1.8 s, the same answer.
  - the Moon's natural apsides scan their passages in fixed 50-day blocks,
    kept per engine: 266 → 33 µs a call at scattered instants, and a
    passage no longer depends on which instant was asked first (answers
    moved by up to 0.03 mas);
  - the nutation series with rates: 22.6 → 18.4 µs a node, and a fresh
    instant's two nodes are computed together, a lane each of a two-double
    vector, bit-identical to the scalar path: 36–42 → 19.8 µs (a walk
    through time gets its next node with each one for free). The library
    is built with `-ffp-contract=off`, so no result depends on whether a
    compiler fused a multiply and an add;
  - `prometheiad` under `prometheia-load` (64 connections, fresh instants):
    31,900 → 49,300 requests a second, p99 4.63 → 2.05 ms;
  - a small body's first integration is 2.3× faster: the perturbers' table
    reads each ephemeris record once, not once per body;
  - the force model interpolates a velocity only for the Sun, the one it
    uses: small-body integration 1.4× faster, the same values;
  - a DE (or SPK) state computes its Chebyshev recurrences once for x, y
    and z: 81 → 55 ns, bit-identical;
  - `add_catalog` verifies chunks without decoding records (315 → 235 ms),
    and the name index reads only SPK-IDs and names (new
    `catalog::Reader::verify` and `for_each_name`).
- **`prometheia-json` reads a clock time before 1972 as UT1** (maintainer,
  2026-09-19). It refused any date before 1972, so an agent could not ask
  for most birth charts by clock time. UTC with integer leap seconds begins
  in 1972, and before it civil time kept UT. The reply names the scale
  (`"ut1"` in place of `"utc"`) (docs/JSON_API.md, "Time").
- **`prometheia-json` hardened against hostile input**, found by a new
  fuzzer, `fuzz_json` (docs/SERVER.md, "Fuzzing"; docs/JSON_API.md,
  "Guardrails"). Deeply nested JSON crashed the process (a stack overflow at
  500,000 levels, which fits in a 1 MiB body); it is now a parse error past
  64. An object or array `id` was echoed; it is now answered with null. A
  site 4e19 m up cost 30 s of CPU per small body; `height_m` is now
  −12,000..100,000 m. The HTTP log wrote the client's method and tool
  strings; it now writes only names the server knows. Replies that could
  not serialise now go out with U+FFFD.
- **The cross-test sees orbit points from elsewhere.** The `points` leg asks
  every point from the Sun, the barycentre and Mars's centre, at masks 0
  and 1. The Moon's points are also checked against the Earth answered
  beside them, so a difference is attributed. The `rates` leg adds Mars's
  mean perihelion (docs/CROSS-TEST.md, record s).

## 0.6.0 — 2026-09-18

The library and tools are 0.6.0; the C ABI is unchanged (6), with one new
accepted selector, `PROMETHEIA_ELEMENTS_INTERPOLATED`. `prometheiad` is 0.7.0:
it serves orbit method 2, and its rates and the Moon's points changed.

- **Rates, checked by a new oracle.** The cross-test's `rates` leg checks
  each server's rates against a five-point difference of its own positions.
  Every other leg compares positions only. It found two defects of ours.
  - The Moon's nodes and apsides jittered by 1.2 mas topocentrically. Their
    retarded epoch was rounded to a JD double without the error recovery
    bodies have. Now 8 µas (ORBIT-POINTS.md).
  - The rate step was 0.001 day, and a topocentric Moon's diurnal parallax
    made its truncation 2.6e-5 °/day. At 1/4096 day it is 2.9e-6, with
    geocentric rates 3.4e-8 °/day or better (ENGINE.md, "Rates"). No
    extra cost.

- α Cen's mass split is now Akeson et al. 2021's (mass fraction 0.54266),
  from the paper whose orbit we use. It was Pourbaix & Boffin 2016's 1.13
  and 0.97. α Cen A moves by up to 185 mas at 1900. The Astrolog session
  caught the mismatch.

- **The Moon's natural apogee and perigee** (A.14 method 2, "interpolated";
  ORBIT-POINTS.md). They follow the published definition, an interpolation
  between the Moon's actual passages. They are exact at every passage, and
  shaped between passages by a deviation model fitted to all 29,149 passages
  in DE440. Served by the engine (`OrbitElements::Interpolated`), the C API
  (`PROMETHEIA_ELEMENTS_INTERPOLATED`; ABI unchanged), prometheiad (method 2)
  and the JSON tools ("natural apogee", "Priapus"). Against Swiss's analytic
  curve: apogee within 0.10°, perigee within 2.8°.

## 0.5.0 — 2026-09-18

The library and tools are 0.5.0; the C ABI is unchanged (6). `prometheiad` is
0.6.0: its binary stars and the Moon's apsis names changed. New program:
`prometheia-json`.

- **JSON and MCP for AI agents** (docs/JSON_API.md): `prometheia-json`
  serves four tools (`positions`, `lookup`, `capabilities`,
  `convert_time`) over MCP, by stdio or streamable HTTP, and as plain JSON
  over HTTP. It is a convenience surface in its own process. The C++
  library, the C API and `prometheiad`'s binary protocol stay the fast
  paths.
  - Objects go by name, including astrology's ("true node", "Lilith"), and
    times in ISO 8601 UTC.
  - Each result names what it resolved to, and carries its source, the
    corrections applied, the frame and a measured accuracy statement.
  - Errors are typed.
  - nlohmann/json (MIT) is vendored for these alone.
- The Moon's apsides are named perigee and apogee in answers, rather than
  perihelion and aphelion.
- The JSON words follow the v4 registries, as amended by the Astrolog
  session.
  - A body-centred observer (`"observer": "body", "center": "Jupiter"`).
  - `gravitational-deflection` for the correction (`deflection` is still
    read).
  - All five A.14 orbit methods are named. The three not computed are
    refused as `unsupported`.
  - `capabilities` lists each zodiac's sidereal planes. `anchor` on a
    zodiac defined at the instant is refused before anything is computed.

- Binary stars' orbits are laid in the sky's plane at the date, not at the
  catalog position. The catalog position's plane turned α Cen's orbit by
  0.065° by 2025, about 10 mas. The test now checks position angle as well
  as separation, against the Sixth Catalog's ephemeris; the separation
  check could not see the turn. The Astrolog session found the convention
  and asked that it be checked.

## 0.4.0 — 2026-09-18

The same day as 0.3.0.
- **New:** eleven zodiacs defined at the instant; binary stars on their
  orbits.
- **Faster:** sidereal positions four times faster.
- **Hardening:**
  - a fuzzer and a load tool;
  - two fuzz findings fixed, one in each project's code;
  - the protocol re-pinned to the Astrolog side's floats drop.

Versions:
- the library and tools are 0.4.0, and the C ABI is still 6 (the new
  sidereal modes are appended values);
- `prometheiad` is 0.5.0.

The cross-test's current record is `docs/crosstest/2026-09-18n.tsv`. Its
only findings are the Astrolog side's IAU 1958 pole transfer, which is with
their maintainer.

- **Binary stars move on their orbits** (docs/STARS.md, "Binary stars"):
  Sirius A, Procyon A, and α Cen A and B.
  - Orbits are from the Sixth Catalog of Orbits of Visual Binary Stars, and
    masses from the orbits' own papers. How each orbit is added follows what
    the star's Hipparcos line is.
  - α Cen A no longer drifts: its barycentre is 0.73″ from the FK5 at 1900,
    where the straight line was 28.6″ off.
  - α Cen B − A is the orbit's separation (14.11″ at J2000, against 14.13″),
    and matches the catalog's published ephemeris to 2 mas.
  - Sirius A and Procyon A now show their wobble about their barycentres (up
    to 3.9″ and 1.7″).
- **Fixed: a "successful" answer with NaN coordinates** (found by fuzzing
  after the codec re-pin, docs/SERVER.md "Fuzzing").
  - A topocentric site far below the ground gave rowsOk 1 with −NaN
    coordinates. The engine now refuses such a site, and never reports a
    non-finite answer as a success.
  - `prometheiad` fails a row whole, as the canonical NaN, if any value in
    it is not finite (§3.1).
- **Protocol v4 re-pinned to Astrolog `114f2a5`** (the floats drop): DATA
  values finite, or a whole failed row of the canonical NaN.
- **Sidereal positions four times faster:** 85.6 → 20.3 µs for the Sun
  with Lahiri, the same as tropical, measured over 100,000 instants on
  DE440.
  - The anchored zodiacs summed the full IAU 2000A nutation series on every
    call, three times per position with rates.
  - They now take Δψ from the engine's epoch cache, like the zodiacs defined
    at the instant, which cost 1–2 µs over tropical. The cached value is
    interpolated, 0.004 µas from the series.
- **Zodiacs defined at the instant** (docs/FRAMES.md): eleven sidereal
  modes whose zero point is where a star, the Galactic Centre or the
  galactic node sits at the moment asked. The modes are True Chitra,
  Revati, Pushya and Mula; four Galactic-Centre modes (0° Sag, Cochrane,
  Gil Brand, Wilhelm); and three galactic-equator modes.
  - The definitions are the published ones, with the anchor at its true
    position on the true ecliptic of date.
  - Checked against ERFA by independent routes to 0.06 mas, and against
    two statements in the published documentation.
  - They have no anchor epoch, so the ecliptic of the anchor epoch is
    refused for them. On the invariable plane their zero point is the
    instant's.
  - In the C API as `PROMETHEIA_SIDEREAL_TRUE_CITRA` … `_GALEQU_MULA`,
    numbered as the protocol's A.11 tokens. ABI 6 is unchanged, since
    enumerated values are only appended. The Astrolog side reviewed them
    before they landed.
  - `prometheiad` advertises the eleven tokens.
  - `ephem --sidereal` takes the same tokens.
- **Fuzzing** (docs/SERVER.md, "Fuzzing"): `tools/fuzz.sh` runs libFuzzer
  targets over the protocol codec and over `prometheiad`'s session, under
  ASan and UBSan. The check is the protocol's own: every reply must parse
  and re-encode byte for byte.
  - The first 10-minute runs gave 2.36 million session inputs with no
    failure.
  - One codec finding, reported upstream: DATA accepts non-finite values.
  - The tree now also builds with clang: `src/hypotheticals.cpp`'s JSON
    value, and an unused session field.
- **Load and soak** (docs/SERVER.md, "Load and soak"): `prometheia-load`
  runs N connections of chart requests for a while and samples the server's
  memory and open files. 64 connections for 5 minutes: about 35,600
  requests a second, p99 2.94 ms, memory flat at 368 MB, no leak. The
  per-address connection cap and compute budget hold at their defaults.
- **The `sidsweep` leg** grades an explicit refusal of a zodiac on a fixed
  plane as `refused`. Only a silent plane-0 answer is a finding.

## 0.3.0 — 2026-09-18

The protocol's §3.5a amendment and C ABI 6, with two new outside checks.

Versions:
- the library and tools are 0.3.0, and the C ABI is 6;
- `prometheiad` is 0.4.0: its answers for a node in a fixed frame changed.

The cross-test's current record is `docs/crosstest/2026-09-18g.tsv`
(docs/CROSS-TEST.md).

- **Fixed stars checked against the FK5** (Fricke et al. 1988), a
  ground-based catalogue that predates Hipparcos, so an error both servers'
  Hipparcos-derived catalogues share would show.
  `tools/check/stars_fk5.py` (docs/STARS.md, "Checked against FK5").
  - Both servers: 24 ordinary stars within 0.56″ over 1900–2100, and within
    0.007″ of each other.
  - Four astrometric binaries up to 2.3″ (Sirius), and α Cen A to 28.6″ at
    1900: straight-line motion from the Hipparcos epoch, now a documented
    limit.
- **Every zodiac token on every sidereal plane** (the cross-test's
  `sidsweep` leg), graded on whether a plane request moves the answer. A
  plane accepted and ignored is a finding. `prometheiad` passes all 288
  rows.
- **The invariable plane's zero point** (§3.5a Part A, approved by both
  maintainers): the zodiac's zero-point direction projected onto the plane,
  which is what Prometheia already did. No change here.
- **A node lies on the mean ecliptic of date in every frame** (protocol v4
  §3.5a, amended 2026-09-18, approved by both maintainers). A node asked in
  J2000 or ICRF is now the node of date, rotated; before, it lay on the
  J2000 ecliptic, a different point up to 680″ away on the Moon's mean node
  at 1800 and 2100. The node of date is where eclipses fall, which is what
  an astrologer means by the node. The frame of date is unchanged.

- **C ABI version 6:** `prometheia_options.sidereal_plane` and the
  `PROMETHEIA_SIDEREAL_PLANE_*` constants, which take the protocol's A.8
  values. The field is appended, so bindings must rebuild: the ABI check is
  equality. The Astrolog side reviewed it before it landed; it lets their
  plugin serve Astrolog's solar-system-plane charts. `ephem --sid-plane`.
- **Declined: zstd wire payloads.** Measured, real f64 DATA compresses to
  96.8% at best (docs/SERVER.md, "Not implemented").

## 0.2.0 — 2026-09-18

The same day as 0.1.0, in 56 commits:
- the Hamburg School's Uranian points and other hypothetical bodies;
- a client-server cross-test against the Astrolog project's server,
  refereed by JPL Horizons and by textbook formulas, down to an
  application-level test that casts real Astrolog charts through
  `prometheiad`;
- the sidereal planes;
- per-kind correction masks in protocol v4;
- logging and traceability end to end.

Versions:
- the library and tools are 0.2.0, and the C ABI is still 5;
- `prometheiad` is 0.3.0, speaking protocol v4 with the per-kind
  correction mask drop (ephv4 `eed6429`).

The cross-test's current record is
`docs/crosstest/2026-09-18f.tsv` (docs/CROSS-TEST.md).

### Client-server cross-testing

- **`tools/check/crosstest.py`** runs the reference client against two v4
  servers, compares their answers as angular separations, and uses JPL
  Horizons as referee wherever the corpus has the point. It writes a dated
  leg table ([docs/CROSS-TEST.md](docs/CROSS-TEST.md)). The first run
  (`docs/crosstest/2026-09-18.tsv`) found three defects in the Astrolog
  server, each bisected to one correction term. It also found one in its own
  adjudication, fixed before the table was written.
- The cross-test gained a topocentric anchor, a barycentric anchor and
  refusal checks, and sends only masks both servers list. Deliberate
  differences are recorded as expected-difference with their reason.
- `prometheiad` logs one line per connection, HELLO, request, ERROR and
  close, each with a connection id and the request id, at `--log-level
  quiet|info|debug` (default `info`). It never logs instants, sites, names
  or tokens (SERVER.md, "Logging").
- Cross-test leg `stars`: 29 named stars, tropical and sidereal, and the
  two IAU names of α Centauri.
- Cross-test legs `points` (orbit points by direction and distance, and
  the node-on-its-frame's-ecliptic rule) and `sidereal` (the three planes
  for two zodiacs).
- All three protocol v4 sidereal planes: the ecliptic of date, the
  ecliptic of the anchor epoch, and the solar system's invariable plane
  (derived from DE440's angular momentum). Available as
  `CalcOptions::sidereal_plane` in the engine (not yet in the C ABI) and
  through `prometheiad`; `prometheia-wire-client --sid-plane`. Segments
  serve the ecliptic of date only.
- `prometheia-wire-client --node` asked for the ascending node whatever
  point was named, and read the point letter as the method. It is now
  `NAIF.P[.M]`: point `a|d|p|A`, method `m`, `o` or 0–4, and a bad point is
  refused.
- Traceability: `prometheia-wire-client` picks a fresh request id per run
  (`# request N`, the daemon's `req=N`), and the cross-test TSV records the
  ids behind every row. The fetch tools log each GET with a UTC timestamp,
  URL and outcome. `ephem` names the ephemeris file and its version in its
  header and JSON.
- Protocol v4 per-kind correction masks (WELCOME tag 0x0014, ephv4
  `eed6429`, set `472b21a`): vendored and verdicted 99/99, with the
  handshake-dependent judgements 7/7. `prometheia-wire-client` prints
  `# corrkind` lines, and the fixture reader gained `--judge` for request
  fixtures whose refusal depends on a WELCOME. `prometheiad`'s WELCOME
  does not change.
- The cross-test judges the Swiss refit by position (km), not a fixed
  angle. It also referees deflection from a planet centre against the
  textbook formula, where Horizons has no observer.
- `frames::ut1_from_sidereal_time` recovers UT1 from a published local
  apparent sidereal time. `prometheia-ut1` exposes it on the command line
  for the harness.
- `prometheia-wire-client` gained `--deltat`, `--hyp`, `--elements`,
  `--center`, `--corrections` and a capabilities line. `tools/check/wirelib.py`
  is the one reader of its output.
- SERVER.md writes down the per-object error contract.

### Hypothetical bodies

- **Bodies from polynomial orbital elements**, `Engine::calc_elements`, the
  protocol's kind 4: pure two-body motion about the Sun or the Earth, with
  elements in any of five equinoxes, and the corrections applied as for a
  body. The mean anomaly and mean motion follow the protocol owner's
  normative rule, which was corrected before release: M's own nonzero
  coefficients decide its meaning, and n comes from the Gaussian constant,
  so an answer does not depend on the loaded ephemeris.
  [docs/HYPOTHETICALS.md](docs/HYPOTHETICALS.md).
- **Named hypothetical bodies**, `Engine::calc_hypothetical`, the protocol's
  kind 3, defined in JSON Lines element files: one strictly checked body per
  line, in Prometheia's own format. A shipped set is compiled in, and
  `add_hypotheticals()` adds an operator's file, whose definitions win.
- Orbit points and bodies from elements share one correction path; the
  orbit-point results are unchanged.
- **`prometheiad` serves both kinds**, as DATA and as fitted segments. The
  new `--hypotheticals FILE` option adds element files. WELCOME advertises
  kind 4 always, kind 3 with its tokens (A.3 0x0011) whenever any are
  defined, and all five element equinoxes (0x0012). The dataset id digests
  the shipped set and every element file. The fitted-cell key carries the
  elements, so two element sets never share a cell; a test that removes
  them from the key goes red.
- **C ABI version 5**, purely additive: `prometheia_calc_elements` and
  `prometheia_calc_hypothetical` (each with a `_ut` form),
  `prometheia_engine_add_hypotheticals`, token enumeration and
  `prometheia_hypothetical_get`. The signatures were reviewed by the Astrolog
  side before landing, per the ABI policy above. The structs are frozen for
  the life of an ABI version; only the TT forms are interoperable (the UT1
  forms bring in the engine's Delta T).
- The body an orbit is about is called its **origin** in Prometheia's own
  names (C, C++ and element files), because `PROMETHEIA_ELEMENTS_CENTRE_*`
  beside `PROMETHEIA_CENTER_*` was a misreading waiting to happen. Only the
  wire keeps the protocol's "centre".
- **`ephem`**: `hyp:TOKEN` and `hyp:all` as bodies, `--hypotheticals FILE`,
  and `$PROMETHEIA_HYPOTHETICALS`.
- The claim that orbit points are geometric, stale since corrections began
  applying to them, turned up a fourth time (SERVER.md's list of v4
  semantics) and a fifth (prometheia.h). Both are corrected, found this time
  by searching for the phrase rather than by a reader.
- **Error text never quotes the request** (protocol §3.8). Adding a message
  that would have echoed an unknown token exposed a whole class: unknown star
  names, designations, NAIF ids and zodiac tokens all came back verbatim in
  per-object or ERROR text. Per-object text is now a fixed sentence per
  error code, classified from the full message, and a test pins it for every
  object kind.
- **Correction masks follow the protocol.** WELCOME now lists every exact
  mask honoured per observer (all eight; at the Sun's centre only those
  without deflection), and anything else is ERROR 11. Before, it listed one
  mask per observer while answering every combination, so a conforming
  client would never have asked for a geometric or astrometric position.
- **`corrapplied.py` no longer passes vacuously.** It asks only the masks a
  server's WELCOME honours, reports what it cannot ask as inapplicable, and
  fails when nothing, or under half, was checked, or when a check never ran.
  Against the Astrolog server it had printed OK after checking zero cases.
- **Fixes from a review of this session's own work:** a client's name can no
  longer steer the per-object error code (NotFound is classified by its code
  before any message text is read); an element file's `"equinox": 0` is
  refused at load; `ephem` keeps a 64-character token whole; and the wire
  client's `--helio` asks for the mask the Sun's centre honours, and its
  `--jd` defaults to J2000.0 as its help text always said, not JD 0.
- The kind-4 rule is released on the protocol side as "the kind-4 elements
  rule" (Astrolog `176e333`).
- **Shipped element set:** the eight Hamburg points, and Le Verrier's
  predicted Neptune from his 1846 paper. The Hamburg elements are those
  Astrolog distributes, a recorded exception ([docs/DESIGN.md](docs/DESIGN.md)),
  and match `swetest` to 0.00076″. Le Verrier's are held to the position he
  printed himself.
- 0.1.0's notes called kinds 3 and 4 a settled end state. That reflected
  one client's needs and has been reversed; see
  [docs/SERVER.md](docs/SERVER.md).

## 0.1.0 — 2026-09-18

First release. A cleanroom successor to the Swiss Ephemeris, written without
reference to its source, under GPL-2.0-or-later. The work it covers landed
over 2026-09-16 and 2026-09-17, in 97 commits.

### Ephemeris and mechanics

- **JPL DE readers** for the binary format, self-describing across DE200
  through DE440+, either byte order, including the nutation, libration and
  TT−TDB columns. **NAIF SPK readers** (`.bsp`, type 2 and 3 segments, with
  segment chaining). [docs/DE.md](docs/DE.md), [docs/SPK.md](docs/SPK.md).
- **Mechanics core**: a Kepler engine (elliptic and hyperbolic, closed-form
  propagation used as the test oracle), adaptive DP5(4), and an IAS15-class
  Radau-15 collocation integrator whose constants are derived from the Jacobi
  recurrence rather than transcribed. Cubic-Hermite perturber trajectory
  tables, heliocentric N-body forces, a windowed memo cache.
- **EPM1 catalog container**: indexed, zstd-chunked, CRC-checked, no time axis.
  87 bytes per body for the real 1.57M-body catalog — 136.7 MB against the
  48 GB the equivalent `.se1` files occupy. [docs/FORMAT.md](docs/FORMAT.md).
- **Ingest pipeline** for the JPL SBDB Query API: resumable, strictly
  sequential, rate-limited, with freshness machinery (identity sweeps,
  `orbit_id` delta overlays, hot-subset sweeps).
  [docs/INGESTION.md](docs/INGESTION.md).

### Time and frames

- Exact proleptic-Gregorian calendar ↔ JD. UTC ↔ TAI ↔ TT with the USNO
  leap-second table, including correct `23:59:60` labelling in both
  directions. TT ↔ TDB via the published truncated series (~10 µs class).
- ΔT (TT − UT1) is pluggable and defaults to observed USNO values (1657 to
  the latest published month), with the Stephenson–Morrison–Hohenkerk eclipse
  reconstruction before 1657 and a tidal trend after. Continuous everywhere.
  **A ΔT model takes no state from the ephemeris** and is a pure function of
  the instant — deliberately, and [docs/TIME.md](docs/TIME.md) records why.
- IAU 2006 precession with the complete IAU 2000A nutation series (1365 terms,
  parsed from the public-domain USNO Circular 179), ICRF → equatorial and
  ecliptic of date, ERA/GMST/GAST, a WGS84 topocentric helper.
  [docs/FRAMES.md](docs/FRAMES.md).

### Engine, stars, orbit points

- `prometheia::Engine::calc(body, time, options)` over DE, SPK and catalog
  bodies, with apparent-place corrections (light time, gravitational
  deflection, annual aberration) applied as the caller asks, for geocentric,
  topocentric, heliocentric, barycentric and planet-centred observers.
- Fixed stars from a naked-eye BSC + Hipparcos set with an alias table, SIMBAD
  radial velocities cross-checked against the BSC.
  [docs/STARS.md](docs/STARS.md).
- **Nodes and apsides**, mean and osculating, as first-class computed points.
  Corrections apply to an orbit point exactly as to a body. The conventions
  and the measured magnitudes — including why a distant orbit's node moves
  21″ from the observer's velocity and ~0″ from light time — are in
  [docs/ORBIT-POINTS.md](docs/ORBIT-POINTS.md).
- Sidereal zodiacs (ayanamshas), on-demand covariances and position
  uncertainties.

### Server

- `prometheiad`, a WebSocket daemon speaking **ephemeris protocol version 4**,
  co-designed with the Astrolog project and locked from both sides: NAIF body
  IDs, profiles, instant lists, batched LOOKUP, per-object metadata with a
  truthful `corrApplied`, and SEGDATA — Chebyshev segments fitted on a
  server-owned 32-day lattice, with the ayanamsa carried as its own scalar
  series. Block-wise compute (~2 ms slices) with CANCEL and priority, cell
  caches shared across requests, TLS, token budgets, rate limits, and
  `/healthz`, `/readyz`, `/metrics` on the same port.
  [docs/SERVER.md](docs/SERVER.md), [docs/SEGMENTS.md](docs/SEGMENTS.md).
- The protocol's conformance fixtures are vendored with checksums and gated
  in-tree: 91/91, set digest pinned.

### Interfaces

- A C ABI (29 entry points, opaque handles) and `ephem`, a C99 CLI over it.
  [docs/C_API.md](docs/C_API.md), [docs/EPHEM.md](docs/EPHEM.md).
- Tools: `prometheia-fetch` (Python), `prometheia-convert`,
  `prometheia-spk-trim`, `prometheia-info`, `prometheia-bench`,
  `prometheia-wire-client`.
- `pkg-config --cflags --libs prometheia` links both a C and a C++ consumer,
  from an install prefix or from the build tree; both are verified.
- Build requirements are C++20, CMake, zstd and a threads implementation.
  OpenSSL is optional and enables TLS in the daemon. `doctest`, `uWebSockets`
  and `uSockets` are vendored.

### Accuracy, measured

Method and full tables in [docs/VALIDATION.md](docs/VALIDATION.md) and
[docs/ENGINE.md](docs/ENGINE.md).

| quantity | measured |
|---|---|
| DE440 against JPL's `testpo.440` (13,201 points) | 1.4e-14 AU |
| `de440s.bsp` against the DE440 binary | 4 cm |
| Sun and planets, astrometric, vs JPL Horizons (geo / topo) | 6 µas / 11 µas |
| Moon, astrometric, geocentric (1900–2100) | ≤ 0.006″ |
| Apparent place of date, published frame offsets removed | ~1 mas |
| Precession/nutation, differential vs `swetest` | 0.0005″ |
| Small bodies, ±10 yr from element epoch, with SB441-N16 | 0.0053″ |
| Light-time range, Sun and planets | 1.1 m |

### Performance, measured

- **1.9 µs** per position without rates, **4.0 µs** with rates, and **2.95 µs**
  for the server's access pattern (every body at one instant before the next).
  These were 53 and 55 µs before the optimisation work in this release.
- A 90-day segment request for heliocentric Jupiter at a 0.1″ target fits in
  4 segments with a worst residual of 0.031″; a repeat is served wholly from
  the cell cache.

### Not included

- **Hypothetical bodies and polynomial elements** (protocol kinds 3 and 4):
  not served, not advertised. This is a settled end state with reasoning
  recorded in [docs/SERVER.md](docs/SERVER.md), not a gap awaiting work.
- **zstd wire payloads** and **`deadlineMs` as a strategy switch**: the field
  is parsed and advisory; no strategy switch is implemented, and the document
  says so.
- **Vondrák 2011 long-term precession**: unstarted by choice. IAU 2006
  degrades far from J2000, which matters only for DE441's ±13k-year span.
- **Fuzzing of the wire parser.** The protocol reader is exercised by valid
  fixtures and five hand-written malformed frames; it has never been fed
  systematically hostile input. Stated here because a network server's parser
  is the part a reader should be told about.
- **Soak, load and concurrency testing.** Single-client, short-session only.

### Data and licensing

GPL-2.0-or-later. Sources are open with no strings attached; attribution-only
is acceptable and no CC BY-SA material is used anywhere. Data that is not
committed is reproducible from a committed `tools/` script with checksums.
Catalogs are published as tagged release assets rather than repo-tree files,
so a downstream can pin a tag and reproduce any historical answer.

Vendored third-party code: `doctest`, `uWebSockets`/`uSockets`, and the
Astrolog project's `ephproto.h` protocol header with its registries, each with
checksums recorded in `third_party/README.md`.

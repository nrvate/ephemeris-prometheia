# Fixed stars and deep-sky objects

The fixed-star catalog covers the naked-eye sky, plus the 110 Messier
objects.
- **Lookup:** by any designation or name — Bayer, Flamsteed, HR, HD, HIP,
  Messier, the IAU name, or a traditional name (Acrab = Graffias = β Scorpii
  = HR 5984).
- **Positions:** computed like any other body. They carry the star's space
  motion, light deflection, aberration, precession and nutation into any
  observer, frame and zodiac.

```cpp
#include <prometheia/stars.hpp>
size_t acrab = prometheia::stars::find("Graffias").value();     // or "β¹ Sco", "HR 5984"
const auto& o = prometheia::stars::at(acrab);                    // designations, magnitude, astrometry
auto r = engine.calc_star(acrab, jd_tt, prometheia::CalcOptions{}); // apparent ecliptic of date
```

## Sources and terms

Every source is declared, with its terms and a pinned SHA-256, in
`tools/fetch/stars_fetch.py`. None of them carries share-alike or
non-commercial terms.

| source | what it gives | terms |
|---|---|---|
| Yale Bright Star Catalogue, 5th revised ed. (Hoffleit & Warren 1991), CDS V/50 | the star list (9,110 entries to V ≈ 6.5): HR, Bayer/Flamsteed name, HD, SAO, FK5, V magnitude, colours, spectral type, radial velocity, multiplicity | CDS: free use, acknowledge |
| Hipparcos, the New Reduction (van Leeuwen 2007), CDS I/311 | astrometry: ICRS position at epoch J1991.25, parallax, proper motions and their errors | CDS: free use, acknowledge |
| SIMBAD (Wenger et al. 2000), one query | HR ↔ HIP cross-identifications | CDS: free use, acknowledge |
| IAU Catalog of Star Names, IAU Division C WGSN (exopla.net) | the IAU names, with HR/HIP and Bayer designations and each name's origin | IAU: CC BY 4.0 |
| R. H. Allen, *Star-Names and Their Meanings* (1899), Internet Archive OCR text | the check for the curated traditional names: every alias must appear in the book | public domain |
| SIMBAD, one query | the Messier objects: ICRS position, object type, angular size | CDS: free use, acknowledge |
| SIMBAD, one query | radial velocities of Hipparcos stars (quality A–C, 91,438) | CDS: free use, acknowledge |
| SIMBAD, one query | traditional "NAME" identifiers of HR stars: the cross-check of which star each curated name belongs to | CDS: free use, acknowledge |
| NASA HEASARC Messier Nebulae table (from *Sky Catalogue 2000.0* vol. 2) | the Messier objects' constellations; a second set of positions to cross-check SIMBAD | US Government service |
| Identification of a Constellation from Position (Roman 1987, PASP 99, 695), CDS VI/42 | the constellation boundaries (Delporte 1930), equinox B1875.0 | CDS: free use, acknowledge |
| Basic Fifth Fundamental Catalogue, FK5 Part I (Fricke et al. 1988), CDS I/149A | not used by the catalog: the outside check on its positions ("Checked against FK5") | CDS: free use, acknowledge |
| Swiss Ephemeris general documentation (Astrodienst), published | not used by the catalog: the published definitions of the zodiacs defined at the instant (FRAMES.md). Read, never copied; its code is never read | published documentation |
| SIMBAD, one query | Sgr A*'s ICRS position (Petrov et al. 2011), for the Galactic-Centre zodiacs | CDS: free use, acknowledge |
| Reid & Brunthaler 2020 (ApJ 892, 39), arXiv abstract | Sgr A*'s apparent proper motion | facts from a public abstract |
| Liu, Zhu & Zhang 2011 (A&A 526, A16), arXiv abstract and paper | the IAU 1958 galactic pole in the ICRS and the modern pole, for the galactic-node zodiacs | facts from a public preprint |
| Sixth Catalog of Orbits of Visual Binary Stars (USNO/GSU), orbits, format and ephemeris | the relative orbits of Sirius, Procyon and α Cen AB ("Binary stars"), and the published ephemeris that checks them | US Government work, public |
| Bond et al. 2017, Bond et al. 2015, Pourbaix & Boffin 2016: arXiv title searches | the component masses of Sirius, Procyon and α Cen, which split each orbit between its stars | facts from public abstracts |

Acknowledgements the terms ask for:
- This research has made use of the SIMBAD database and the VizieR catalogue
  access tool, operated at CDS, Strasbourg, France.
- Star names from the IAU Catalog of Star Names, IAU Working Group on Star
  Names (CC BY 4.0).

## Re-creating the data

The fetched files are not committed. `stars-raw/` is gitignored.

```sh
tools/fetch/stars_fetch.py --list                       # the 26 sources
tools/fetch/stars_fetch.py --raw-dir stars-raw          # ~17 MB, 26 requests, 5 s apart
tools/fetch/stars_fetch.py --raw-dir stars-raw --verify # against the pinned checksums
```

A source that changed upstream fails `--verify`: a new IAU name, a
SIMBAD update, a re-OCR of the book. To refresh deliberately:
1. Refetch with `--force --only NAME`.
2. Regenerate the catalog.
3. Review the diff.
4. Pin the new checksum in the script.

## The generated catalog

`tools/gen/gen_star_catalog.py --raw-dir stars-raw` writes
`src/star_catalog.inc`. It first checks the cache against the pinned
checksums; `--check` reports whether the committed file is stale.

**Contents** (at the 2026-09-17 sources):
- **Stars:** 9,096 Bright Star Catalogue stars (every entry with a
  position). 8,997 have Hipparcos astrometry; the rest keep the catalogue's
  J2000 position, proper motion and parallax.
- **Named stars:** 84 further IAU-named stars beyond the Bright Star
  Catalogue, with Hipparcos astrometry. 553 IAU names are attached. The 75
  without a Hipparcos star are mostly nicknames of nebulae and protostars.
- **Traditional names:** 38 curated names that the IAU list lacks, from
  `data/star_names.txt`.
- **Messier:** the 110 Messier objects.
- **Constellations:** the 357 constellation boundary segments.

Per object:
- HR, HD, HIP, Flamsteed and Messier numbers;
- the Bayer letter and superscript, and the constellation;
- kind: star, galaxy, globular, open cluster, nebula, planetary nebula,
  supernova remnant, asterism, double star;
- the astrometry's source;
- V magnitude;
- ICRS right ascension and declination at the catalog epoch (J1991.25 for
  Hipparcos, J2000 otherwise);
- proper motions (RA · cos Dec, and Dec) and parallax;
- radial velocity: SIMBAD's (quality A–C, mostly modern surveys) where it
  has one, 7,712 stars; else the Bright Star Catalogue's;
- angular size;
- spectral type;
- names (IAU first).

**Identification.**
- **Hipparcos match:** a Bright Star Catalogue star takes its Hipparcos
  entry from SIMBAD's HR–HIP cross-identifications. A component-suffixed
  HIP identifier counts only for the primary (A). 56 stars SIMBAD does not
  cross-identify are matched by position instead: the nearest Hipparcos star
  within 30″ at J2000, magnitudes within 1.
- **IAU names:** an IAU name attaches by its HR designation first (it names
  the component of a double sharing one HIP number, as Pulcherrima does for
  ε Boo B), then by HIP.
- **Designations shared by components:** the brighter component takes a
  Bayer or Flamsteed designation both carry.
- **Curated names:** each must be found in Allen's text, allowing for OCR
  damage (one letter in 6–10, two beyond). The generator prints the
  passage each matched.

**Cross-checks**, run on every generation:

| check | result |
|---|---|
| Bright Star Catalogue J2000 position vs Hipparcos carried to J2000 | median 0.7″, 99% within 3.6″, max 15″ (8,997 stars); a match over 60″ is refused |
| IAU name's HR/HIP/Bayer designations vs the star it attaches to | all agree (398 Bayer letters compared); a disagreement is fatal |
| IAU printed coordinates vs the star | median 0.01″; 15 errata below |
| curated names vs SIMBAD's NAME identifiers | 91 confirmed on the same star; a name SIMBAD gives to a different star is refused (this removed Scheat from δ Aqr, Deneb from ζ Aql and Algenib from α Per) |
| one object per name | enforced |
| Bright Star Catalogue radial velocities vs SIMBAD | median 1.3 km/s over 7,477 stars; 453 differ by over 10 km/s (spectroscopic binaries and variables, where SIMBAD's modern values are used) |
| SIMBAD vs HEASARC Messier positions | median 0.58′, within half the object's size; 1 erratum below |

**Upstream errata found by the cross-checks.** The identification is right
in each case (the designations agree); only the printed coordinates are
wrong, and the catalog uses Hipparcos or SIMBAD positions.
- **IAU Catalog of Star Names page (2026-08-13).** The printed RA/Dec sit
  0.28°–60° from the star:
  - Ebla (HD 218566): RA 47.29°, actual 347.29°;
  - Alrescha: its Dec column repeats its RA;
  - also Alsephina, Nganurganity, Alrakis, Tegmine, Alkalurops, Copernicus,
    Tonatiuh, Mira, Mizar, Rasalgethi, Chalawan, Errai and Veritate.
  - The page gives some stars J1991.25 coordinates and others J2000, so
    both epochs are compared (Barnard's Star moves 90″ between them).
- **HEASARC Messier table:** M 67's RA is 08ʰ50ᵐ24ˢ; SIMBAD and the
  cluster's published position put it one time-minute later. HEASARC
  omits M 102; SIMBAD identifies it with NGC 5866.

## Lookup

`stars::lookup(query, max, prefix)` returns every object answering to a query,
best first; `stars::find(query)` returns the one it means.

- **Matching ignores** case, spaces, punctuation and accents. Greek letters
  may be written as symbols (β), names (beta) or abbreviations (bet, and
  SIMBAD's alf, tet, ksi). Superscripts may be written ¹ or 1.
- **Names:** the IAU name, then the curated traditional names ("Acrab",
  "Graffias", "Zuben Elgenubi" = "Zubenelgenubi").
- **Bayer designations:** letter plus the constellation's abbreviation,
  genitive or name ("bet1 Sco", "Beta1 Scorpii", "β¹ Scorpius").
  - The designation of one component is an exact match.
  - A query without the superscript ("Beta Sco") answers every component
    as an alias match.
  - `find` then takes the brightest.
- **Flamsteed designations:** "8 Sco", "80 Ursae Majoris".
- **Catalog numbers:** "HR 5984", "HD 144217", "HIP 78820", "M 45",
  "Messier 45".
- **Prefix lookups** add names beginning with the query (3 letters at
  least), for completion.
- **`find` errors:** ArgumentError (listing the candidates) when several
  different objects match equally well; NotFound when nothing does.

Match qualities (exact, alias, prefix) follow the LOOKUP message planned
for Astrolog's ephemeris protocol version 4.

Each object also carries:
- its constellation — the designation's, else the one containing its
  position (`stars::constellation_at`, from the Delporte boundaries at
  B1875.0);
- the kind, magnitude, spectral type and size;
- its astrometry and the source of that astrometry.

`stars::constellations()` lists the 88 abbreviations, names and genitives.

## Apparent place

`Engine::calc_star(index, jd_tt, options)` and `calc_star_ut` compute a
catalog object like any body: any observer (geocentric, topocentric,
heliocentric, barycentric, planet-centred), frame, coordinates and zodiac,
with rates from central differences.

1. **Space motion.** The catalog position at its epoch (J1991.25 for
   Hipparcos, J2000 otherwise) is placed at its parallax distance and moved
   in a straight line to the date. The velocity combines the proper motion
   across the line of sight and the radial velocity along it.
   - An object without a parallax is placed at 10¹⁰ AU (reported as its
     distance) and takes its proper motion only.
2. **Parallax.** The observer's barycentric position is subtracted.
3. **Light deflection** by the Sun (skipped within the solar disc, and for
   Sun-centred or barycentric observers), then **aberration** from the
   observer's velocity, each as options say. `light_time` does not apply:
   a catalog position is already the direction light arrives from.
4. **Output.** Frame bias, precession and nutation into the requested
   frame, then the sidereal zodiac, exactly as for bodies.

Right ascension and declination come with `Coords::Equatorial`, ecliptic
longitude and latitude with `Coords::Ecliptic`, in any frame: ICRF, J2000,
mean or true of date.

## From C, `ephem` and prometheiad

- **C interface** (`prometheia.h`):
  - `prometheia_star_count`, `prometheia_star_find`,
    `prometheia_star_lookup` (graded matches), `prometheia_star_info`;
  - `prometheia_calc_star` and `prometheia_calc_star_ut`;
  - `prometheia_constellation_at`.
  - Answers are bit-identical to the C++ engine (tests/test_c_api.cpp).
- **`ephem`:** `star:NAME` among the bodies: `ephem star:Graffias
  "star:Beta Scorpii" star:M45 --equatorial`.
- **prometheiad:** object kind 1 (a fixed star by name) is answered for any
  name or designation above, with the request's observer, flags and
  zodiac. An unknown or ambiguous name fails that object alone, with the
  reason in its error text.

## Validation

`tests/test_stars.cpp`:
- **Always:** the catalog contents, every lookup form, and the 88
  constellations. Every one of the 3,000+ designated Bright Star Catalogue
  stars lies inside its designation's constellation boundaries.
- **With DE440:** positions against **ERFA** (the IAU SOFA algorithms),
  generated by `tools/gen/gen_star_fixtures.py` (pyerfa, pinned in `tools/requirements-oracle.txt`)
  into `tests/star_fixtures.inc`.
  - **Objects:** 12, chosen to exercise every term — Sirius, Canopus,
    Arcturus, Polaris, Acrab, Alcyone, 61 Cyg A, Barnard's Star, Proxima
    Centauri, Spica, a star with Bright Star Catalogue astrometry, and M 31.
  - **Epochs:** 1900, 1950, J2000, 2026 and 2100.
  - **ERFA's pipeline:** pmsafe, apci13, atciq, equation of the origins,
    true obliquity; pmpx for the astrometric place.

| quantity | max difference | gate |
|---|---:|---:|
| apparent RA/Dec, true equator and equinox of date | 0.34 mas | 1 mas |
| apparent ecliptic longitude/latitude of date | 0.34 mas | 1 mas |
| astrometric RA/Dec, ICRS | 0.33 mas | 1 mas |

The largest difference is Barnard's Star in 1900. It is the fastest-moving
star, with a radial velocity of −110 km/s, and ERFA's space motion includes
the light-time term that the straight-line model leaves out. Every other
object agrees to about 0.15 mas.

### Checked against FK5

The cross-test's `stars` leg shows `prometheiad` and `astrolog-ephd` agree to
milliarcseconds, but both take their stars from Hipparcos-derived catalogues,
so an error they share would pass. `tools/check/stars_fk5.py` compares a
server with the **FK5** (Fricke et al. 1988), which is ground-based and
predates Hipparcos:
- **Stars:** the stars leg's 29, matched by the Bright Star Catalogue's FK5
  number. Castor is not compared: FK5 287 is HR 2890, Castor's fainter
  component, and the catalog's Castor is HR 2891.
- **Question:** each star as a barycentric direction, ICRS equatorial, no
  corrections, at 1900, 2000 and 2100.
- **Reference:** the FK5 entry carried into the Hipparcos frame and to the
  epoch by ERFA's `fk52h` and `pmsafe` (pyerfa).
  - Where the FK5 gives no parallax (Rigel, Deneb, Zubeneschamali, Acrux),
    the reference uses a nominal 1 mas and no radial velocity.
  - At zero parallax ERFA caps the space motion, which would erase the
    proper motion and put Zubeneschamali 10″ off.

Measured 2026-09-18 on `prometheiad`:
- **The ordinary stars:** 24 of them, all within 0.56″ over the two
  centuries. The worst is Antares in 1900. At 2000 all are within 0.31″.
- **Size of the differences:** several times the FK5's stated mean errors.
  Those errors leave out the FK5's system errors, per its ReadMe.
- **Binaries.** The FK5 fits a straight line to about two centuries of each
  star wobbling about its barycentre, which finds the barycentre. For the
  stars that carry their orbit ("Binary stars" below), what is compared is
  ours: the server's star less its orbit offset, which `binary_orbits.py`
  computes independently.
  - **α Cen A:** 0.73/0.32/0.73″ at 1900/2000/2100. The straight line had
    been 28.6/6.8/17.4″ off: Hipparcos's α Cen A carries the orbit's
    velocity of 1991.
  - **Sirius and Procyon:** 1.76/0.69/2.33″ and 1.35/0.17/1.55″. These are
    exactly the old straight-line numbers, because both lines were already
    barycentric (Hipparcos orbital solutions).
    - What is left is the two catalogues' barycentric proper motions, about
      20 mas/yr apart, not the orbit.
    - An earlier version of this section said Hipparcos carried the
      companion's orbit for these two. That was wrong.
  - **Achernar and Polaris** (1.01″, 0.78″) carry no orbit here. Their
    orbits are much less certain.

**`astrolog-ephd`** (Astrolog `qt` at `c04cdf6`, before the orbits here): the
same picture for the stars both serve, within 0.007″ of `prometheiad` on every
star and epoch.

The FK5 cannot check the α Centauri names. It lists α Cen A only (FK5 538),
with no separate entry for B, and Proxima is far too faint for it.

The band is 1″, and 3″ for the binaries. **Both are estimates, not
measurements.** The check is there to catch a wrong star, a wrong proper
motion or a wrong epoch, which are arcseconds or more. A proper motion off by
10 mas/yr, injected into Vega, shows as 1.1″ and fails.

### Binary stars

Four stars move on their orbits, not in straight lines. For each, the catalog's
straight line comes from Hipparcos, and what that line *is* decides how the
orbit is added. That is read from the star's Hipparcos solution type (I/311,
new and old reductions):

| star | Hipparcos line | how the orbit is added |
|---|---|---|
| Sirius A (HIP 32349) | the barycentre (an orbital solution in the published catalog, kept in the new reduction) | the star's whole offset from its barycentre |
| Procyon A (HIP 37279) | the barycentre (as Sirius) | the whole offset |
| α Cen A (HIP 71683) | the star itself in 1991, orbital velocity included (a 5-parameter component solution) | the offset less its value and rate at the catalog epoch: only the curvature the line misses |
| α Cen B (HIP 71681) | a weak component solution (proper motion ±20–26 mas/yr) | placed from α Cen A plus the relative orbit; its own line gives only its distance |

- **The relative orbits** are from the Sixth Catalog of Orbits of Visual
  Binary Stars (USNO): Bond et al. 2017 for Sirius, Bond et al. 2015 for
  Procyon, Akeson et al. 2021 for α Cen.
  - They are evaluated with the Thiele–Innes constants, position angle
    from north through east.
  - The nodes refer to the equinox of 2000 (the catalog's equinox field;
    blank for α Cen, where the ephemeris fits 2000 too). The orbit is used
    in that fixed frame, never re-precessed.
  - The offset is laid in the tangent plane at the line's direction *at the
    date*, whose north is the one position angles are measured from. The
    catalog position's plane, used until 2026-09-18, turned α Cen's orbit
    by d(RA)·sin(Dec) of its proper motion since 1991: 0.065° by 2025, about
    10 mas at its separation.
  - They match the catalog's published ephemeris, 2025–2029, in both
    coordinates. The separations agree to the milliarcsecond. The position
    angles agree to 0.024° once the ephemeris's precession to the date is
    taken off, inside its 0.1° printing; that precession is 0.13–0.21° for
    these three. The Astrolog session found the position-angle convention
    and asked that it be checked.
- **The split between the two stars** is by mass: Sirius 2.063 and 1.018
  M☉ (Bond et al. 2017), Procyon 1.478 and 0.592 M☉ (Bond et al. 2015),
  α Cen 1.13 and 0.97 M☉ (Pourbaix & Boffin 2016). A primary is offset by
  −M_B/M of the relative orbit; the other star is the relative orbit away
  from it.
- **What it changes:**
  - Sirius A moves about its barycentre by up to 3.9″ and Procyon A by up
    to 1.7″, which the straight line never showed.
  - α Cen A no longer drifts away: 28.6″ from the FK5 at 1900 before, and
    0.73″ for its barycentre now.
  - α Cen B − A is the orbit's separation exactly (14.11″ at J2000, against
    the orbit's 14.13″), where the two straight lines gave 16.47″.
- **Why α Cen B is placed from A:** its own solution is poor. A barycentre
  shared by both lines, weighted by mass, took B's proper motion with it,
  and the FK5 then put that barycentre 6.4″ off at 1900, against 0.73″
  from A's solution alone.
- **Tested** (`tests/test_stars.cpp`, "stars_binary_orbits"):
  - α Cen A sits exactly at its catalog place at the catalog epoch.
  - α Cen B − A matches the published ephemeris for 2025–2029: separations
    to 2 mas, position angles to 0.06° (measured 0.024°). With the orbit in
    the catalog position's plane, three of the five epochs fail.
  - Sirius sits off its barycentre line by its share of the published
    separation, in the published direction.
  - The ERFA fixtures (`gen_star_fixtures.py`) apply the orbit
    independently, from ORB6's own file.
- **Against `astrolog-ephd`,** which moves stars in straight lines, the
  cross-test's `stars` leg expects each of these stars to differ by exactly
  our orbit's bend, and it does, to 3 mas.

## Known limits

- Stars without a radial velocity in either source move in proper motion
  only. This matters only for fast, near stars over centuries.
- Motion is a straight line from the Hipparcos epoch, except for the four
  stars that carry their orbits ("Binary stars").
  - Other stars with unseen companions still wobble about their lines,
    among them Achernar (1.0″ from the FK5 at 2100) and Polaris (0.78″).
  - Their orbits are too uncertain to apply.
- Deep-sky positions are their SIMBAD centres. Extended objects have no
  single position; M 40 and M 73 are loose groups.


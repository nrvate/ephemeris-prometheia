# Fixed stars and deep-sky objects

The fixed-star catalog covers the naked-eye sky, plus the 110 Messier
objects.
- **Lookup:** by any designation or name — Bayer, Flamsteed, HR, HD, HIP,
  Messier, the IAU name, or a traditional name (Acrab = Graffias = β Scorpii
  = HR 5984).
- **Positions:** computed like any other body. They carry the star's space
  motion, light deflection, aberration, precession and nutation into any
  observer, frame and zodiac.

This document is built up as the work lands. So far it covers the sources,
re-creating the data and the generated catalog. The lookup rules and the
apparent-place computation are added with the engine code.

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
| SIMBAD, one query | traditional "NAME" identifiers of HR stars: the cross-check of which star each curated name belongs to | CDS: free use, acknowledge |
| NASA HEASARC Messier Nebulae table (from *Sky Catalogue 2000.0* vol. 2) | the Messier objects' constellations; a second set of positions to cross-check SIMBAD | US Government service |
| Identification of a Constellation from Position (Roman 1987, PASP 99, 695), CDS VI/42 | the constellation boundaries (Delporte 1930), equinox B1875.0 | CDS: free use, acknowledge |

Acknowledgements the terms ask for:
- This research has made use of the SIMBAD database and the VizieR catalogue
  access tool, operated at CDS, Strasbourg, France.
- Star names from the IAU Catalog of Star Names, IAU Working Group on Star
  Names (CC BY 4.0).

## Re-creating the data

The fetched files are not committed. `stars-raw/` is gitignored.

```sh
tools/fetch/stars_fetch.py --list                       # the 12 sources
tools/fetch/stars_fetch.py --raw-dir stars-raw          # ~11 MB, 12 requests, 5 s apart
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
- proper motions (RA · cos Dec, and Dec), parallax and radial velocity;
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


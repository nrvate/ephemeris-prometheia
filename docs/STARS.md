# Fixed stars and deep-sky objects

The fixed-star catalog covers the naked-eye sky, plus the 110 Messier
objects.
- **Lookup:** by any designation or name — Bayer, Flamsteed, HR, HD, HIP,
  Messier, the IAU name, or a traditional name (Acrab = Graffias = β Scorpii
  = HR 5984).
- **Positions:** computed like any other body. They carry the star's space
  motion, light deflection, aberration, precession and nutation into any
  observer, frame and zodiac.

This document is built up as the work lands. It covers the sources and how
to re-create the data; the catalog format, the lookup rules and the
apparent-place computation are added with the generator and the engine
code.

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

Acknowledgements the terms ask for:
- This research has made use of the SIMBAD database and the VizieR catalogue
  access tool, operated at CDS, Strasbourg, France.
- Star names from the IAU Catalog of Star Names, IAU Working Group on Star
  Names (CC BY 4.0).

## Re-creating the data

The fetched files are not committed. `stars-raw/` is gitignored.

```sh
tools/fetch/stars_fetch.py --list                       # the 8 sources
tools/fetch/stars_fetch.py --raw-dir stars-raw          # ~11 MB, 8 requests, 5 s apart
tools/fetch/stars_fetch.py --raw-dir stars-raw --verify # against the pinned checksums
```

A source that changed upstream fails `--verify`: a new IAU name, a
SIMBAD update, a re-OCR of the book. To refresh deliberately:
1. Refetch with `--force --only NAME`.
2. Regenerate the catalog.
3. Review the diff.
4. Pin the new checksum in the script.

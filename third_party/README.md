# Third-party code

Vendored verbatim, never modified. Each directory keeps its upstream license.

| directory | what | version | license | source |
|-----------|------|---------|---------|--------|
| `ephproto/` | Astrolog's ephemeris protocol (version 3), the byte-level authority prometheiad speaks (docs/SERVER.md) | Astrolog commit `632f3c0` | GPL-2.0 (Astrolog, `license.htm` in its tree) | `/shares/Astrolog` → `ephsrv/ephproto.h`; `server_ephproto_matches_astrolog` checks it against `$PROMETHEIA_ASTROLOG` |
| `doctest/` | single-header C++ test framework (test code only; not linked into `libprometheia`) | 2.5.3 | MIT (`doctest/LICENSE.txt`) | https://github.com/doctest/doctest, tag `v2.5.3`, `doctest/doctest.h` |

To verify: `sha256sum third_party/doctest/*`.

```
cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421  doctest/doctest.h
0fe0b331fa1513dcce8604ff1fa925f32d1cea17d8aeb1c2471fad40d291adc5  doctest/LICENSE.txt
531ef7192e27a6d87ce099f84d38c80a7e116c84013d2432e6c4e20eb36629ae  ephproto/ephproto.h
```

To update, fetch the same two files from the new release tag, replace them, and
update the table and checksums.

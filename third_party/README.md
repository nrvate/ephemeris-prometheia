# Third-party code

Vendored verbatim; the one local change is the uSockets patch below. Each
directory keeps its upstream license.

| directory | what | version | license | source |
|-----------|------|---------|---------|--------|
| `ephproto/` | Astrolog's ephemeris protocol (version 3), the byte-level authority prometheiad speaks (docs/SERVER.md) | Astrolog commit `632f3c0` | GPL-2.0 (Astrolog, `license.htm` in its tree) | `/shares/Astrolog` → `ephsrv/ephproto.h`; `server_ephproto_matches_astrolog` checks it against `$PROMETHEIA_ASTROLOG` |
| `uWebSockets/` | WebSocket server library, prometheiad's transport (`src/` and `LICENSE` only) | v20.80.0, commit `3ffd6f44c9c3c92c96345d9f96bd01ba9c025ab5` | Apache-2.0 (`uWebSockets/LICENSE`) | https://github.com/uNetworking/uWebSockets |
| `uSockets/` | its event-loop and socket layer (`src/` and `LICENSE` only), with `uSockets-write2.patch` applied | commit `86097c490263ab662d62e8e7b541390bdec7d149`, the one v20.80.0 pins | Apache-2.0 (`uSockets/LICENSE`) | https://github.com/uNetworking/uSockets |
| `doctest/` | single-header C++ test framework (test code only; not linked into `libprometheia`) | 2.5.3 | MIT (`doctest/LICENSE.txt`) | https://github.com/doctest/doctest, tag `v2.5.3`, `doctest/doctest.h` |

To verify: `sha256sum third_party/doctest/*`.

```
cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421  doctest/doctest.h
0fe0b331fa1513dcce8604ff1fa925f32d1cea17d8aeb1c2471fad40d291adc5  doctest/LICENSE.txt
531ef7192e27a6d87ce099f84d38c80a7e116c84013d2432e6c4e20eb36629ae  ephproto/ephproto.h
```

The two network libraries match Astrolog's vendored copies of the same
commits (`ephsrv/uWebSockets`, `ephsrv/uSockets`), including the patch:
`us_socket_write2` did not set `last_write_failed` on a partial write, so a
partial write from inside the writable callback had its writable poll turned
back off and the buffered tail was never sent. Astrolog found and fixed it
(its review item S2). Built without TLS (`LIBUS_NO_SSL`) and without zlib
(`UWS_NO_ZLIB`). Checksum of the two trees, file by file in sorted order
(run in `third_party/`: `find uWebSockets uSockets -type f | sort | xargs sha256sum | sha256sum`):
`4e1b8459c1df6348203d7d650ab64fb12c31d1240b22877e5fd8899432cb03cc`.

To update doctest, fetch the same two files from the new release tag, replace them, and
update the table and checksums.

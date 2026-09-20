# Working rules for this repository

Durable rules for anyone (human or AI assistant) contributing here.
Point-in-time state lives in [docs/HANDOFF.md](docs/HANDOFF.md); the
decision history in [docs/DESIGN.md](docs/DESIGN.md).

## Cleanroom

This is a cleanroom successor to Swiss Ephemeris. Never read SWE source
or headers — no `swe*.cpp`, no `swephexp.h` — anywhere on this machine.
Comparing **output numbers** against an installed `swetest` binary is
fine and is the established oracle method; reading its code is not. By
the same standing agreement with the Astrolog project, do not read
`ephsrv/ephswiss.h` or the compute path of `eph_srv.cpp` in their tree;
their `ephsrv/ephproto.h` (the wire spec) is explicitly fine, and the
byte-level authority for protocol v4 is vendored at
`third_party/ephproto/v4/`.

## Data

Open sources with no strings attached. Attribution-only is fine;
**no CC BY-SA** material (that rules out Wikipedia and OpenNGC as
sources). Data that is not committed must be reproducible from a
committed `tools/` script with checksums — never an ad-hoc fetch.
Public services (JPL/Horizons, SBDB, SIMBAD…) are treated politely:
strictly sequential, paced, an identifying User-Agent, and ask the
maintainer before any large pull.

## Git

- Commit as `nrvate <11264848+nrvate@users.noreply.github.com>` (the
  repo-local config is pinned; do not rely on any global identity).
  Commits up to `c1cfc4b` carry the maintainer's personal address
  instead; GitHub began refusing pushes that expose it on 2026-09-19,
  and the maintainer chose the noreply address over relaxing the
  setting. Do not rewrite the older commits to match.
- The working branch is `initial`. Push over SSH to `origin/initial`
  only — never `main`, and never use the `gh` CLI for anything.
- No hosted CI, ever: no GitHub Actions, no external runners. The gate
  is `tools/gate.sh`, run locally before every commit, and it must stay
  fast (seconds); anything slow goes behind `doctest::skip()`.
- Stage named files only, never `git add -A` — the tree sits beside
  large gitignored data directories (`sbdb-raw*`, `stars-raw`,
  `horizons-raw`, `ephe/`, `build*`).
- No session IDs or session URLs in commits or PRs, ever.

## Build, test, format

- C++20, no globals, one engine context per object; `namespace
  prometheia`; every file carries an SPDX GPL-2.0-or-later header.
- `tools/gate.sh` runs clang-format, the v4 registries check, and the
  full test set in both `build/` (Release) and `build-asan/`
  (ASan+UBSan). It must pass before every commit.
- Test binaries live at `build/test_*` (not `build/tests/`); build a
  single suite with `cmake --build build --target test_<name>`.
- The `Result` API is `.ok()` / `.value()` / `.error()` — there is no
  `operator->` or `operator*`, and a failed Result carries a
  zero-initialized value, so always check `.ok()` before comparing.
- Real-data tests are env-gated (`PROMETHEIA_SOURCE_DIR`, …) and skip
  cleanly when the big files are absent.

## Conventions worth keeping

- **When a commit changes behavior, update every prose surface that
  describes it in the same commit** — header comments *and* docs. This
  project has twice shipped a stale sentence describing superseded
  behavior (`engine.hpp`'s orbit-point comment, ENGINE.md's "the point
  is geometric"), and both were found by an outside reviewer.
- Dates in docs and commit messages come from
  `git log --format=%ci`, not from memory.
- `docs/C_API.md` is a cross-repo contract: the Astrolog project's
  plugin builds against it. ABI changes are relayed to them through the
  maintainer before they land, never shipped unannounced.
- Promises in docs are measured numbers or they say "estimate".

# EPM1 — the Ephemeris Prometheia catalog container (version 1.2)

**Status:** implemented by `src/catalog.cpp` (writer + reader), exercised by
`tests/test_catalog.cpp`. This document is the normative spec.

## Design intent

A catalog stores **initial conditions, not answers**: one osculating-elements
record per body, at one epoch, with 1-sigma element uncertainties and optional
physical parameters. The engine integrates positions on demand, so the file
has **no time axis** — file size scales with the number of bodies, not with
bodies × time-span. Random access by body id and full streaming scans are
both first-class; compression is per-chunk so either access pattern pays only
for what it touches.

## Conventions

- All multi-byte integers and IEEE-754 floats are **little-endian**, written
  bit-explicitly (never `memcpy` of a host integer), so the format is
  host-endian independent.
- Strings are UTF-8, NUL-terminated inside the string pool. A record carries
  no strings inline.
- Offsets are absolute byte positions from the start of the file.
- Compression is **zstd** (RFC 8878), applied per chunk over the chunk's raw
  (uncompressed) record bytes.

## File layout

```
+-------------------------------+ offset 0
| header (64 bytes)             |
+-------------------------------+
| chunk data (zstd frames)      |
+-------------------------------+
| string pool (raw UTF-8)       |
+-------------------------------+
| chunk index (N x 40 bytes)    |
+-------------------------------+
| metadata (one CBOR map)       |
+-------------------------------+
| footer (48 bytes)             | end of file
+-------------------------------+
```

A writer emits the header as a placeholder and rewrites it in `finish()`,
because the section offsets are only known once the pool, index and metadata
sizes are. The footer repeats the offsets so a streaming reader (e.g. over
HTTP Range) can locate the index from the tail alone.

## Header (64 bytes)

| off | type | field |
|----:|------|-------|
| 0   | u32  | magic, `0x314D5045` ("EPM1" little-endian) |
| 4   | u16  | format major (1; bump for breaking changes) |
| 6   | u16  | format minor (1: adds the `kCovariance` record block; 2: `kOrbitClass` and `kHasProvDes`; readers accept any minor of major 1 and reject unknown record flags) |
| 8   | u32  | header size in bytes (64) |
| 12  | u32  | flags: bit 0 = chunks are zstd-compressed |
| 16  | u64  | record count |
| 24  | u32  | records per chunk (chunk granularity) |
| 28  | u32  | reserved (0) |
| 32  | u64  | chunk index offset |
| 40  | u64  | metadata offset |
| 48  | u64  | metadata size in bytes |
| 56  | u64  | string pool offset |

## Footer (48 bytes)

| off | type | field |
|----:|------|-------|
| 0   | u64  | chunk index offset (must equal header's) |
| 8   | u64  | metadata offset (must equal header's) |
| 16  | u64  | metadata size |
| 24  | u64  | string pool offset |
| 32  | u64  | string pool size |
| 40  | u32  | magic (same as header) |
| 44  | u16  | format major |
| 46  | u16  | format minor |

A reader validates header and footer agree; disagreement means a truncated
or damaged file. Missing footer → corrupt/truncated; the reader reports
`CorruptionError`.

## Chunk index

`ceil(record_count / chunk_records)` entries of 40 bytes each, in ascending
order of `first_spkid`:

| off | type | field |
|----:|------|-------|
| 0   | u64  | chunk data offset (absolute) |
| 8   | u64  | stored size in bytes (compressed if flag set, else raw) |
| 16  | u64  | raw (uncompressed) size in bytes |
| 24  | u32  | CRC-32 (IEEE, reflected, 0xEDB88320) of the **raw** bytes |
| 28  | u32  | record count in this chunk |
| 32  | u64  | spkid of the chunk's first record |

Lookups binary-search the index on `first_spkid`, load at most one chunk,
then scan it linearly (records within a chunk are ascending by spkid).

## Record encoding

Records are variable-length, concatenated within a chunk, in ascending spkid
order:

| order | type | field |
|------:|------|-------|
| 1 | uvarint | spkid (LEB128; 1-10 bytes) |
| 2 | u8 | body class: 0 asteroid, 1 comet, 2 planet (reserved), 255 other |
| 3 | u8 | flags (below) |
| 4 | f64 | epoch, JD TDB, equinox J2000 |
| 5 | f64 ×6 | elements: a [AU, negative for hyperbolic], e, i [rad], Ω [rad], ω [rad], M₀ [rad] |
| 6 | f32 ×6 | 1-sigma of the six elements, same order and units (angles in radians) — only if flag `kSigmas`; uncorrelated summaries, not a covariance |
| 7 | f32 ×2 | H magnitude, G slope — only if flag `kHg` |
| 8 | f32 | diameter [km] — only if flag `kDiameter` |
| 9 | f64 + f64 ×6 + f64 ×21 | covariance block — only if flag `kCovariance` (1.1): covariance epoch (JD TDB); the nominal cometary elements at that epoch {e, q [AU], tp [JD TDB], Ω [rad], ω [rad], i [rad]}; the 6×6 covariance of those elements, packed upper triangle row-major ((0,0), (0,1), …, (5,5)), units squared |
| 10 | u8 | SBDB orbit class — only if flag `kOrbitClass` (1.2): a code of the fixed table below |
| 11 | uvarint | name offset into the string pool |

Record flag bits:

- bit 0 `kSigmas` — uncertainty block present
- bit 1 `kHg` — magnitude block present
- bit 2 `kDiameter` — diameter present
- bit 3 `kHasName` — the pool entry continues past the pdes NUL with a proper
  name (derived by the writer from the presence of a name argument; readers
  must not trust it beyond splitting the pool string)
- bit 4 `kCovariance` (1.1) — the orbit solution's full covariance, as the
  source publishes it (JPL SBDB `cov=mat`): its own epoch and element basis,
  so no conversion error enters the container. The six element sigmas
  (`kSigmas`) are uncorrelated summaries and cannot stand in for it.
- bit 5 `kOrbitClass` (1.2) — the orbit-class byte is present.
- bit 6 `kHasProvDes` (1.2) — the pool entry carries a provisional
  designation after the pdes (and the name, when `kHasName`). Kept only for
  a body whose primary designation differs from it, a numbered one.

**Orbit classes** (1.2), SBDB's `class` codes, fixed for the life of the
format (`OrbitClass` in `include/prometheia/catalog.hpp`). The code is a
byte; 0 is "not recorded" and is never stored.

| code | class | | code | class |
|---:|---|---|---:|---|
| 1 | AST asteroid, unclassified | | 20 | JFc Jupiter-family comet |
| 2 | IEO Atira | | 21 | JFC Jupiter-family comet (classical period criterion) |
| 3 | ATE Aten | | 22 | HTC Halley-type comet |
| 4 | APO Apollo | | 23 | ETc Encke-type comet |
| 5 | AMO Amor | | 24 | CTc Chiron-type comet |
| 6 | MCA Mars-crosser | | 25 | COM comet, unclassified |
| 7 | IMB inner main belt | | 26 | PAR parabolic comet |
| 8 | MBA main belt | | 27 | HYP hyperbolic comet |
| 9 | OMB outer main belt | | | |
| 10 | TJN Jupiter trojan | | | |
| 11 | CEN centaur | | | |
| 12 | TNO trans-Neptunian object | | | |
| 13 | PAA parabolic asteroid | | | |
| 14 | HYA hyperbolic asteroid | | | |

Every class in the 2026-10 SBDB holdings is in the table (21 of them; PAA
is documented by SBDB and held none). A record with any other byte is
corruption. The converter refuses a class the table lacks, naming it.

**Writer validation** (records failing these are rejected at `add()`):
strictly ascending spkid; all six elements and epoch finite; `e >= 0` and
`e != 1`; `a != 0` with sign(a) = sign(1 − e) (elliptic a > 0, hyperbolic
a < 0); no NUL inside pdes or name; unknown flag bits rejected; a covariance block
must be finite with non-negative variances, cometary `e >= 0`, `e != 1` and
`q > 0`. Unknown body
classes are rejected on decode.

**uvarint** is LEB128: 7 payload bits per byte, least significant first,
high bit set on all but the final byte, at most 10 bytes.

## String pool

Raw UTF-8. Per record: `pdes` NUL-terminated, then optionally `name`
NUL-terminated when `kHasName` is set, then optionally the provisional
designation NUL-terminated when `kHasProvDes` is set (1.2). A record's `name_offset` points at the
start of its `pdes`. The pool is not compressed in v1 (~15 MB at full-catalog
scale); a future minor version may zstd-compress it without breaking the
record format.

## Metadata

Exactly one CBOR map, decoded with the library's RFC 8949 subset codec
(unsigned/negative ints, text, f64 floats, bool/null, arrays, maps; nesting
depth capped at 64). The converter writes: `format`, `generator`, `frame`,
`time_scale`, `elements`, `source`, `source_url`, `fetched_started_utc`,
`fetched_finished_utc`, `sbdb_count`, `rows_skipped`, `spkid_dupes`.
Metadata is descriptive — correctness of positions never depends on it.

## Integrity

- Per-chunk CRC-32 over raw bytes, stored in the index, verified on every
  read (scan and lookup both).
- zstd frames detect decompression failures.
- Header/footer cross-check catches truncation of the tail.
- Structural validation on open: offsets in range, index entries ascending
  with nonzero counts, per-chunk counts summing to the header's record count.

Corruption inside a chunk is detected when that chunk is read, not at open —
this is deliberate: opening a catalog with one bad chunk out of thousands
should not require reading all of it.

## Extension policy

Minor versions may only add flag bits, body classes or metadata keys —
readers of a newer file by an older library must fail loudly on unknown flag
bits (they do), which is the trade for v1 simplicity. Anything else is a
major-version bump with a new magic.

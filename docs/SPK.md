# NAIF SPK reader

`prometheia::spk` (in `include/prometheia/spk.hpp`, `src/spk.cpp`) reads
SPK ephemeris kernels (`.bsp`), the container JPL publishes its DE
ephemerides in alongside the native binaries ([DE.md](DE.md)) and the
format most other astronomy software exchanges. Written from NAIF's
public DAF and SPK format documentation; no SPICE code is used.

## Test corpus

`de440s.bsp` (32.7 MB, `ssd.jpl.nasa.gov/ftp/eph/planets/bsp/`): DE440
from 1849-12-26 to 2150-01-22, 14 type-2 segments. Kept in the gitignored
`ephe/` directory (`PROMETHEIA_DE440S`); real-file tests SKIP without it.

## DAF byte map

A DAF is a sequence of 1024-byte records addressed by 1-based record
number; array data is addressed by 1-based 8-byte *word* numbers.

File record (record 1):

| offset | type | field |
|-------:|------|-------|
| 0 | 8 chars | identification word, `DAF/SPK ` |
| 8 | i32 | ND, doubles per summary (SPK: 2) |
| 12 | i32 | NI, integers per summary (SPK: 6) |
| 16 | 60 chars | internal file name (`NIO2SPK` in de440s) |
| 76 | i32 | FWARD, first summary record |
| 80 | i32 | BWARD, last summary record |
| 84 | i32 | FREE, first free word |
| 88 | 8 chars | byte order, `LTL-IEEE` or `BIG-IEEE` |
| 699 | 28 bytes | FTP validation string |

The FTP string (`FTPSTR:\r:\n:\r\n:\r\0:\x81:\x10\xce:ENDFTP`) exists to
catch files damaged by an ASCII-mode transfer; the reader rejects a file
whose string is present but altered.

Summary records form a doubly linked list. Each holds three control
doubles (next record, previous record, summary count) followed by
summaries of ND + ⌈NI/2⌉ = 5 words; the integers are packed into the
bytes of the trailing doubles. The record after each summary record holds
the segment names, 8 × 5 = 40 characters each.

SPK summary: start and end epoch (TDB seconds past J2000), then target,
center, frame, segment type, first word, last word.

## Segment types 2 and 3

Records of RSIZE words, each `MID, RADIUS` then `DEGREE + 1`
coefficients per component (type 2: x, y, z; type 3: x, y, z, vx, vy, vz).
The last four words of the segment are `INIT, INTLEN, RSIZE, N`. For an
epoch *et*: record `floor((et − INIT) / INTLEN)` clamped to `[0, N−1]`,
`τ = (et − MID) / RADIUS`. Type 2 velocity is the series derivative
divided by RADIUS; type 3 evaluates its velocity series. Units are km and
km/s; the reader returns km/day to match `prometheia::de`.

de440s segments:

| target / center | degree | interval |
|-----------------|-------:|---------:|
| 1 Mercury bary / 0 | 13 | 8 d |
| 2 Venus bary / 0 | 9 | 16 d |
| 3 EMB / 0 | 12 | 16 d |
| 4 Mars bary / 0 | 10 | 32 d |
| 5–9 outer barycentres / 0 | 7, 6, 5, 5, 5 | 32 d |
| 10 Sun / 0 | 10 | 16 d |
| 301 Moon / 3, 399 Earth / 3 | 12 | 4 d |
| 199 Mercury / 1, 299 Venus / 2 | 1 | one record (zero offset) |

The reader validates each type 2/3 directory (`RSIZE × N + 4` equals the
segment length, shapes integral) at open. Other segment types are listed
but not evaluable (`FormatError`).

## Chaining and precedence

`SpkFile::state(target, center, jd, out)` walks each body up its segment
chain (body → segment center → … → a body with no segment), summing
states, then subtracts at the first common ancestor. When several
segments cover a body at an epoch, the one later in the file wins — the
SPK precedence rule. `state_et` / `segment_state_et` take TDB seconds past
J2000 directly: a JD double near the present only resolves ~40 µs, which
matters for queries at exact segment boundaries.

An `SpkFile` caches one record per segment and is not safe for
concurrent use.

## Validation

`tests/test_spk.cpp`:

- **Synthetic** (runs everywhere, no data files needed), in both byte orders:
  type-2 and type-3 segments, a later segment overriding the middle
  record of an earlier one, an unsupported type-1 segment; direct
  evaluation across record boundaries (exact), chaining in both
  directions through the common ancestor, precedence; error paths
  (missing, non-DAF, FTP-damaged and truncated files, unsupported type,
  coverage, no path, bad segment index).
- **de440s.bsp**: segment table pinned (targets, centers, types, frame,
  names, coverage, degrees, intervals); cross-checked against the DE440
  native binary at 9,008 states over 1850–2149 (Moon/Earth, Earth/Sun,
  Mercury, Venus, Mars, Jupiter, Pluto, Sun) — worst |Δposition| 4.1 cm,
  |Δvelocity| 2.4 × 10⁻⁶ km/day: the same ephemeris in two containers.

## Writing and trimming

`spk::write_spk(path, internal_name, comments, segments)` writes a
little-endian DAF/SPK file of type 2/3 segments (`spk::WriteSegment`: the
summary fields plus the raw records), with the comment area in NAIF's
convention (NUL line separators, EOT terminator, 1000 characters per
record) and summary/name records chained 25 summaries each.
`SpkFile::segment_records()` and `SpkFile::comments()` read the raw
records and the comment text back.

`spk::trim_segments(file, et0, et1)` keeps, per type 2/3 segment, the whole
records covering [et0, et1], copied verbatim, with the directory's initial
epoch and the coverage narrowed to them; states inside the span are
bit-identical. `prometheia-spk-trim <in> <out> --from <JD> --to <JD>` wraps
it and prepends a provenance line to the original comments.

- **Test** (`tests/test_spk.cpp`, `spk_writer_roundtrip_and_trim`): a type-2
  (degree 5) and a type-3 (degree 2, offset half a record) segment of 12
  records each with arbitrary coefficients round-trip exactly (metadata,
  records, a 1500-character comment); trimmed to 3.25–7.5 records, every
  state sampled inside matches the original bit for bit, epochs outside are
  refused, and an empty span or segment list is an error.
- **Real kernel:** JPL's `sb441-n16.bsp` (645.7 MB, −8000 … +9000) trimmed to
  the DE440 span (JD 2287184.5 … 2688976.5) is 41.8 MB (16 of 64 segments);
  200,000 random states over the span agree bit for bit with the original,
  and the Horizons small-body gate gives identical numbers on either file.

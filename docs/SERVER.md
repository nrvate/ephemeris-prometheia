# prometheiad

`prometheiad` serves the engine to Astrolog. It speaks **Astrolog's ephemeris
protocol (version 3)** over WebSocket, so Astrolog's ephemeris client can
point at it the way it points at Astrolog's own server (`astrolog-ephd`).

- **Protocol.** Astrolog owns it. Its byte-level authority is
  `ephsrv/ephproto.h` in the Astrolog tree. This repository keeps a pinned,
  unmodified copy in `third_party/ephproto/`. When an Astrolog checkout is
  named by `$PROMETHEIA_ASTROLOG`, the test `server_ephproto_matches_astrolog`
  fails if the copy has drifted from Astrolog's.
- **Transport.** Binary WebSocket frames, one protocol message per frame. The
  default port is 47190 (`eph::kDefaultPort`).

## Running it

```sh
prometheiad --ephemeris ephe/linux_p1550p2650.440 \
            --catalog sbdb.epm --perturbers ephe/sb441-n16-de440span.bsp \
            --wire-map astrolog.wiremap --port 47190
```

- **Options.**
  - `--threads N`: the number of event loops (default: one per hardware
    thread). Each loop opens its own engine.
  - `--bind ADDR`: the listen address.
  - `--max-cells N`: the per-request bound WELCOME advertises.
  - `--cache-mb N`: the result cache per loop.
  - `--verbose`: log each connection.
- **Stopping.** SIGINT or SIGTERM closes the listeners and every connection.
- **Reference client.** `prometheia-wire-client --port 47190 --obj 0 --jd
  2461300.5 --count 3` sends HELLO and one REQUEST, and prints each object's
  metadata and one line per row. `--help` lists the options.

## Licence of the binary

uWebSockets and uSockets are Apache-2.0. Apache-2.0 is compatible with
GPL version 3 but not with version 2, so `prometheiad` and
`prometheia-wire-client`, which link them, are distributable under
GPL-3.0-or-later terms. The library, `ephem` and the other tools do not link
them and stay GPL-2.0-or-later.

## The protocol in brief

- **Envelope.** Every message starts with a 16-byte little-endian envelope:
  magic `0x1EF0`, the message's version, flags, type, request id and payload
  length.
- **HELLO / WELCOME.**
  - The client's first message is HELLO. The server answers WELCOME in the
    lower of the two ends' highest versions (2 or 3), and every later message
    is written in that version.
  - A client below version 2 gets ERROR 8 in its own version, and the
    connection closes.
  - WELCOME advertises the limits: 64 objects, 20,000 rows, 500 rows per
    chunk, 4 MiB payloads, and the cell bound (objects × rows, default
    100,000).
- **REQUEST.**
  - A request carries up to 64 objects: a body by id, a fixed star by name,
    or a node/apsis.
  - The request-wide fields follow: center, a 64-bit `iflag`, the sidereal
    triple, the topocentric site, a start instant, a step in seconds, a row
    count, the precision (f64 or f32) and a chunk-size hint.
  - Row *r* is at `jdStart + r·stepSeconds/86400`: UT, or TT when `iflag`
    bit 32 is set.
- **DATA.**
  - The answer comes back in chunks of contiguous rows.
  - Each chunk carries every object's 128-byte metadata (return flag, error
    text, name), then the values: object-major, six per row.
  - A failed row is six NaNs. An object whose rows all failed has return
    flag −1.
- **ERROR.** Whole-request failures only: 1 malformed, 2 over a limit,
  3 unknown message type, 4 internal, 8 version too old.
- **PING / PONG.** An application-level heartbeat.

## The wire map

The protocol names bodies, flag bits and sidereal modes by the Swiss
Ephemeris numbering. This repository is a cleanroom and holds none of those
numbers: they come from the interface table in Astrolog's protocol
specification, written into a **wire-map file** that the server loads at
startup. A number without an entry means nothing here, and the objects that
use it fail on their own (NaN rows, return flag −1, the reason in the error
text) while the rest of the request answers.

One entry per line, whitespace-separated, `#` starts a comment:

| entry | meaning |
|---|---|
| `body <wire-id> <naif-id> [name]` | a body the engine knows by NAIF id; the name goes into the DATA metadata |
| `asteroids <wire-base>` | wire id *base + N* is numbered asteroid *N* (SBDB SPK-ID 20000000 + *N*) |
| `flag <bit> <meaning>` | bit 0–31 of `iflag` |
| `sidereal <wire-mode> <zodiac>` | `fagan-bradley`, `lahiri` or `user` |

Flag meanings, applied to the engine's apparent-place defaults (geocentric,
light time, deflection, aberration, true equator/ecliptic of date, ecliptic
coordinates):

| meaning | effect |
|---|---|
| `speed`, `ignore` | none (rates are always computed and sent) |
| `heliocentric`, `barycentric`, `topocentric` | the observer; topocentric uses the request's site (degrees east, degrees, metres). More than one fails the objects |
| `equatorial` | right ascension and declination |
| `j2000`, `icrs` | mean equator and equinox of J2000; ICRF axes (wins over `j2000`) |
| `no-nutation` | mean equinox of date |
| `true-position` | no light time |
| `no-aberration`, `no-deflection` | switch that correction off |
| `astrometric` | both of the above off |
| `sidereal` | the zodiac `sidereal` maps the request's mode to; `user` takes the anchor epoch (TT JD) and its mean ayanamsha (degrees) from the request |
| `xyz` | rectangular position (AU) and velocity (AU/day) instead of angles |
| `radians` | angles and angular rates in radians |

Always answered per object, never as an ERROR:

- **Unsupported for now:** fixed stars, nodes and apsides, planet-centred
  positions (a nonzero center or `iflag` bit 33), any bit or mode without a
  wire-map entry, and bodies neither the ephemeris nor a loaded catalog has.
- **Return flag:** the request's low 32 bits of `iflag` for an object with at
  least one computed row.
- **WELCOME:** reports Swiss Ephemeris version 0.

The wire map for Astrolog is not in this repository yet: its numbers must
come from Astrolog's specification, written outside this cleanroom.

## Server internals

`server/ws_server.{hpp,cpp}` is the WebSocket head:

- **Event loops.** One uWebSockets event loop per thread, each with its own
  engine and `LoopContext`, all listening on one port through `SO_REUSEPORT`.
  The kernel balances the accepts. For `--port 0` a free port is chosen
  first, because uSockets enables port sharing only on a nonzero port.
- **Messages.**
  - Each binary message goes to the connection's `Session`.
  - Replies are sent while less than 4 MiB is buffered; the rest follow from
    the drain callback.
  - uWebSockets' own backpressure limit is off, because past it uWebSockets
    silently drops sends, ERRORs included.
  - After a closing error the connection ends once the error is sent.
- **Heartbeats.** WebSocket protocol pings, with a 30-second idle timeout.
- **Build.** No compression, TLS or HTTP routes.

`server/ws_client.{hpp,cpp}` is a small blocking WebSocket client (masking,
fragment reassembly, answering pings) behind `prometheia-wire-client` and
`tests/test_prometheiad.cpp`. That test runs two loops on the synthetic
kernel in-process and covers, in about 0.15 s:

- the upgrade and a 1,200-row answer in three chunks, checked against the
  engine;
- PING, with six simultaneous clients;
- closing after REQUEST-before-HELLO;
- `stop()` closing live connections.

### The protocol core

`server/session.{hpp,cpp}` is the protocol core, with no sockets in it:

- **`Session`** takes one WebSocket message at a time (`on_message`) and hands
  back whole messages to send (`next`). Control replies (WELCOME, ERROR,
  PONG) come first, then DATA chunks in request order.
  - `on_message` returning false means close once the queued replies are
    sent: REQUEST before HELLO, or a version below 2.
  - A connection holding four computed answers it has not read yet is refused
    a fifth with ERROR 2, before anything is computed.
- **`LoopContext`**, one per event-loop thread, owns that thread's `Engine`
  and a least-recently-used **result cache** (64 MiB by default). The cache
  key is the REQUEST payload minus its two delivery-only fields (precision
  and chunk size): the same question asked for f32 in large chunks is a hit
  on the f64 answer.
- **Answers** are pure functions of the request and the files loaded at
  startup, so failures are cached with the rest.

`tests/test_server.cpp` drives `Session` directly on the synthetic linear
kernel (wire-map numbers invented for the test), in about 10 ms. It covers:

- negotiation and version refusal;
- malformed frames and limits;
- chunking and f32;
- bit-exact values against `Engine::calc`/`calc_ut`;
- each flag meaning;
- per-object failure and cache hits.

## Not implemented

- **From Astrolog's version 3:** tokens (ERROR 7) and per-address rate
  limits (ERROR 6). HELLO's token is accepted and ignored.
- **Other features:** zstd payloads, and the planned negotiated extra columns
  (sigma, ayanamsha).
- **Operations:** TLS, the `/healthz`, `/readyz` and `/metrics` routes, and
  connection caps (Astrolog's server has all of these).

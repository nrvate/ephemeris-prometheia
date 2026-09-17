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

- **Options.** `prometheiad --help` lists them all.
  - `--threads N`: the number of event loops (default: one per hardware
    thread). Each loop opens its own engine.
  - `--bind ADDR`: the listen address.
  - `--max-cells N`: the per-request bound WELCOME advertises.
  - `--cache-mb N`: the result cache per loop.
  - `--verbose`: log each connection.
  - Limits, tokens, TLS and draining: see Operations below.
- **Reference client.** `prometheia-wire-client --port 47190 --obj 0 --jd
  2461300.5 --count 3` sends HELLO and one REQUEST, and prints each object's
  metadata and one line per row. `--token`, `--tls`, `--ca`, `--sni` and
  `--insecure` cover the options below; `--help` lists the rest.

## Operations

These follow Astrolog's own server (`astrolog-ephd`), so a deployment
behaves the same with either.

- **Connection caps.** `--max-conns N` (default 10,000) in total and
  `--max-conns-per-ip N` (default 64) per peer address; 0 disables a cap. A
  refused client gets HTTP 503 with the reason at the upgrade, before any
  WebSocket exists.
- **HELLO deadline.** A connection that has not sent HELLO after
  `--hello-seconds N` (default 10; 0 = never) is closed (1008).
- **Tokens.** `--tokens FILE` lists accepted tokens, one a line (`#` lines
  and blank lines skipped; at most 128 bytes each). The token is HELLO's
  version-3 field. With `--require-token`, a HELLO without a known token gets
  ERROR 7 and the connection closes. The log counts tokens and never prints
  them.
- **Compute budget.** Each peer address, or each known token, has a bucket of
  cells (objects × rows) that refills at `--cells-per-sec N` (default 10,000;
  0 = no budget) up to `--max-cells`, so one full request is always possible
  from a full bucket. A REQUEST over budget gets ERROR 6, "rate limited: N
  cells a second; ask again in S s", and later requests are served. The
  charge is made whether or not the answer is cached.
- **Probes and metrics**, on the same port and scheme:
  - `GET /healthz`: `ok` while the process answers.
  - `GET /readyz`: `ready`, or 503 with the reason while draining or before
    every loop listens.
  - `GET /metrics`: Prometheus text summed over the loops. It covers
    connections (open, total, refused, HELLO timeouts), HELLOs, requests,
    cells computed, cache hits and misses, bytes sent, backpressure waits,
    ERRORs by code, a compute-time histogram, a draining gauge and build
    info. Nothing in it names a client or a request.
- **Draining.** SIGTERM or SIGINT stops accepting (the listeners close).
  Connections with answers queued or bytes unsent get up to
  `--drain-seconds N` (default 10) to finish. Then every connection is closed
  with 1001 (hard, for one whose client stopped reading) and the process
  exits 0. A second signal exits at once.
- **TLS.** `--tls-cert FILE --tls-key FILE` serves wss:// (and the probes
  over HTTPS) on the same port.
  - At startup the pair is checked: readable PEM, a key that matches, a
    certificate valid now.
  - SIGHUP re-reads both into every loop, after the same check. A bad pair
    keeps the current one; established sessions are untouched.
  - Protocols and ciphers: TLS 1.2 and later. TLS 1.2 is restricted to
    forward-secret AEAD suites; TLS 1.3 uses OpenSSL's defaults.
  - The build: TLS is compiled in when CMake finds OpenSSL
    (`PROMETHEIA_WITH_TLS`, on by default then).
- **Port sharing.** Every loop listens on the port with `SO_REUSEPORT`, so a
  second server would bind the same port silently and share its traffic.
  Startup therefore refuses a port something already accepts connections on.

## Licence of the binary

uWebSockets, uSockets and OpenSSL 3 (linked for TLS) are Apache-2.0. Apache-2.0 is compatible with
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

- **Planet-centred positions:** a nonzero request `center`, or center 0
  with `iflag` bit 33, names the observing body by its wire body id,
  resolved through the wire map like any object. A center without an
  entry, or a center together with an observer flag, fails the objects.
- **Nodes and apsides** (object kind 2): point 1–4 is the ascending node,
  descending node, perihelion or aphelion, and method 1 osculating or 0 mean.
  The body is a wire body id. The engine's `calc_orbit_point` answers it
  (docs/ENGINE.md, "Nodes and apsides"), in the request's observer, frame,
  flags and zodiac. The metadata name is the body's with " asc. node",
  " desc. node", " perihelion" or " aphelion" appended.
- **Fixed stars** (object kind 1): the name is looked up in the compiled-in
  star catalog. Any IAU or traditional name, Bayer, Flamsteed, HR, HD, HIP
  or Messier designation works (docs/STARS.md). The metadata name is the
  catalog's display name.
- **Unsupported for now:** any bit or mode without a wire-map entry, and
  bodies neither the ephemeris nor a loaded catalog has.
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
- **Build.** No compression; TLS when OpenSSL is found.

`server/ws_client.{hpp,cpp}` is a small blocking WebSocket client (masking,
fragment reassembly, answering pings) behind `prometheia-wire-client` and
`tests/test_prometheiad.cpp`. That test runs two loops on the synthetic
kernel in-process and covers:

- the upgrade and a 1,200-row answer in three chunks, checked against the
  engine;
- PING, with six simultaneous clients;
- closing after REQUEST-before-HELLO;
- `stop()` closing live connections;
- the connection caps (503, and the place coming back after a close);
- the HELLO deadline;
- `/healthz`, `/readyz` and `/metrics`;
- a drain that delivers all 30 chunks of an answer in flight before 1001;
- refusing a port already served;
- wss:// with a certificate generated by the test: verification against the
  right and a wrong trust anchor, HTTPS probes, reload, and a mismatched key
  refused at startup.

The whole suite takes about 0.7 s.

### The protocol core

`server/session.{hpp,cpp}` is the protocol core, with no sockets in it:

- **`Session`** takes one WebSocket message at a time (`on_message`) and hands
  back whole messages to send (`next`). Control replies (WELCOME, ERROR,
  PONG) come first, then DATA chunks in request order.
  - `on_message` returning false means close once the queued replies are
    sent: REQUEST before HELLO, or a version below 2.
  - A connection holding four computed answers it has not read yet is refused
    a fifth with ERROR 2, before anything is computed.
  - With `Limits` (`server/limits.{hpp,cpp}`), HELLO checks the token
    (ERROR 7) and each REQUEST is charged to its budget (ERROR 6).
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

## Throughput

Measured 2026-09-17 on DE440, one loop, `prometheia-wire-client` on the
same host:

- **Workload:** one REQUEST of the Sun, Moon and eight planets at hourly
  rows, 10,000 rows (100,000 cells, the default bound), apparent geocentric
  ecliptic with rates.
- **Result:** 0.29 s of compute, about 2.9 µs a cell. The request took
  0.51 s end to end, including 9.7 MB of f64 DATA.
  - A repeat was a cache hit: 0.23 s, which is transfer only.
  - The first measurement, before the engine work in docs/ENGINE.md
    ("Cost"), was 5.6 s (56 µs a cell).
- **Loop order:** the server evaluates time-major, every object at one row
  before the next row, so the objects share each instant's observer, Sun,
  frame and nutation work.
- **Comparison:** Astrolog's plan records 64 bodies × 20,000 rows in 13.7 s
  on its Swiss Ephemeris server, about 11 µs a cell.
- **Caching:** a new connection may land on another loop, which has its own
  cache.

## Not implemented

- **zstd payloads.** Reserved in the envelope, advertised by no one.
- **Extra columns** (sigma, ayanamsha). The plan is a negotiated extension,
  which has to go into Astrolog's specification first.
- **The wire map for Astrolog** (above).

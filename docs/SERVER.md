# prometheiad

`prometheiad` serves the engine to Astrolog. It speaks **Astrolog's ephemeris
protocol (version 4)** over WebSocket, so Astrolog's ephemeris client can
point at it the way it points at Astrolog's own server (`astrolog-ephd`).

- **Protocol.** Astrolog owns it; §3 of its `EPHEMERIS_PLUGINS_PLAN.md` is
  the normative spec, locked from both sides (below). The byte-level
  authority is `ephsrv/ephproto.h` in the Astrolog tree; this repository
  keeps a pinned, unmodified copy with its generated registries in
  `third_party/ephproto/v4/`. When an Astrolog checkout is named by
  `$PROMETHEIA_ASTROLOG`, the test `server_ephproto_matches_astrolog` fails
  if the copy has drifted, `tests/test_ephproto4.cpp` runs the locked
  conformance fixtures through the vendored codec (99/99, digest-pinned),
  and `tools/check/ephproto4_registries.py` (in `tools/gate.sh`) checks the
  registries against the header by name — in both directions: a registry
  that grew is reported as unknown to the checker, and one the JSON no
  longer carries is reported as a pin the pair has lost, because a registry
  that simply disappears from the file drops out of the loop and would
  otherwise leave the run saying "ok" over a smaller corpus.
- **Transport.** Binary WebSocket frames, one protocol message per frame. The
  default port is 47190 (`eph::kDefaultPort`).

## Running it

```sh
prometheiad --ephemeris ephe/linux_p1550p2650.440 \
            --catalog sbdb.epm --perturbers ephe/sb441-n16-de440span.bsp \
            --port 47190
```

- **Options.** `prometheiad --help` lists them all.
  - `--threads N`: the number of event loops (default: one per hardware
    thread). Each loop opens its own engine.
  - `--bind ADDR`: the listen address.
  - `--max-cells N`: the per-request bound WELCOME advertises.
  - `--max-seg-span-days N`: the widest span one segments REQUEST may ask
    (WELCOME's `segMaxSpanDays`; default 1024).
  - `--cache-mb N`: the result cache per loop.
  - `--seg-cache-mb N`: the fitted segment cells per loop (default 16).
  - `--hypotheticals FILE`: an element file of named hypothetical bodies
    (JSON Lines, [HYPOTHETICALS.md](HYPOTHETICALS.md)); repeatable, and a
    later file's definition of a token wins. The tokens defined are
    advertised in WELCOME.
  - `--log-level quiet|info|debug` (default `info`; `--verbose` is
    `debug`): one line per connection, HELLO, request and ERROR on stderr.
    See "Logging" below.
  - Limits, tokens, TLS and draining: see Operations below.
- **Dataset identity.** At startup the daemon digests every data file's
  contents and prints the dataset id it will serve
  (`<engine>/<ephemeris>/<catalogs>#<8 hex>`). The id changes whenever any
  answer could change; clients key their caches on it and may pin it.
  Each file's digest is SHA-256 over its size and the SHA-256 of each 8 MiB
  piece, computed on every core. `prometheia-json` with DE440 starts in
  0.10 s, where one SHA-256 stream made it 0.66 s (2026-09-19). The id changed with this
  once; it changes with every release anyway, since it carries the engine
  version. The
  digest covers the element set this build ships and every
  `--hypotheticals` file too, because a redefined token changes its answers.
- **Reference client.** `prometheia-wire-client --port 47190 --obj 599 --jd
  2461300.5 --count 3` sends HELLO and one REQUEST, and prints each object's
  metadata and one line per row. `--obj` takes a NAIF/SPK-ID, `--name` a
  catalog designation, `--star` a star, `--node N.P[.M]` an orbit point (P = `a|d|p|A`, M = `m`, `o` or 0–4);
  `--helio/--bary/--center NAIF/--eq/--j2000/--icrs/--sid/--topo` shape the
  profile, and `--corrections MASK` asks for a chosen subset of the three
  correction terms (`--no-corrections` is `--corrections 0`).
  `--token`, `--tls`, `--ca`, `--sni` and `--insecure` cover the options
  below; `--segments --target ARCSEC` asks for SEGDATA instead of samples
  (rectangular form; the ayanamsa series rides a `--sid` profile),
  `--priority 0|1` marks a prefetch, `--cancel-after-ms N` cancels the
  request N ms in to show CANCEL at work, and `--timeout S` waits longer
  than the default 10 s for each message (a large request on a busy server
  computes before its first byte); `--help` lists the rest.

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
  HELLO field. With `--require-token`, a HELLO without a known token gets
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

## Logging

`prometheiad` writes one line per event on stderr, timestamped in UTC:

```
prometheiad 2026-09-18T17:04:49.307Z c=0.0 open addr=127.0.0.1
prometheiad 2026-09-18T17:04:49.308Z c=0.0 hello v=4 client="prometheia-wire-client/0.2.0" token=none
prometheiad 2026-09-18T17:04:49.308Z c=0.0 req=2 accepted rows objs=4 (body:2,point:1,hyp:1) rows=2 profiles=1 prio=0
prometheiad 2026-09-18T17:04:49.308Z c=0.0 req=2 done rows objs=4 rows=2 chunks=1 objerr=- cache=miss ms=0.4
prometheiad 2026-09-18T17:04:49.308Z c=0.0 close code=1006 requests=1 ms=1
prometheiad 2026-09-18T17:04:49.310Z c=0.1 open addr=127.0.0.1
prometheiad 2026-09-18T17:04:49.311Z c=0.1 hello v=4 client="prometheia-wire-client/0.2.0" token=none
prometheiad 2026-09-18T17:04:49.311Z c=0.1 req=2 error=11 "a correction mask this server does not honour for that observer (WELCOME lists the pairs)"
prometheiad 2026-09-18T17:04:49.311Z c=0.1 close code=1006 requests=1 ms=0
```

(Two runs of `prometheia-wire-client`, captured 2026-09-18. Close code 1006
means the client dropped the socket without a close frame, which is what
the reference client does.)

- **`c=<loop>.<n>`** names the connection on every line. **`req=<id>`** is
  the client's request id, so `grep 'c=0.1 '` is one connection's whole
  story and `grep 'c=0.1 req=1 '` one request's. `prometheia-wire-client`
  picks a fresh id per run and prints it (`# request N`), so the client's
  output and the server's lines join on it.
- **Events at `info`:** a refused connection (the cap's reason), open,
  HELLO (client name, token `none`/`known`/`unknown`/`ignored`, never the
  token), each REQUEST accepted with objects by kind, rows, profiles and
  priority, its `done` line with per-object error codes (`code:count`),
  cache hit or miss and time from acceptance to the last chunk, every
  ERROR sent with its code and this server's text, LOOKUPs (query and match
  counts), a HELLO timeout, and close with the number of requests.
- **At `debug`:** also PINGs and a CANCEL that arrives after its answer was
  sent.
- **At every level, `quiet` included:** the operational lines, in the same
  timestamped format: the tokens file loaded (counted, never printed), the
  dataset, the Uranian and other named bodies, listening, SIGHUP's TLS reload
  or why not, a failed reload per loop, the drain on a signal, a second
  signal, `stopped after N s`, and fatal errors. Only a usage error on the
  command line prints plainly, before the log exists.
  ```
  prometheiad 2026-09-18T18:25:37.728Z listening on port 47190 (ws://, every interface, 1 loop)
  prometheiad 2026-09-18T18:25:37.743Z SIGHUP: no certificate to reload (plain ws://)
  prometheiad 2026-09-18T18:25:38.244Z signal 15: draining for up to 10 s
  prometheiad 2026-09-18T18:25:38.245Z stopped after 1 s
  ```
- **Never logged:** request instants, observer sites, element coefficients,
  object names or designations, LOOKUP query strings, tokens. A request is
  someone's birth data. The log says how many objects and rows, which codes
  and how long, not when or where. It is the rule §3.8 sets for ERROR text,
  applied to the log, and a test holds it
  (`server_log_traces_a_request_without_its_contents`).
- Embedders of `WsServer` get `LogLevel::Quiet` unless they ask.

## Licence of the binary

uWebSockets, uSockets and OpenSSL 3 (linked for TLS) are Apache-2.0. Apache-2.0 is compatible with
GPL version 3 but not with version 2, so `prometheiad` and
`prometheia-wire-client`, which link them, are distributable under
GPL-3.0-or-later terms. The library, `ephem` and the other tools do not link
them and stay GPL-2.0-or-later.

## The protocol in brief

Version 4 is a clean break from version 3 (`kProtoMin` 4): bodies are named
by NAIF/SPK-ID, options are typed profile fields, zodiacs are tokens, and
there is no wire map. The full spec is Astrolog's §3; the shape:

- **Envelope.** Every message starts with a 16-byte little-endian envelope:
  magic `0x1EF0`, the message's version, flags, type, request id and payload
  length. Version 4 only; a version-3 client gets the legacy ERROR 8
  refusal in its own layout.
- **HELLO / WELCOME.** The session version is `min(client.protoMax, 4)`,
  fixed by the first HELLO. WELCOME advertises the limits (64 objects,
  20,000 rows, 500 rows per chunk, 16 profiles, 4 MiB payloads, the cell
  bound), the caps bits (f32, instant lists, lookup, deep sky, cancel,
  priority, segments), the capability TLVs (kinds, observers,
  planes/forms/frames, per-observer correction masks, orbit points and
  methods, extra columns, the zodiacs this engine serves, sidereal planes,
  time scales, the delta T model, the lookup budget, the segments bounds —
  degree, per-object cap, the 0.001″ floor, the kinds fitted, the span —
  and the deep-sky catalogues) and the dataset id.
- **REQUEST.** A 16-byte delivery block (precision, priority,
  representation, degree hint, chunk hint, segment target, deadline — all
  advisory, all outside the cache key) then the question block: time scale
  and grid or instant list, the delta T (a table TLV, one value, or the
  canonical NaN for the server's model), up to 16 profiles and up to 64
  objects, each naming its profile. Objects: bodies by NAIF id, orbit
  points (node/apsis, mean or osculating), fixed stars by name,
  hypotheticals and polynomial elements (not served here: per-object
  error 2), designations (resolved exactly, as a LOOKUP).
- **DATA.** The answer comes back in chunks of contiguous rows; chunk 0
  carries the source table and per-object META (rows computed, error code
  and text, source, flags, `corrApplied`, resolved NAIF id, first failed
  row, name). Values are object-major, 6 plus the extra columns asked
  (sigma, ayanamsa, light time, delta T, in bit order). A failed row is all
  NaN; an object whose rows all failed says so without failing the request.
- **LOOKUP / LOOKUP_RESULT.** Batched name search with one budget for the
  whole message: catalog designations exactly, stars by name (prefix when
  asked), matches ordered by quality, `truncated` when the budget runs out.
- **ERROR.** 1 malformed, 2 over a limit, 3 unknown type, 4 internal,
  5 pin not served, 6 rate limited (retryable, with a delay), 7 token,
  8 version, 9 busy (retryable), 10 cancelled, 11 unsupported,
  12 draining.
- **PING / PONG.** An application-level heartbeat, requestId 0.

## What this server answers

The wire map is gone with version 3: bodies are NAIF/SPK-IDs the ephemeris
and catalogs answer directly, zodiacs are tokens this engine implements
(`fagan-bradley`, `lahiri`, `user`, and the eleven zodiacs defined at the
instant: [FRAMES.md](FRAMES.md), "Zodiacs defined at the instant"; they
refuse plane 1 per object, errCode 2), and the observers, planes, forms,
frames and corrections are typed profile fields. What the cleanroom server
does with them:

- **Profiles** resolve to the engine's `CalcOptions` once per request
  (session.cpp). Every object names its profile, so one request can mix a
  geocentric apparent chart with a heliocentric rectangular one. A profile
  this engine cannot serve (a zodiac it does not implement, or a fixed
  sidereal plane asked as segments) refuses the whole REQUEST with ERROR
  11; a body the data does not carry fails alone.
- **Times.** Rows are in the request's time scale (UT1, TT, TDB). The
  delta T comes from the request's table TLV (piecewise linear, TT
  instants), its one value, or — the canonical NaN — the engine's observed
  USNO model, which WELCOME names. The delta T column reports the value
  used.
- **Correction masks** are advertised as the protocol defines them (A.3
  0x0004): a list of the EXACT masks honoured, with the observers each is
  honoured for. Here that is every mask for every observer, except that the
  Sun's centre cannot be deflected, so a heliocentric profile has only masks
  0, 1, 4 and 5. Any other combination is ERROR 11 (§3.5a). Until 2026-09-18
  this server listed one mask per observer (7, and 5 at the Sun) while
  answering every combination. A client following the rules would then never
  have sent it mask 0, the geometric position CROSS-TEST.md calls the
  portable comparison. A body observer that is the Sun cannot be told apart
  in a capability that names observer kinds; there the answer's
  `corrApplied` shows that deflection did not apply.
- **Per-kind correction masks** (landed 2026-09-18, ephv4 `eed6429`).
  WELCOME tag 0x0014, `CORRECTIONS_BY_KIND`, is non-critical and can only
  add masks per (observer, kind) to 0x0004. 0x0004 is now the
  intersection over kinds, and a pair no 0x0014 entry names falls back to
  it. A profile's mask is checked against the kinds of the objects that
  reference it, ERROR 11 for the whole request; an unreferenced profile is
  checked against 0x0004 alone. Both maintainers approved revision 3 of
  the drop text. This engine honours the same masks for every kind, so
  `prometheiad` sends no 0x0014, its WELCOME is byte-identical, and its
  per-profile check already is the drop's rule. The wire client prints
  0x0014 entries as `# corrkind` lines. `wirelib.py` reads them.
  `crosstest.py` asks which masks are listed per (observer, kind).
- **corrApplied** reports structural availability per object, never the
  request's mask (below). Our table: bodies and orbit points carry light
  time and aberration always; deflection everywhere except an observer at
  the Sun (this engine cannot bend the Sun's own light, and a barycentric
  observer is 0.005 AU from the Sun, so it is deflected); stars carry
  deflection and aberration but no light time (catalog positions are
  directions of arrival); hypothetical bodies and bodies from elements
  carry what a body carries.
- **Orbit points** (kind 1) answer by the engine's `calc_orbit_point`
  (docs/ENGINE.md), points 0-3 by methods mean and osculating, and the
  Moon's apogee and perigee by method 2, interpolated (the natural apsides;
  WELCOME lists methods 0-2; any other point by method 2 is error 2). The Sun and
  the barycentre have no orbit (per-object error 2); an undefined point
  (a node of an orbit in the ecliptic, an apsis of a circle) is error 5.
- **Fixed stars** (kind 2) resolve through the compiled-in catalog by
  `stars::find`'s rules ([STARS.md](STARS.md)). A Bayer designation without
  its component number ("Beta Sco") means every component, and resolves to
  the brightest, Acrab. Only different stars matching a name equally well
  are ambiguous, and that is per-object error 6 rather than a silent choice.
  (An earlier version of this line gave "Beta Sco" as the ambiguous example,
  contradicting STARS.md. The Astrolog suite had copied it, and a
  cross-client probe found the disagreement.) A star without a parallax answers distance 0 with the
  `noDistance` flag. Deep-sky designations resolve for the Messier
  catalogue, which the caps name.
- **Named hypotheticals** (kind 3) resolve a token against the element set
  this build ships and any `--hypotheticals` files, ASCII case-insensitively;
  an undefined token is per-object error 1. The elements are server-defined
  (A.15), so the answer's source string names the set they came from. Kind
  3 is advertised only when at least one token is defined, with the tokens
  in A.3 0x0011.
- **Bodies from elements** (kind 4) are computed from exactly the elements
  sent: two-body motion by the protocol's rule, in any A.16 equinox (all
  five are advertised in A.3 0x0012), about the Sun or the Earth.
  Conventions and the mean-anomaly rule are in
  [HYPOTHETICALS.md](HYPOTHETICALS.md). Elements that are not a bound orbit
  at an instant fail that row with error 2. A client that needs a body from
  its own elements, identically on every server, sends it this way, not by
  name. Neither kind has a NAIF id, so META's `resolvedNaif` is the "none"
  value, and a body observer is never "the object".
- **Designations** (kind 5) resolve exactly as a LOOKUP of quality 0 or 1
  through the catalogs' name index; no match is per-object error 1.
- **Per-object errors** (A.17). One object failing never fails the request.
  The code is classified from the engine's error *type* first, and message
  text is consulted only for types that carry no client text. `errText` is a
  fixed sentence per code (§3.8), except for the reasons the server writes
  itself, which are content-free.

  | code | when | `errText` |
  |---|---|---|
  | 1 unknown | a NAIF/SPK-ID no loaded data answers; no star, designation or token of that name | not a body, name or token this server knows |
  | 2 unsupported | a NAIF id that is not a body id, or the barycentre; an orbit method or object kind not served; elements not a bound orbit at the instant; a kind not fitted as segments; the Moon's osculating apsides as segments | not supported by this server for this object (or the server's own reason) |
  | 2 unsupported | a body observer that is the object (a body, or a designation resolving to it; that body's orbit points are places and are answered) | the observer is the object |
  | 3 coverage | outside the ephemeris's time span, or its perturbers' | outside the ephemeris's time coverage |
  | 4 data | a data file unreadable or corrupt | the data this object needs is not loaded |
  | 5 undefined | a node of an orbit in the ecliptic, an apsis of a circle, the aphelion of an open orbit | the point is undefined for this orbit |
  | 6 ambiguous | a star name several stars answer equally | the name is ambiguous |
  | 7 numerical | a small body's integration failed | the computation failed numerically |
  | 8 internal | anything else | internal error |

  Whole-request refusals are ERROR 11: a zodiac, sidereal plane or
  correction mask this server does not advertise.
- **Speeds** off (profile `speeds` 0): the three rate columns are zero and
  META carries `noSpeeds`.
- **Pins.** REQUEST TLVs 0x8001-0x8003 name the ephemeris, catalog or
  dataset the answer must come from; anything else is ERROR 5.
- **Segments** (representation 1), CANCEL and priority ordering are
  served; see "The segment lattice" below. Named hypotheticals and bodies
  from elements are fitted like bodies. The fitted-cell key carries a named
  body's token and every coefficient of a body from elements, so two
  element sets never share a cell.

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
- a SEGDATA answer over the wire, and a CANCEL that stops a request still
  computing (no DATA precedes the ERROR 10, because chunks only follow a
  whole answer);
- refusing a port already served;
- wss:// with a certificate generated by the test: verification against the
  right and a wrong trust anchor, HTTPS probes, reload, and a mismatched key
  refused at startup.

The whole suite takes about 0.7 s.

### The protocol core

`server/session.{hpp,cpp}` is the protocol core, with no sockets in it:

- **`Session`** takes one WebSocket message at a time (`on_message`) and hands
  back whole messages to send (`next`); its owner works the computes between
  its own turns (`has_work`/`work`). Control replies (WELCOME, ERROR, PONG)
  come first, then the chunks of answers ready to stream — priority 0
  before priority 1, arrival order within one priority, one request's
  chunks always in order.
  - `on_message` returning false means close once the queued replies are
    sent: REQUEST before HELLO, or a version below 4.
  - A connection holding four answers it has not read yet is refused a fifth
    with ERROR 2, before anything is computed.
  - With `Limits` (`server/limits.{hpp,cpp}`), HELLO checks the token
    (ERROR 7) and each REQUEST is charged to its budget (ERROR 6).
  - A REQUEST no longer computes in the message callback. A samples answer
    is computed by `SamplesComputer` a block of rows at a time (~2 ms a
    slice; at least one row a slice, so every slice makes progress); a
    segments answer by `SegmentsComputer` a lattice cell at a time per
    object. Each slice installs the request's ΔT model on the loop's engine
    and restores it after, so slices of different requests interleave
    safely on the one engine. CANCEL drops the stream, which drops the
    computer — that is what stops the work — and answers ERROR 10 unless
    the id is unknown or everything was already sent.
- **The pump** (`ws_server.cpp`): after each message (and each drain), the
  loop takes a deferred pass that gives every session with work one slice
  and then sends what the slices finished, rescheduling itself while any
  work remains. Deferring is what leaves the loop free to read a CANCEL
  between slices; the pump copies the live-socket set at run time, and
  nothing touches a session after a flush that may have ended its socket.
- **`LoopContext`**, one per event-loop thread, owns that thread's `Engine`,
  a least-recently-used **result cache** (64 MiB by default) and the two
  **segment caches** (fitted cells and ayanamsa cells, 16 MiB each by
  default). The result cache key is the dataset id plus the question
  block's bytes, exactly as received: the delivery block (precision,
  chunking, deadline, priority, representation) is outside it, so the same
  question asked for f32 in small chunks is a hit on the f64 answer — and a
  segments answer, whose sharing unit carries the rung, never enters it.
- **Answers** are pure functions of the request and the files loaded at
  startup, so failures are cached with the rest. `server/dataset.{hpp,cpp}`
  digests those files into the dataset id (contents, not names: same bytes,
  same dataset).

`tests/test_server.cpp` drives `Session` directly on the synthetic linear
kernel plus the committed sample catalog, in about 10 ms. It covers:

- the handshake: WELCOME's bounds and capability TLVs, the session version,
  the legacy refusal of a version-3 envelope, malformed frames;
- the answer: chunking, f32, bit-exact values against `Engine::calc`,
  `calc_ut`, `calc_star` and `calc_orbit_point`, per-object failure
  (unknown body, partial coverage with the reason naming no instant), the
  designation kind, cache hits across deliveries;
- every profile field: observers and sites, planes, forms, frames,
  corrections, speeds off, the zodiacs served and refused, the user
  anchor, the observer-equals-object error;
- the times: UT1/TT/TDB, the one-value and table delta T (bit-exact
  against the same model installed on the check engine), instant lists,
  backward grids, the delta T and light-time columns;
- LOOKUP: catalog bodies by name and designation, stars with prefixes,
  the budget and `truncated`;
- CANCEL: unknown and fully-answered ids get silence, a mid-compute id gets
  ERROR 10 with the compute stopped and nothing cached, and the same
  question re-asked is answered whole;
- priority: an interactive request that arrives behind a prefetch is worked
  and answered first, each answer's chunks staying in order;
- segments: SEGDATA evaluated against the engine within the served rung,
  whole-cell coverage and contiguity, the per-object kind refusals
  (stars; the Moon's osculating points), the ayanamsa series against the
  engine's own ayanamsa, the span/floor refusals, and the second identical
  request served from the cell cache;
- limits and errors: objects/rows/cells/profiles over the bounds, busy,
  rate limited, the pins;
- the corrApplied table (above) as unit checks, and the dataset id's
  determinism.

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

## Protocol version 4 (agreed with Astrolog; §3 locked; **served here**)

Astrolog replaced version 3 with **version 4** on its `ephv4` branch: the
spec and conformance fixtures are `EPHEMERIS_PLUGINS_PLAN.md` §3 and
`ephsrv/conformance/` there. Version 4 names bodies by NAIF/SPK-ID, so the
wire map went away with the migration. Prometheia reviewed the draft and the
two sides agreed these semantics:

- **Requests** carry profiles, so one request can mix observers and options;
  instants come as a grid or a list; LOOKUP, CANCEL and segments are new.
- **Rates** are the time derivatives of the answered coordinates, per day of
  the request's time scale. A server whose rates differ from the central
  difference of its own positions flags them and advertises the size of the
  difference. Ours are that central difference.
- **ΔT** may come as a table of samples, interpolated piecewise-linearly, not
  one value for a whole span.
- **Sidereal** is ecliptic-only. The anchor epoch is TT and its ayanamsa is
  the mean one. True of date takes the true ayanamsa, mean of date the mean,
  J2000 and ICRF a constant zero point on the J2000 ecliptic — what the
  engine already does.
- **Sidereal planes (A.8)**, all three served since 2026-09-18:
  - the ecliptic of date;
  - the ecliptic of the anchor epoch: the mean ecliptic and equinox of t0,
    longitude from A0;
  - the invariable plane: DE440's, which WELCOME's engine string names
    ("invariable plane: DE440 total angular momentum at J2000 …").
  For planes 1 and 2 the profile's frame is ignored (the plane is the
  frame), and the ayanamsa column reports A0 (FRAMES.md, "Sidereal
  planes"). Segments serve plane 0 only: they carry the ayanamsa as a
  longitude shift, and a fixed plane is a rotation. A fixed plane asked as
  segments is ERROR 11, and rows are the way to ask.
- **Orbit points** lie on the mean ecliptic of date whatever the profile's
  frame, which gives only their coordinates (§3.5a as amended 2026-09-18),
  heliocentric
  for planets and geocentric for the Moon, and take the profile's corrections
  exactly as a body does (3.5a): they are interoperable applied in full or
  not at all ([ORBIT-POINTS.md](ORBIT-POINTS.md)). A mean model must name
  itself in the object's source string.
- **Fixed stars** have a normative name grammar (IAU name, Bayer with
  component numbers, Flamsteed, HR/HD/HIP). An ambiguous name is a per-object
  error rather than a silent choice, and an object without a parallax
  reports distance 0 with a flag.
- **Corrections** are honoured as sent for every observer, with the
  per-observer masks a server advertises.
- **Frames** are pinned: the ecliptic of date takes longitudes from the true
  equinox on the mean ecliptic; J2000 includes frame bias; ICRF does not; both
  use the IAU 2006 J2000 mean obliquity. Sites are WGS84 geodetic.
- **Privacy:** neither ERROR text nor per-object error text may carry any
  request contents (§3.8): not an instant, a place, or the name, token or id
  the client sent. This server met that only for instants until 2026-09-18,
  when a hypothetical-body message was about to echo a token. That exposed
  the whole class: unknown stars, designations, NAIF ids and zodiac tokens
  all came back verbatim. Per-object text is now a fixed sentence per error
  code. The code is still classified from the engine's full message, so
  only the echo is lost. `server_error_text_never_quotes_the_request` pins it
  for every object kind.

Settled in the same exchange:

- **Segments** carry tropical coefficients; the ayanamsa travels as its own
  segmented series per profile (segmented, not one series, because a
  true-of-date ayanamsa carries nutation). A `maxDegreeHint` rides in the
  delivery block, outside the cache key. The three error figures are the
  largest residuals the server *measured* against its own answers on a
  check set of at least 4(d+1) instants, not claimed bounds; a rate error
  is among them, because stations come from rates. A capability names the
  object kinds a server will fit (ours will exclude osculating lunar
  apsides, which swing degrees a day).
- **Deep sky** is one capability bit plus the catalogs list saying which
  catalogues a server resolves; ours is Messier only.
- **Provenance** is `<engine> | <ephemeris> | <model>`, and `datasetId` is
  `<engine>/<ephemeris>/<catalogs>#<8 hex>` over every data file's checksum
  and the engine version. A request can pin `datasetId` itself.
- **Rate tolerances:** 1e-5 deg/day for angles, 1e-6 AU/day for distances. A
  distance rate that omits the light-time term differs by the observer's
  acceleration times the light time (measured on Swiss: 3e-5 AU/day for
  Uranus), which is a definitional difference a server flags rather than a
  fault.
- **Registries** are generated to `ephsrv/registries.json` beside the header,
  checked against the codec's constants; we vendor both with checksums.
- **An interop harness** in Astrolog's tree runs a question set against both
  servers, records each side's answers for offline diffing, and attributes
  disagreements instead of averaging them. Classes where the models
  legitimately differ (topocentric Moon, heliocentric light time, star
  catalogues, mean-element models, distance rates) are report-only.

**Reading the fixtures.** `tools/check/ephproto4_fixtures.py` parses
Astrolog's conformance fixtures with a reader written from the spec text,
sharing no code with Astrolog's codec or with its fixture generator. A
disagreement between the two readings means the spec, one parser or one
fixture is wrong. Run it against the conformance directory in Astrolog's
tree; it reports the verdict (ok, malformed, unsupported) for every fixture
and exits nonzero on a disagreement.

**Exit status is exactly "did any assertion fire"** (2026-09-20), over six
named assertions, and three of them are not about the bytes. A manifest
listing no fixture printed "0/0 agree" and exited 0; a manifest with no
`# set-sha256` line printed a note and exited 0, so nothing said the
directory had been read whole; and a missing `JUDGEMENTS.tsv` skipped the
per-kind drop's section 2 in silence. An inconsistent set is still not a
finding — a directory read while their generator was writing it exits 2 with
no verdict reported, since a half-written set must not become an accusation.
`tools/check/fixturestest.py` grades all six against a set
`tools/check/fakefixtures.py` builds, and grades the two refusals on their
messages, which the case table cannot tell apart. Those built fixtures carry
no authority about the protocol. They are written from the vendored
`ephproto.h` and `registries.json` and their expected verdicts are what
*this* reader should say, so a misunderstanding the builder and the reader
share stays invisible; Astrolog's set remains the only thing that says
whether the reader reads the protocol right. Runs: 74/74 on the first drop, 85/85 on
the second, and — 2026-09-17, after the reader learned `deadlineMs`, the
batched LOOKUP, `u8 nQueries` and `corrApplied` (all its own staleness, not
the drop's bytes) — **91/91 on the corrApplied drop** (ephv4 `0fbc863`,
`set-sha256 1c934c7d…` verified independently). **§3 is locked** on that
verdict; the lock's prose carries two reading-rule recommendations (the
`maxPayload` zero-check, and unknown precision/column bits being
unsupported-not-malformed) that move no bytes. Astrolog's ack landed the
same day (ephv4 `cf83dc9`, all their gates green, the set's digest
unchanged): §3 is locked from both sides, their corrApplied now sends the
capability set whole instead of intersecting it with the request's mask,
and no further round is owed on §3 — the remaining work here is our
migration, below.

**The floats drop, 2026-09-18:** 105/105 at Astrolog `qt` `114f2a5`
(`set-sha256 cdbd7438…`). The independent reader was taught the DATA float
rule from the spec text: finite values, or a whole failed row of the
canonical NaN, which is `0x7FC00000` at f32. It had the same gap as the
codec until then.

**The per-kind drop, 2026-09-18:** 99/99 at ephv4 `472b21a`
(`set-sha256 d904e358…`, verified independently), with the reader taught
0x0014 from the drop text before the codec was read. The reader validates
what is inside that capability entry: count, layout, and correction bits
above 0x07 as malformed. Since 2026-09-18 it checks every capability
entry against its A.3 layout, written from `registries.json`'s payload
text. That covers all 20 tags across the set's 13 WELCOMEs. It is
stricter than the codec in two places no fixture decides yet: the segments
capability's three reserved bytes must be zero (§3.1), and the rates
bound's floats must be finite. A reader that stored capability payloads raw would have passed
the reserved-bit fixture; removing the check makes exactly that fixture
disagree. Request fixtures whose refusal depends on a server's WELCOME
are paired with one in `JUDGEMENTS.tsv`. The reader applies the drop's
section 2 to every row: 7/7, including a positive control, and it fails a
table that does not exercise both outcomes. A reader that refuses
everything fails there twice (fault-injected). `--judge REQUEST WELCOME`
judges any single pair. The previous pin (`0fbc863`) was one line older
than the §3 lock commit `cf83dc9`: the lock added `ValidateWelcome`'s
refusal of `maxPayload = 0`, the reading rule this side had recommended,
and nothing else differed. Our WELCOME's `maxPayload` is nonzero, so the
server was never out of step.

**Our migration** — complete. Steps 1–5 landed with the codec and the
session rewrite; steps 6–7 are SEGDATA, row-block compute, CANCEL and
priority (2026-09-17):
1. The v4 header and `registries.json` are vendored with checksums at
   `third_party/ephproto/v4/`.
2. The v4 codec runs the locked fixtures in `tests/test_ephproto4.cpp`.
3. The wire map and the v3 session are deleted.
4. Behaviour changes: ambiguous star names become a per-object error;
   no-parallax distance becomes 0 with the flag; a sidereal request on the
   equatorial plane is malformed; deflection is honoured for barycentric
   observers; per-object error text is rewritten to name no instant.
5. datasetId derives from the ephemeris, catalogs and star catalog, and the
   pins answer from the named snapshot only.
6. **Segments (SEGDATA)**: a `representation = 1` REQUEST is fitted on the
   lattice below, a cell at a time per object, with the ayanamsa of every
   sidereal profile as its own scalar series and the residuals the fitter
   measured. Measured motivation: a 10-body, 10,000-row hourly window is
   0.29 s of compute and 9.7 MB of f64 DATA, against about 40 KB per body as
   segments.
7. **Row-block compute, CANCEL and priority**: a samples REQUEST computes in
   blocks of rows (~2 ms a slice) across loop turns through the session
   owner, so a CANCEL that arrives is read before the rest of the work —
   dropping the stream drops the compute — and a cancelled request caches
   nothing (the samples cache is written only by a compute that reached its
   last row). Priority 0 answers are worked and sent before priority 1;
   within one request the chunks stay in order. `deadlineMs` is accepted and
   advisory: this server does not yet switch strategies on it.

## The segment lattice

`server/segcache.hpp` implements the shape agreed with Astrolog for version 4,
independent of the wire format; SEGDATA answers are served from it.

A client asks for segments over a span. The server does **not** fit that span.
It fits the fixed lattice cells covering it — 32 days, aligned from J2000 TT —
and returns those, so two clients asking overlapping spans share everything
but the ends. The client gets contiguous coverage of what it asked for plus a
little either side, which costs it nothing.

The requested error is quantised onto a ladder (1″, 0.1″, 0.01″, 0.001″) so
that two clients asking 0.1″ and 0.12″ are served by one fit. **Quantisation
only ever moves toward a finer fit.** A request below the last rung is refused
rather than served coarser: the client asked for a number because something
downstream depends on it, and the segment's published residual is a promise,
not a disclaimer. The floor is advertised, so no client has to learn it by
being refused.

Neither the lattice nor the ladder is client-chosen. A client able to name a
cell boundary, or to demand an exact error, would defeat the sharing without
meaning to.

Both work because the cost of an answer is dominated by the **time window** it
touches rather than by the objects in it: the frame work underneath — chiefly
the nutation nodes, one per half day — is computed once per window and shared
by every body, instant and client that touches it (docs/FRAMES.md). Two
clients scanning the same year therefore share the fits *and* what is beneath
them.

Cells are cached per event loop, LRU under a byte budget (`--seg-cache-mb`),
alongside the result cache. A segments request's work scales with its span,
which no size limit expresses, so the span is bounded in days — this is what
version 4 advertises as `maxSegSpanDays` (`--max-seg-span-days`, 1024 by
default, 32 cells).

The cache key is supplied by the caller: everything that identifies the
dataset, the object and the profile, with the rung and cell index appended as
raw bytes. `segcache.cpp` knows nothing about any wire format, and the fitter
under it knows nothing of the lattice (docs/SEGMENTS.md). The prefix the
server builds names the resolved object, the observer (and site, and
observing body), plane, frame, corrections and precession — everything that
shapes the fit — plus the ΔT a UT1 request converts by (the table's digest,
the one value's bits, or `M` for the model): a cell fitted under one ΔT never
serves another. The zodiac is deliberately absent, because coefficients are
tropical whatever the zodiac; the ayanamsa is a frame quantity, so its own
key names the zodiac (and a user anchor), frame and precession, and its
sampler always observes from the geocentre. Segments answers do not enter
the samples result cache: their sharing unit is the cell, whose key carries
the rung — a delivery-block field — so the two key spaces are kept apart.

## What the answer says it applied

An object's metadata carries `corrApplied`: the corrections this server's
engine actually applied to that object, as opposed to the ones the request
asked for. The byte sits after `metaFlags` in the per-object metadata,
three bits used and five reserved, and a client must ignore the high ones.
This server sends the structural table, pinned by unit checks in
`tests/test_server.cpp`.

It reports **structural** availability — the corrections the engine is able to
apply for this kind of object and this observer. A bit is clear when the
engine cannot apply that correction here at all. It stays set when the
correction's own model ran and contributed nothing, because a deflection term
that returns zero for a body far from the Sun has still been applied; the
alternative reading would make the field vary with the sky, and a per-object
field that answers a per-row question reads as stable without being so.

This replaces a per-kind mask on the correction capability. The narrowing that
motivated it does not fall along kinds: measured on Saturn at J2000, the
observer-velocity term is 0.0000" for an observer at the barycentre, 0.0107"
at the Sun, 10.4372" geocentric and 8.7706" centred on Jupiter. An engine that
forces it off is therefore exactly right in one case, harmless in another and
8.8" wrong in a third — a split by observer, not by kind, and one no capability
bit was going to express. Reporting it per object costs a byte and needs no
round trip.

**It is a diagnostic, not a gate.** `corrApplied` explains a difference; it
does not predict one, and equality of it is neither necessary nor sufficient
for two answers to agree. Both halves of that are live between this server and
the Astrolog one: our lunar orbit points agree to 0.0002" while reporting
different `corrApplied`, because what is one correction in one frame is a
cancelling pair in another; and our mean nodes differ by up to 0.025" with
identical `corrApplied`, because our mean-element fits are different fits. A
conformance harness must not filter comparisons on it.

Orbit points are the worked example, and `docs/ORBIT-POINTS.md` has the
measurements. The short version: corrections on an orbit point are
interoperable applied in full or not at all. A proper subset is well defined
only within one implementation, and a client must not compare such an answer
across servers.

### Checking that it is true

`tools/check/corrapplied.py` points at a running v4 server and asks whether
its `corrApplied` matches its own behaviour. Being a diagnostic, the field has
nothing to compare against but the server that sent it, so all three checks
are internal:

- **independence** — the byte is the same whatever the request's mask asked
  for. A server that echoes the request into the slot fails here and nowhere
  else, and echoing is the easy bug: this server did it until the v4 rewrite.
- **declared** — the byte is a subset of the correction bits WELCOME says can
  be honoured for that observer *and that object kind* (A.3 0x0004, as
  widened or narrowed per kind by 0x0014). Two things one server said,
  disagreeing. It read 0x0004 alone until 2026-09-20, which accused a server
  of claiming corrections it had declared in the other record.
- **truthful** — asking for one correction alone either moves the position or
  does not. A clear bit that moves the sky is a false denial. A set bit that
  moves nothing is correct and reported as a note, because that is what a term
  contributing nothing looks like: aberration at the barycentre measures
  ~1e-11″ here, and the model did run.

Only a clear movement (>1e-3″) accuses; anything smaller is a note. It is not
in the gate — it needs a live daemon, and the gate stays hermetic and fast —
so it is run against a server on purpose:

    ./build/prometheiad --ephemeris ephe/linux_p1550p2650.440 &
    python3 tools/check/corrapplied.py            # -v to show the notes

It asks only the masks the server's own WELCOME honours. A check a server
cannot be asked (a lone deflection at the Sun's centre, or anything without
light time on an engine that cannot switch light time off) is reported as
inapplicable, never as passed. **Checking nothing fails.** The run exits
non-zero if no case was checked, if fewer than half were (`--min-fraction`),
or if any of the three checks never ran. The first version printed OK after
checking zero cases against the Astrolog server: a green that could not have
been red, in the tool built to catch exactly that. Against this server it
checks 27 of 29 cases.

**Its own assertions are falsified by `tools/check/corrtest.py`** (2026-09-20),
the second tool here to name its own checks rather than have a list kept
beside it: `corrapplied.py --list-assertions` is the table, every run ends
with `assertions evaluated [...] fired [...]`, and the exit status is exactly
"did any assertion fire". Eight cases drive one scripted server each, and a
case passes only when the *named* assertion fires and no other — the clause
that separates a deleted check from a working one, since a neighbouring
assertion reds on the same input.

The fault goes in at the client, not the server. `corrapplied.py` reaches a
server only through `prometheia-wire-client` and sees only what `wirelib.py`
parses from its stdout, so `tools/check/fakewire.py` answers from a scenario
file: no daemon, no ephemeris, no socket, and a server that lies in exactly
one way at a time. That is what makes "this assertion and no other" a claim
worth making — a real server cannot be asked to echo the request into
`corrApplied` while staying honest about everything else.

What it cannot see, said rather than left to be assumed: nothing here grades
`wirelib.py` against a real client, and a drift in the client's printing has
to be mirrored in `fakewire.py` by hand. The compensating property is the
direction it rots — the regexes stop matching, every case loses its answers,
and the run fails on `nothing-checked` rather than passing quietly. The wire
itself is covered by the conformance fixtures and the cross-test.

Meta-falsified four ways, each failing exactly the cases it should: deleting
the independence judge (`did not fire: independence`, and `exit 0, wanted 1`,
because the exit status no longer counts anything on its own); making
`declared` fire unconditionally (five cases report `fired but should not
have`, including the control); adding an eighth assertion with no case (the
script refuses to start); and removing one the cases expect (`cases expect
assertions the tool does not declare`). **The fourth injection was a silent
no-op the first time** — the edit did not match, the run passed, and the pass
meant nothing. An injection that does not change the file is a green that
proves nothing, so assert the mutation before trusting its result.

It is written to run against any v4 server, not only this one, and it never
compares two servers against each other — §3.5a forbids gating on this field,
and the interesting failures are self-inconsistencies anyway. The wire client
grew `--corrections MASK` and `--center NAIF` for it, which are also the two
flags you want when probing a correction question by hand.

It is one item on a larger plan: [CROSS-TEST.md](CROSS-TEST.md) covers the
first live session against the Astrolog project's daemon — the four pairings,
which anchors adjudicate which disagreements, the tolerance tiers that say in
advance which differences are defects and which are model choices, and the
bisection ladder for when a number disagrees.

## Fuzzing

`tools/fuzz.sh [SECONDS]` runs three libFuzzer targets under ASan and UBSan.
It builds them with clang in `build-fuzz/` and runs each for SECONDS
(default 60). It is never part of the gate.
- **`fuzz_frame`**: any bytes into the protocol codec's whole-message parser
  (`eph::ParseFrame`, the vendored `ephproto.h`).
  - Besides the sanitizers, the check is the protocol's own: a frame the
    codec accepts must re-encode to exactly the input. §3.1 makes a
    receiver reject non-canonical input rather than normalise it.
- **`fuzz_session`**: a connection's messages into a fresh `Session`, on the
  synthetic kernel the server tests use, with small limits.
  - An optional valid HELLO comes first, so the fuzzer starts inside the
    session.
  - Every reply must parse as a v4 frame and re-encode to the same bytes,
    whatever the server was sent.
- **`fuzz_json`**: `prometheia-json`'s messages into its MCP dispatcher, on
  the same synthetic kernel, through the one parser every transport uses
  (`mcp::parse`).
  - A mode byte either sends the text as the whole message or wraps it as
    one tool's arguments, so the fuzzer reaches argument handling without
    having to discover the JSON-RPC envelope.
  - Besides the sanitizers: no exception escapes, every reply serialises,
    and every reply is a JSON-RPC 2.0 response (`"jsonrpc"`, a string,
    number or null `id`, exactly one of `result` and `error`).
  - Seeds are the examples in JSON_API.md; `fuzz/json.dict` holds the
    methods, argument names and vocabulary.

Corpora for the binary targets start from the protocol's conformance set,
read from the Astrolog tree (`$PROMETHEIA_ASTROLOG`). Both also get `fuzz/protocol.dict`:
the magic, the message types, canonical and non-canonical NaNs, the zodiac
tokens and some names. Those are byte strings blind mutation rarely
guesses, which the planted-fault run below shows is its weak spot. They grow in `fuzz-corpus/`, and failures land
in `fuzz-artifacts/`; both are gitignored. The fuzzers run with address-space
randomisation off (`setarch -R`). Under this kernel's randomisation, clang
14's sanitizer runtime spun forever at start-up in 5 of 12 starts.

**What it catches, measured by planting faults and reverting them:**
- An off-by-one read in the codec's string reader was found within seconds.
- A two-byte magic value checked at the start of a REQUEST was not found in
  1.2 million session inputs, with or without value profiling.

Byte-exact magic deep inside a structured message is the known weak spot of
blind fuzzing. The session target reaches REQUEST handling, and the seeds are
valid requests, but it should not be read as having explored every value of
every field.

**First runs, 2026-09-18** (10 minutes per target, after 1-minute runs):
- **`fuzz_session`:** 2.36 million inputs, 7,444 edges covered, no failure.
  Our session never crashed, leaked, hung, or sent a frame that failed to
  parse or re-encode. A further 30-minute run gave 4.46 million
  inputs and 8,264 edges, also with no failure.
- **`fuzz_frame`:** one finding, in the vendored codec, after 589,000 inputs.
  `ParseData` accepts any value in DATA: infinities, and NaNs with any bit
  pattern. §3.1 allows only finite values, plus the canonical quiet NaN
  where a field says so (a failed row). The fuzzer saw it as a round-trip
  failure: an f32 signalling NaN `0xFFA00000` parses, and re-encodes quieted
  as `0xFFE00000`. f64 NaNs survive a round trip bit for bit, so the same
  missing check there is invisible to this oracle. The spec also names no
  canonical NaN for f32 delivery; the codec's encoder writes `0x7FC00000`.
  Reported to the Astrolog side, whose codec it is, and fixed there (their
  `114f2a5`, re-pinned here).
- **The stricter codec then found one in this server** (2026-09-18, the
  first session input after the re-pin, an unmutated seed).
  - A topocentric site whose height put the observer thousands of AU below
    the ground came back with rowsOk 1 and −NaN coordinates, with rates of
    0 beside them. That is a non-canonical NaN and a half-failed row, and a
    "success" nobody could use.
  - The protocol bounds a site's latitude and longitude, not its height, so
    the request is legal.
  - The earlier claim that "it sends nothing non-canonical" was wrong: the
    encoder writes whatever double it is given.
  - Fixed in three places:
    - the engine refuses a site that is not finite or not above the
      Earth's centre;
    - the engine never reports a non-finite answer as a success (errCode 7
      on the wire);
    - the server fails a row whole, as the canonical NaN, if any value in
      it is not finite.
  - Regression tests replay the fuzzer's request.
  - The session fuzzer then ran 340,000 inputs against the fixed server
    and the stricter codec, with no failure.

**`fuzz_json`, 2026-09-18/19** (10 minutes, 8 workers, `-fork=8`):
- **Before the fixes:** 26.7 million inputs, three failures, and a fourth
  found by reading the transports it feeds:
  - an `id` that is an object was echoed in the reply, where JSON-RPC
    requires null (the one crash);
  - a topocentric site 4.4e19 m up: two timeouts. Light time from there
    runs centuries back, and a small body was integrated that far before
    the call failed. On the real catalogue it cost 30 s of CPU per body;
  - JSON nested 500,000 deep crashed `prometheia-json` with a stack
    overflow. The fuzzer's inputs are too short to reach that depth; a
    1 MiB body is not;
  - the HTTP log wrote the client's method and tool strings verbatim.
  Each is fixed (JSON_API.md, "Guardrails"), and each has a regression test
  that fails without its fix.
- **After:** 19.2 million inputs, 35,386 edges, no failure.

## Load and soak

`prometheia-load` opens `--conns N` connections to a server and sends
REQUESTs back to back for `--seconds S`. Each connection sends HELLO once,
then asks for a chart: the Sun, Moon, planets and Pluto at one instant, or
`--rows R` daily instants. Every request asks for a fresh random instant
between 1900 and 2100, so the result cache does not flatter the numbers.
`--cached` asks for the same instant every time.
- **What it reports:** throughput, latency percentiles, ERRORs by code, and
  upgrades refused with 503.
- **The canaries.** A run that counts arrivals cannot see a wrong answer, so
  one request in `--canary-every` (default 16) re-asks one of `--canaries`
  fixed instants (default 8) and grades every number against what the *same
  server* answered for that instant before the load started. A difference is
  a failure of the run, not a statistic, and so is grading nothing at all
  when canaries were asked for. Three things it checks:
  - every value, bit for bit, NaN counting as equal to NaN, with the worst
    `|difference|` printed so a last-bit difference and an answer for the
    wrong body are told apart;
  - the shape: objects, rows, columns present;
  - that each object came back as the NAIF id it was asked for, which needs
    no baseline — it is the answer disagreeing with its own request, which
    is what a contaminated answer looks like.
  - **This is consistency, not correctness.** The baseline is the server's
    own idle answer, so a server that is wrong the same way twice passes.
    Whether the numbers are right at all is the cross-test's question
    (CROSS-TEST.md). What this catches is contention changing an answer —
    a race, a corrupted cache, one request's answer delivered to another.
  - `--chunk-rows N` makes the load ask for chunks of N rows while the
    baseline keeps the server's own chunking, so one answer is graded
    against a differently chunked copy of itself. Answers are reassembled
    by `iTime` into a layout that does not depend on the chunking.
  - **Fault-injected four ways** (2026-09-20), each reddening only its own
    check: a canary instant moved by 1e-6 d reds all 874 of a run (worst
    1.53e-5°, on the Moon); asking a canary for 3 rows reds the shape;
    dropping `iTime` from the reassembly reds a `--chunk-rows 37` run
    against an unchunked baseline (the Moon's longitude 21.48° where
    316.13° was expected); and a clean run of each is green.
- **With `--pid`:** the server's resident memory and open files, sampled
  once a second, and again after every connection has closed.
- **Exit status:** it exits 1 on any other failure (a transport error, a
  message that does not parse, or a canary that differed).
- It runs from `tools/scheduled.sh`, never in the gate.

**Measured 2026-09-18** on this machine, against `prometheiad` with DE440
and `--threads 4`. The client ran on the same host.
- **Soak:** 64 connections for 300 s, with `--cells-per-sec 0` so the
  budget does not throttle.
  - 10.7 million requests answered, about 35,600 a second. Latency p50
    1.81 ms, p99 2.94 ms, max 25 ms. No errors and no failures.
  - Resident memory reached 368 MB within 30 s and stayed there to the end,
    as the result caches filled (64 MB per loop) and then recycled.
  - Open files peaked at 96 and went back to 31 after every connection
    closed. No growth and no leak over the run.
- **After the performance work of 2026-09-19** (64 connections, 20 s,
  `--threads 4`, `--cells-per-sec 0`, a fresh random instant a request):
  48,300–49,300 requests a second, p50 1.27–1.29 ms, p99 2.05 ms. The
  same run gave
  31,900, p50 1.91 ms and p99 4.63 ms before it. Nutation had been 43% of
  the server's CPU (ENGINE.md, "Performance").
- **With the canaries, 2026-09-20**, same machine, `--threads 4`,
  `--cells-per-sec 0`, a freshly started server, 64 connections for 20 s:
  **42,066 requests a second**, p50 1.48 ms, p99 2.80 ms, **52,646 canary
  answers graded and none differed**. Back to back on the same server, the
  same run with `--canaries 0` gave 40,015 a second against 41,770 with
  them: the check costs nothing measurable, because a canary is a cache
  hit. These are below the 48,300–49,300 above because the machine was not
  idle — a second ephemeris daemon and a compiler were running — not
  because of the canaries.
  - A chunked run, 400 rows a request in 37-row chunks, 8 connections for
    8 s: 89 canary answers graded, none differed. Each is 24,000 doubles
    over 11 chunks.
- **The memory bound, 2026-09-20 (`--memory-bound MB`).** Nothing in this
  project asserted anything about memory until now: the 369 MB plateau below
  was observed once and written down, and a result cache that stopped
  evicting would have passed every check we have. The Astrolog side found the
  same hole on their side the same day and gated it; this is ours.
  - **Two assertions, or the run refuses to start.** Resident memory must
    grow by no more than MB, *and* the cache must really be filling
    (`prometheiad_cache_hits_total` from `/metrics` must rise). A bound on
    its own is passed by a server that caches nothing at all, the same way a
    row grading two observers as equal is passed by two observers that never
    arrived. `--memory-bound` without `--pid`, or against a server whose
    `/metrics` is unreadable, exits 2 **before any load is applied** rather
    than asserting half the check and reporting a pass.
  - RSS is sampled once *before* the load starts, not a second into it: a
    small `--cache-mb` can be full within the first second, and growth
    measured from there is growth already missed.
  - **Measured, `--threads 1 --cells-per-sec 0`, 8 connections:**

        --cache-mb 8    grew 11.3 MB   flat at 20.8 MB from t=5s to t=60s,
                                       over 818,169 requests
        --cache-mb 64   grew 89.3 MB
        --cache-mb 0    grew  0.1 MB   and cache hits rose by 0

    Growth is **about 1.4x the cap** at both points — the cache accounts
    payload bytes, and per-entry and allocator overhead is the rest — and it
    plateaus. That also explains the 369 MB below, which had never been
    connected to the cap: 64 MB a loop x 4 loops x 1.4 is 358.
  - **It measures growth during the run, so it says nothing about a server
    that is already warm.** A cache at its plateau grows by nothing and
    passes any bound. Point `--memory-bound` at a freshly started daemon, or
    it reports a pass having measured nothing. This was not noticed when the
    check was written; `loadselftest.py` found it on its first run, because
    reusing one daemon across cases made "the bound is exceeded" pass with
    the bound set to 1 MB.
  - **Sabotage-proven five ways**, each reddening only its own check: the
    bound set below the known growth (OVER, exit 1); `--cache-mb 0`, where
    the bound passes at 0.1 MB and only the pairing catches it (NOT CACHING,
    exit 1); `--memory-bound` without `--pid` (exit 2); against a server with
    no `prometheiad` `/metrics` (exit 2, no load applied); and the clean run
    green.
  - Cache hits rose by exactly the number of canary answers graded (22,390 of
    22,390; 51,139 of 51,139), which is its own small confirmation: the
    canaries are the only repeated instants, and a fresh random instant
    always misses.
- **The selftest, 2026-09-20 (`tools/check/loadselftest.py`).** Every fault
  injection in this document was done by hand, once, and written up as prose;
  nothing re-ran any of them. A falsification recorded as prose is a
  measurement taken once. The script breaks one thing at a time and requires
  **the right assertion** to catch it — a case passes only when the named
  assertion fires and no other one does.
  - That last clause is the whole check. A selftest asking only "did the run
    go red" passes a gate whose bound has been deleted, because some other
    assertion reds on the same broken input and the selftest cannot tell
    which. The Astrolog side found exactly that in their own selftest the
    same day, by weakening a bound to a number nothing could exceed.
  - Ten cases. **Seven need no sabotage hook**, being induced by how the
    server is started and how the tool is invoked — `--cache-mb 0`, a bound
    below the real growth, no `--pid`, a server whose `/metrics` lacks the
    counter, a compute budget the baseline spends exactly, a server that is
    not `prometheiad` at all, and the clean control. The three canary cases
    use `prometheia-load --sabotage values|shape|identity`, which damages the
    client's own copy of an answer beside the grading it falsifies, inside
    the binary that ships rather than in a copy of the check.
  - **It began as the same overclaim it exists to catch.** The first version
    declared nine assertions and exercised seven: `canary-none-graded` and
    `failures` had no case, so either could have been deleted from
    `prometheia-load` and every case would still have passed, while the
    docstring said it falsified *every* assertion the tool makes. Found by
    reading the case table against the assertion table rather than by running
    anything — a count, not a measurement.
  - **The list now comes out of the binary, because the first fix left the
    half that mattered.** Refusing to run when the table and the cases
    disagree catches an assertion with no case; it cannot catch an assertion
    added to `prometheia-load` and to neither list, since both lists were in
    Python. So the tool names its own: every check is an entry in `A`, judged
    through `Assertions::judge`, and `--list-assertions` prints the table
    while each run ends with `assertions evaluated [...] fired [...]`. The
    selftest reads names instead of matching patterns — a pattern rots in the
    worst direction, where it stops matching, the derived list shrinks and
    everything goes green. (The Astrolog side raised that objection against
    their own message-grepping version the same afternoon, and moved to
    recording each assertion as it evaluates.)
  - **Two cases that red identically are one case**, and the table does not
    say so — delete either and everything still passes. The script refuses
    a table where two cases assert the same set at the same exit status.
    Measured, and the answer was that the eight already differed; the first
    attempt read the file with a regex, silently saw seven of eight, and
    would have reported the same "no duplicates" either way. The check is
    on the real list for that reason.
  - **The exit status is now exactly "did any assertion fire".** It used to
    count `st.failures`, a differing canary, an ungraded canary and the
    memory verdict a second time, on its own — which is how deleting the
    silence guard left the run red on the right input for the wrong reason.
    A condition counted twice lets a deleted check stay invisible. With the
    duplicate gone, deleting that judge reports `exit 0, wanted 1` as well as
    `did not fire`: a hole found by falsification and closed in the thing
    falsified, not in the falsifier.
  - **Falsified against itself**, each fault failing exactly one case, by
    name. Deleting the bound assertion: "did not fire: memory-over".
    Reporting the failure count as zero: only the sham-server case fails.
    Making the cache check fire unconditionally fails the two cases it should
    not have fired in, one of which was *otherwise reddening correctly on its
    own assertion*. Adding a tenth assertion to the binary with no case stops
    the script before it starts a daemon. Deleting the silence guard fails
    only its own case — and now fails it twice over, on the assertion and on
    the exit status, which is the fix described above.
    - This paragraph is the one layer that stays prose: falsifying the
      falsifier means patching and rebuilding the binary, which nothing here
      re-runs. The case table below it is the part that does.
  - It starts its own daemon per case and stops only what it started. It
    needs an ephemeris, so it is not in `tools/gate.sh`, which must stay
    seconds. `tools/scheduled.sh` runs it (below).
  - **It covers one tool.** `corrapplied.py` has had the same treatment
    since (`corrtest.py`, above) and `crosstest.py`'s adjudicators theirs.
    `ratesweep.py` has it too (`ratestest.py`), and so does
    `ephproto4_registries.py` (`registriestest.py`); both run in the gate.
    `stars_fk5.py` has `starstest.py`, which mutates a copy of `stars-raw/`
    and answers from a scripted client, so it needs no daemon but does need
    pyerfa and the catalogue: `tools/scheduled.sh`, not the gate.
    `ephproto4_fixtures.py` has `fixturestest.py`, which runs in the gate
    although the tool it grades cannot: the reader needs Astrolog's
    conformance directory, its assertions need only a set that
    `fakefixtures.py` builds. `crossrun.py` has `crossruntest.py`, which drives it
    with scripted steps and two sockets in place of the daemons.
    Named rather than counted, because a number here drifts and a list does
    not: what still carries its falsification as prose is `crosstest.py`'s
    legs — as opposed to its adjudicators — and nothing else under
    `tools/check/`.
- **The same tool against another implementation, 2026-09-20.** Pointed at
  `astrolog-ephd` with the Astrolog session's consent (CROSS-TEST.md,
  "Pointed at `astrolog-ephd`"): 2,463 canary answers graded, none
  differed, and their compute budget produced the same arithmetic as the
  limits run below — 19,999 ten-cell requests in the first ten seconds,
  then 1,000 a second. Nothing in `prometheia-load` is ours-only; it
  speaks v4 and takes `--host`/`--port`.
- **Limits, at their defaults:** 100 connections from one address for 10 s.
  - 36 were refused at the upgrade with 503, which is the 64-per-address
    cap.
  - The compute budget of 10,000 cells a second held to the request:
    exactly 20,000 ten-cell requests were answered. That is the 100,000-cell
    bucket's 10,000, then 1,000 a second.
  - Everything else, 559,116 requests, was answered ERROR 6. The client
    waited out each ERROR's `retryAfterMs` before asking again.
  - No failures, and memory stayed at 55 MB.
- **A large answer to a reader that stops** (the stall the Astrolog side
  found in their own server, 2026-09-18). A connection's replies pause once
  4 MB sit unsent, and resume from uWS's drain callback. The pump stops
  rescheduling once no session has compute left, so drain is the only
  resume path.
  - It cannot strand an answer: the pause needs uWS itself to hold 4 MB
    unsent, and uWS writes that out on the socket's next writable event,
    which calls drain.
  - Measured: a 61 MB answer (64 objects × 20,000 rows) to a
    `prometheia-wire-client` frozen with SIGSTOP for 12 s from its first
    second. The server hit backpressure three times, and every one of the
    1,280,000 rows arrived within 3 s of the client resuming.

## The checks the gate cannot run

`tools/gate.sh` runs before every commit and has to stay hermetic and fast:
no daemon, no ephemeris, seconds. Everything that needs one of those has
been run by hand since it was written, which means it ran when somebody
remembered it. `tools/scheduled.sh` is the list, in one command:

```
tools/scheduled.sh                  the checks that need only our own tree
tools/scheduled.sh --with-cross     also the cross-test (needs their daemon)
```

In order: `tools/gate.sh` first, because a soak failure reported against a
tree that does not compile has told nobody anything; then `corrtest.py`, which
needs no daemon but spends fourteen seconds where the gate spends two
(`ratestest.py` makes the same promise for `ratesweep.py` in under two, so
it runs in the gate itself); then `loadselftest.py`; then a 60 s soak and the memory bound
against a daemon it starts and stops itself.

Every run writes `build/scheduled/<stamp>.log` and prints one summary; the
exit status is the number of steps that failed. Two properties are load
bearing, and both were injected before being believed:

- **A skip is only ever a missing precondition.** The first version started
  the daemon with `--log-level warn`, which that binary does not accept; it
  exited, `/healthz` never answered, and the soak reported SKIP. A skip
  whose precondition was met is a green that could not have been red. Once
  the binary and the ephemeris are both present, a daemon that does not come
  up is a failure.
- **The port must be empty before it starts.** `/healthz` on a busy port is
  answered by whoever is already there, and the soak would then drive that
  server while `--pid` measured this script's corpse — rows from one
  process, memory from another, and a pass. Checking "is our pid still
  alive" after the wait loop does *not* catch it, because the loop breaks on
  the other server's instant answer before the child has exited; that
  version was written here and the injection went green. Verified 2026-09-20
  by pointing it at the port of a running daemon: FAILED, and at a free one:
  4 steps, 0 failed.

`--with-cross` is opt-in on purpose: the cross-test reads another project's
server, and a thing that reaches outside this tree should be asked for
rather than happening because a default said so.

There is no hosted CI and there will not be. `tools/systemd/` holds a
user-level service and timer for whoever wants this daily; **nothing in this
repository installs them**, the `.service` file carries the six commands
that do, and `--with-cross` is deliberately absent from its `ExecStart`.

## Not implemented

- **zstd payloads.** Reserved in the envelope, advertised by no one, and
  declined by the maintainer (2026-09-18) on measurement: a 288,357-byte f64
  DATA chunk (12 bodies × 1000 days, all answering) came to 96.8% at zstd
  levels 1–10 and 95.5% at 19, and the fast levels stored it uncompressed.
  Packed ephemeris values do not compress; f32 halves them.
- **`deadlineMs` as a strategy switch.** The field is parsed and advisory;
  this server does not yet choose a cheaper strategy (samples rather than a
  fit) to meet one.
- **TDB's own timescale machinery**: TDB instants convert to TT through the
  Fairhead-Bretagnon series (a few ns against its own ~10 us; TIME.md).

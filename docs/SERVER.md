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
  conformance fixtures through the vendored codec (91/91, digest-pinned),
  and `tools/check/ephproto4_registries.py` (in `tools/gate.sh`) checks the
  registries against the header by name.
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
  - `--cache-mb N`: the result cache per loop.
  - `--verbose`: log each connection.
  - Limits, tokens, TLS and draining: see Operations below.
- **Dataset identity.** At startup the daemon digests every data file's
  contents and prints the dataset id it will serve
  (`<engine>/<ephemeris>/<catalogs>#<8 hex>`). The id changes whenever any
  answer could change; clients key their caches on it and may pin it.
- **Reference client.** `prometheia-wire-client --port 47190 --obj 599 --jd
  2461300.5 --count 3` sends HELLO and one REQUEST, and prints each object's
  metadata and one line per row. `--obj` takes a NAIF/SPK-ID, `--name` a
  catalog designation, `--star` a star, `--node N.M` an orbit point;
  `--helio/--bary/--eq/--j2000/--icrs/--sid/--topo` shape the profile.
  `--token`, `--tls`, `--ca`, `--sni` and `--insecure` cover the options
  below; `--help` lists the rest.

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
  bound), the caps bits (f32, instant lists, lookup, deep sky), the
  capability TLVs (kinds, observers, planes/forms/frames, per-observer
  correction masks, orbit points and methods, extra columns, the zodiacs
  this engine serves, sidereal planes, time scales, the delta T model, the
  lookup budget, the deep-sky catalogues) and the dataset id.
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
(`fagan-bradley`, `lahiri`, `user`), and the observers, planes, forms,
frames and corrections are typed profile fields. What the cleanroom server
does with them:

- **Profiles** resolve to the engine's `CalcOptions` once per request
  (session.cpp). Every object names its profile, so one request can mix a
  geocentric apparent chart with a heliocentric rectangular one. A profile
  this engine cannot serve (a zodiac it does not implement, a sidereal
  plane other than the ecliptic of date) refuses the whole REQUEST with
  ERROR 11; a body the data does not carry fails alone.
- **Times.** Rows are in the request's time scale (UT1, TT, TDB). The
  delta T comes from the request's table TLV (piecewise linear, TT
  instants), its one value, or — the canonical NaN — the engine's observed
  USNO model, which WELCOME names. The delta T column reports the value
  used.
- **corrApplied** reports structural availability per object, never the
  request's mask (below). Our table: bodies and orbit points carry light
  time and aberration always; deflection everywhere except an observer at
  the Sun (this engine cannot bend the Sun's own light, and a barycentric
  observer is 0.005 AU from the Sun, so it is deflected); stars carry
  deflection and aberration but no light time (catalog positions are
  directions of arrival).
- **Orbit points** (kind 1) answer by the engine's `calc_orbit_point`
  (docs/ENGINE.md), points 0-3 by methods mean and osculating. The Sun and
  the barycentre have no orbit (per-object error 2); an undefined point
  (a node of an orbit in the ecliptic, an apsis of a circle) is error 5.
- **Fixed stars** (kind 2) resolve through the compiled-in catalog; an
  ambiguous name ("Beta Sco") is a per-object error 6 rather than a silent
  choice. A star without a parallax answers distance 0 with the
  `noDistance` flag. Deep-sky designations resolve for the Messier
  catalogue, which the caps name.
- **Designations** (kind 5) resolve exactly as a LOOKUP of quality 0 or 1
  through the catalogs' name index; no match is per-object error 1.
- **Speeds** off (profile `speeds` 0): the three rate columns are zero and
  META carries `noSpeeds`.
- **Pins.** REQUEST TLVs 0x8001-0x8003 name the ephemeris, catalog or
  dataset the answer must come from; anything else is ERROR 5.
- **Segments** (representation 1), CANCEL and priority ordering are not
  served yet and not advertised; a segments REQUEST gets ERROR 11 until
  they land.

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
  key is the dataset id plus the question block's bytes, exactly as
  received: the delivery block (precision, chunking, deadline, priority) is
  outside it, so the same question asked for f32 in small chunks is a hit
  on the f64 answer.
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
- limits and errors: objects/rows/cells/profiles over the bounds, busy,
  rate limited, the pins, segments refused while dark;
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
- **Orbit points** are geometric, on the ecliptic of the profile's frame,
  heliocentric for planets and geocentric for the Moon. A mean model must
  name itself in the object's source string.
- **Fixed stars** have a normative name grammar (IAU name, Bayer with
  component numbers, Flamsteed, HR/HD/HIP). An ambiguous name is a per-object
  error rather than a silent choice, and an object without a parallax
  reports distance 0 with a flag.
- **Corrections** are honoured as sent for every observer, with the
  per-observer masks a server advertises.
- **Frames** are pinned: the ecliptic of date takes longitudes from the true
  equinox on the mean ecliptic; J2000 includes frame bias; ICRF does not; both
  use the IAU 2006 J2000 mean obliquity. Sites are WGS84 geodetic.
- **Privacy:** neither ERROR text nor per-object error text may name an
  instant or a place.

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
and exits nonzero on a disagreement. Runs: 74/74 on the first drop, 85/85 on
the second, and — 2026-09-18, after the reader learned `deadlineMs`, the
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

**Our migration** (the pass has landed and the fixtures are pinned; step 1
done — the v4 header and `registries.json` are vendored with checksums at
`third_party/ephproto/v4/`, the v3 header stays until the delete step):
1. Take the renamed `ephproto.h` and the fixtures; the pinned-header check
   diverges once, as expected.
2. Implement the v4 codec and run the fixtures in `test_server`.
3. Delete the wire map and the v3 session.
4. Behaviour changes: ambiguous star names become a per-object error;
   no-parallax distance becomes 0 with the flag; a sidereal request on the
   equatorial plane is malformed; deflection is honoured for barycentric
   observers; per-object error text is rewritten to name no instant.
5. Advertise the star catalog as a pinnable catalog id, and derive datasetId
   from the ephemeris, catalogs and star catalog.
6. Segments (SEGDATA) are ours to implement first; the engine already
   evaluates Chebyshev series. Measured motivation: a 10-body, 10,000-row
   hourly window is 0.29 s of compute and 9.7 MB of f64 DATA, against about
   40 KB per body as segments.
7. Priority ordering and CANCEL, which our v3 server does not have. CANCEL
   must stop the computation, not just drop bytes, so a request computes in
   row blocks that check for cancellation between them; a cancelled request
   caches nothing.

## The segment lattice

`server/segcache.hpp` implements the shape agreed with Astrolog for version 4,
ahead of the wire format and independent of it.

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

Cells are cached per event loop, LRU under a byte budget, alongside the
result cache. A segments request's work scales with its span, which no size
limit expresses, so the span is bounded in cells (`max_cells`) — this is what
version 4 advertises as `maxSegSpanDays`.

The cache key is supplied by the caller: everything that identifies the
dataset, the object and the profile, with the rung and cell index appended as
raw bytes. `segcache.cpp` knows nothing about any wire format, and the fitter
under it knows nothing about the lattice (docs/SEGMENTS.md).

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

## Not implemented

- **zstd payloads.** Reserved in the envelope, advertised by no one.
- **Segments (SEGDATA), CANCEL and priority** — migration steps 6 and 7
  above; dark in WELCOME until they land.
- **Hypothetical bodies and polynomial elements** (kinds 3 and 4): not
  served, not advertised (per-object error 2). The engine has no models
  for them; a client that needs a body from particular elements will find
  them in a later version of this server.
- **TDB's own timescale machinery**: TDB instants convert to TT through the
  Fairhead-Bretagnon series (a few ns against its own ~10 us; TIME.md).

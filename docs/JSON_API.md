# JSON and MCP interfaces

**Status, 2026-09-18:** implemented as `prometheia-json`, over the tools in
`server/json_tools.hpp` and the MCP dispatcher in `server/mcp.hpp`. The
shared vocabulary ("Words") is still being agreed with the Astrolog project.

**What Prometheia is:** a high-speed ephemeris. Its interfaces, fastest first:
1. **The C++ library**, in process (`prometheia::Engine`, ENGINE.md). This is
   the primary interface.
2. **The C API** over the same engine (C_API.md).
3. **`prometheiad`'s binary protocol** (SERVER.md), for high-speed data over
   the network.
4. **The `ephem` CLI** (EPHEM.md).
5. **JSON and MCP**, this document: convenience surfaces, mainly for AI
   agents.
   - One separate program, `prometheia-json`, serves both.
   - Plain JSON over HTTP (`/v1/…`).
   - MCP (the Model Context Protocol) over stdio, or streamable HTTP at
     `/mcp`.
   - It never runs inside `prometheiad`, so nothing here touches the fast
     paths above. It is sized for the questions an agent asks, not for bulk
     data.

This is the Prometheia side of the joint AI-forward plan with the Astrolog
project (docs/HANDOFF.md). The shared vocabulary in "Words" is the part the
two projects must agree on; the rest is Prometheia's.

## Why

An AI agent, whether an LLM with tools or an MCP server, should be able to
ask for positions and use the answer without guessing:
- what the numbers are;
- which frame and zodiac they are in;
- what corrections were applied;
- how accurate they are;
- why a request was refused.

The binary protocol (SERVER.md) carries all of that, but as packed fields and
registry numbers. This API says the same things in JSON, in words.

## Principle: one meaning, two encodings

The MCP tools resolve objects with the server's own code
(`server/objects.cpp`: names, stars, designations, hypotheticals, orbit
points), map options onto the same `CalcOptions`, and ask the same engine. The
binary protocol and the agent tools cannot disagree about what a question
means, and the cross-test can check that they do not. The tools are sized
for questions an agent asks, one chart or a short series. Bulk data belongs
on the binary protocol.

## Running it

```sh
# MCP over stdio: what an MCP client launches (Claude Code, Claude Desktop...)
prometheia-json --ephemeris ephe/linux_p1550p2650.440 --stdio

# MCP over streamable HTTP at /mcp, and plain JSON at /v1/<tool>
prometheia-json --ephemeris ephe/linux_p1550p2650.440 --http 47290
curl -d '{"time":"1990-06-15T14:30:00+02:00","objects":["Sun","Moon","true node"]}' \
     http://127.0.0.1:47290/v1/positions
```

To register it with Claude Code:
`claude mcp add prometheia -- /path/to/prometheia-json --ephemeris /path/to/linux_p1550p2650.440 --stdio`.
The `--catalog`, `--perturbers` and `--hypotheticals` options are those of
`prometheiad`, and the dataset identity is computed the same way.

**Streamable HTTP, as served:**
- **`POST /mcp`:** one JSON-RPC message or a batch. Requests are answered
  with `application/json`; notifications and responses only with 202.
- **`GET` and `DELETE /mcp`:** 405. The server sends no messages of its own,
  and keeps no session.
- **`MCP-Protocol-Version`:** checked when sent. An unsupported one is 400.
- **Revisions spoken:** 2025-06-18, 2025-03-26 and 2024-11-05, negotiated at
  `initialize`.

**Plain JSON:**
- `POST /v1/<tool>` takes the tool's arguments as the body.
- `GET /v1/tools` lists the tools with their schemas.
- `GET /llms.txt` is the agent-facing summary.
- `GET /healthz` answers ok.

**Security defaults:**
- It binds 127.0.0.1.
- An `Origin` other than localhost is 403 unless `--allow-origin` names it
  (against DNS rebinding).
- `--tokens FILE` requires `Authorization: Bearer`.
- Bodies over 1 MiB are refused.
- The log (stderr) names the method and tool, never the arguments.

**Measured, 2026-09-18:** the same question over MCP and over `/v1` gives the
same number. A `positions` call for one planet takes about 0.15 ms round
trip over local HTTP (curl, loopback). That is for agents and scripts; bulk
data goes through the library or `prometheiad`.

## Tools (`prometheia-json`)

Each tool is one call, whether it arrives as an MCP `tools/call` or as a JSON
`POST /v1/<tool>`. The request and answer are the same JSON either way.

| tool | what |
|---|---|
| `positions` | positions of objects at an instant or a short series |
| `lookup` | resolve a name to the objects it could mean (`prefix` matches a half-remembered one) |
| `capabilities` | what this engine answers: bodies, zodiacs, frames, coverage, in words, and whether a small-body catalog is loaded |
| `convert_time` | UTC, TT, UT1 and Julian dates, with ΔT and leap seconds |

The agent-facing summary of what to ask, and how, is the MCP resource
`prometheia://llms.txt`, and `GET /llms.txt` over HTTP.

## A request

```json
{
  "time": "1990-06-15T14:30:00+02:00",
  "observer": "topocentric",
  "site": {"lon_deg": 8.55, "lat_deg": 47.37, "height_m": 500},
  "zodiac": "lahiri",
  "objects": ["Mars", {"point": "ascending-node", "of": "Moon", "method": "mean"}, "Spica"]
}
```

- **Time:**
  - `time` is an ISO 8601 clock time (with an offset or `Z`), or
    `{"utc": …}`, `{"jd_tt": …}` or `{"jd_ut1": …}`.
  - **A clock time without an offset or `Z` is refused**, wherever a time
    is read (`time`, `times`, a `series` start, `convert_time`). It was
    silently read as UTC until 2026-09-20, which is the one wrong answer
    this surface could give without saying anything: an agent holding
    "14:30 in Zurich" and forgetting the offset got a chart two hours out
    and a reply that called it UTC. A bare date is still 00:00 UTC — that
    is a date, not a clock time.
  - `times` is a list of those.
  - `series` is `{"start", "step_days", "count"}`.
  - UTC goes through the leap-second table and the engine's ΔT
    (`convert_time` shows both).
  - **Before 1972 a clock time is read as UT1**, since UTC with integer
    leap seconds begins then, and before it civil time kept UT to under a
    second (from 1961). So a birth chart can be asked by clock time in any
    year. A reply names the scale: each row's `time` is `{"jd_tt", "utc"}`
    from 1972 and `{"jd_tt", "ut1"}` before it, as does `convert_time`.
    A leap second (`:60`) before 1972 is refused (maintainer, 2026-09-19).
- **Defaults** are what a chart wants: apparent (all three corrections),
  geocentric, the true ecliptic of date, tropical, rates on. Each can be
  overridden: `observer`, `frame`, `coordinates`, `corrections`, `zodiac`,
  `sidereal_plane`, `precession`, `rates`.
- **Objects by name**, not NAIF numbers.
  - Bare names resolve in a fixed order: planet, the Moon's points ("true
    node", "Lilith"…), hypothetical, star, catalog body.
  - A name that is unknown is a per-object error that says so, never a
    silent guess. When the name could only have been a catalog body and
    **this server was given no catalog**, the error says that instead, and
    `capabilities` carries the same fact under `asteroids`
    (`{"loaded", "catalogs", "note"}`): a deployment without a catalog
    answers no asteroid name, and an agent that cannot tell that from a
    misspelling will keep trying spellings.
  - `{"body"|"star"|"asteroid"|"hypothetical"|"naif": …}` and
    `{"point", "of", "method"}` say exactly which.
- **An argument a tool does not read is refused by name.** `positions`
  reads `time`/`times`/`series`, `objects`, `observer` (`site`, `center`),
  `frame`, `coordinates`, `corrections`, `zodiac`, `sidereal_plane`,
  `precession` and `rates`; `lookup` reads `query` and `prefix`;
  `convert_time` reads `time`; `capabilities` takes none. Anything else is
  `invalid-arguments` naming the key. An agent asking for `houses` or
  `aspects` — neither of which this engine serves — used to get positions
  back and no hint that half its request had gone nowhere, and a mistyped
  `prefix` on `lookup` quietly matched exactly and answered nothing, which
  reads as "no such name". Same rule as a name: never a silent guess.
- **`lookup` and a half-remembered name.** `prefix: true` matches a star by
  the start of its name, and a planet, lunar point or hypothetical body by
  the start of **any word** in its name: `node` finds `true node` and
  `mean node`, `apogee` finds `natural apogee`, `Lili` finds both `lilith`
  and the star Lilii Borea. The distinguishing word of a point's name is
  usually last, so an anchored match would answer `true` and never `node`.
  Without the flag every kind is exact. A catalog body is always exact —
  the catalog index resolves a name, it does not enumerate.

## An answer

The answer to the request above, as served (the first result only, the
digits cut):

```json
{
  "engine": "Prometheia 0.7.0, JPL DE440 binary",
  "dataset": "Prometheia 0.7.0, JPL DE440 binary/linux_p1550p2650.440/-#7ddcde20",
  "results": [
    {
      "object": {"asked": "Mars", "kind": "body", "naif": 4, "resolved": "Mars"},
      "rows": [{"time": {"jd_tt": 2448058.0214952, "utc": "1990-06-15T12:30:00.000Z"},
                "longitude_deg": 347.3272534, "latitude_deg": -1.9861654,
                "distance_au": 1.2789454, "light_time_days": 0.0073866,
                "ayanamsa_deg": 23.7272979,
                "rates": {"longitude_deg_per_day": 0.7182236,
                          "latitude_deg_per_day": -0.0057857,
                          "distance_au_per_day": -0.0053913}}],
      "provenance": {
        "source": "JPL DE440 binary",
        "observer": "topocentric",
        "site": {"lon_deg": 8.55, "lat_deg": 47.37, "height_m": 500},
        "corrections": ["light-time", "gravitational-deflection", "aberration"],
        "frame": "true equator/ecliptic and equinox of date",
        "coordinates": "ecliptic",
        "zodiac": {"token": "lahiri", "plane": "date",
                   "doc": "docs/FRAMES.md (zodiacs) and docs/ENGINE.md (ayanamshas)"},
        "precession": "iau2006",
        "accuracy": {"statement": "JPL planetary ephemeris; positions agree with JPL Horizons to 6 µas",
                     "doc": "docs/VALIDATION.md"}
      },
      "error": null
    }
  ]
}
```

- **Every number is named with its unit.** No positional columns.
- **Provenance is per object:** the source, the observer, the corrections
  actually applied, the frame, the zodiac and the precession model.
  - The rule: *an argument that moved the answer is named in the answer.*
    Three were missing until 2026-09-20 — the observer, the sidereal plane
    (1.02° of latitude between `date` and `invariable`) and the precession
    model (6 mas at 1600) — so answers that differ carried provenance that
    did not.
  - `observer` is the word that was asked for, with `site` (topocentric) or
    `center` (`{"naif"}`, a body observer) beside it. An answer travels —
    to Astrolog, to a file, to another agent — and there it no longer has
    the request that made it.
  - `zodiac.plane` is the plane the zodiac is counted along (`date`,
    `anchor`, `invariable`), on every sidereal answer.
  - `precession` appears where it entered the answer: an of-date frame or a
    sidereal zodiac. A tropical answer in ICRF or J2000 axes is the same
    number under either model, and naming one there would claim a
    dependence the answer does not have.
  - The accuracy statement is a measured number with the document that
    measured it, never a promise.
- **Errors** come in two kinds.
  - A per-object error has a code (`unknown-name`, `ambiguous-name`,
    `outside-coverage`,
    `unsupported`, `numerical-failure`, `data-unavailable`) and a sentence.
    The other objects are still answered.
  - A whole-call error (`invalid-arguments`) is MCP's `isError` result, or
    HTTP 400 on `/v1`.

## Words (shared with Astrolog)

One vocabulary for both projects. The rule: the protocol v4 registry
(`third_party/ephproto/v4/registries.json`) is the authority, and a JSON
token is its name in lower case, hyphenated. Checked entry by entry by the
Astrolog session, 2026-09-18. It matters because the agent names what it got
from here when it asks Astrolog to draw it.
- **Bodies:** Sun, Moon, Mercury … Pluto, and the A.15 hypothetical tokens.
- **Zodiacs:** the A.11 tokens.
  - `capabilities` gives each zodiac's `sidereal_planes`.
  - A zodiac defined at the instant (true-citra, the galactic ones) has no
    anchor epoch, so no `anchor` plane (§3.5a).
- **Points (A.13):** `ascending-node`, `descending-node`, `perihelion`,
  `aphelion` of a body. For the Moon they read as perigee and apogee.
- **Orbit methods (A.14):** `mean`, `osculating`, `interpolated`,
  `osculating-barycentric`, `focal-point`.
  - This engine computes the first two, and `interpolated` for the Moon's
    apogee and perigee (the natural apsides; by name, "natural apogee" or
    "natural Lilith", and "natural perigee" or "Priapus").
  - The others are refused as `unsupported`, never answered with another
    method.
- **Frames:** `true-of-date`, `mean-of-date`, `j2000`, `icrf`.
- **Sidereal planes (A.8):** `date`, `anchor`, `invariable`.
- **Corrections (A.7):** `light-time`, `gravitational-deflection`,
  `aberration`, with the shorthands `apparent`, `astrometric` and
  `geometric`. `deflection` is still read, and never written.
- **Observers (A.5):** `geocentric`, `topocentric` (with `site`),
  `heliocentric`, `barycentric`, `body` (with `center`: a name or NAIF id).

## Guardrails

- Per-call limits: 64 objects and 1,000 instants by default
  (`--max-objects`, `--max-times`). A token goes in
  `Authorization: Bearer`.
- The logs never carry instants, sites, names or tokens (SERVER.md,
  "Logging"). A birth chart is personal data. A method or tool name is
  logged only when it is one this server has; anything else is logged as
  `unknown`, since a client could put anything in that string.
- **Hostile input is refused, never crashes the server** (fuzzed:
  SERVER.md, "Fuzzing"):
  - JSON nested more than 64 deep is a parse error (-32700), refused
    before it is built. A 1 MiB body can hold half a million levels, and
    copying a value that deep overflowed the stack.
  - An `id` that is not a string, a number or null is an invalid request,
    answered with a null id (JSON-RPC 2.0, sections 4 and 5).
  - A reply goes out even if something put bytes in it that are not UTF-8:
    they become U+FFFD instead of stopping the transport.
  - A topocentric site's `height_m` is within −12,000..100,000 m, from the
    deepest trench to the Kármán line. A far higher "site" put light time
    centuries back, and a small body was integrated all that way before
    the call failed (30 s of CPU).
- No request contents in error text.

## How the two projects divide the work

Astrolog stays a desktop application, and exposes no chart tools (the
Astrolog session, 2026-09-18, on its maintainer's instruction). An agent
gets positions here, and may ask Astrolog, through its own interface, to
*display* them: Astrolog draws what the agent already knows. That is why
the words above must be one vocabulary.

## `/llms.txt`

One per surface, and here that means one: ours, served by `prometheia-json`
at `GET /llms.txt` and as the MCP resource `prometheia://llms.txt`. A
combined file was considered and settled against on 2026-09-20 — Astrolog
exposes no chart tools to an agent (above), so a combined file would have
one project's tools in it and a paragraph about the other, which is what
this document is for. The file describes what the server it came from will
answer, so an agent that reached a deployment reads that deployment's own
limits and catalog; a second copy in another repository would be a
description of a server the reader is not talking to. If Astrolog ever
serves agent tools, it serves its own `/llms.txt` beside them.

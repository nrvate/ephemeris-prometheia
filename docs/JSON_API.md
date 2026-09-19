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
| `lookup` | resolve a name to the objects it could mean |
| `capabilities` | what this engine answers: bodies, zodiacs, frames, coverage, in words |
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
  - `time` is an ISO 8601 UTC string (with an offset or `Z`), or
    `{"utc": …}`, `{"jd_tt": …}` or `{"jd_ut1": …}`.
  - `times` is a list of those.
  - `series` is `{"start", "step_days", "count"}`.
  - UTC goes through the leap-second table and the engine's ΔT
    (`convert_time` shows both).
- **Defaults** are what a chart wants: apparent (all three corrections),
  geocentric, the true ecliptic of date, tropical, rates on. Each can be
  overridden: `observer`, `frame`, `coordinates`, `corrections`, `zodiac`,
  `sidereal_plane`, `precession`, `rates`.
- **Objects by name**, not NAIF numbers.
  - Bare names resolve in a fixed order: planet, the Moon's points ("true
    node", "Lilith"…), hypothetical, star, catalog body.
  - A name that is unknown is a per-object error that says so, never a
    silent guess.
  - `{"body"|"star"|"asteroid"|"hypothetical"|"naif": …}` and
    `{"point", "of", "method"}` say exactly which.

## An answer

The answer to the request above, as served (the first result only, the
digits cut):

```json
{
  "engine": "Prometheia 0.4.0, JPL DE440 binary",
  "dataset": "Prometheia 0.4.0, JPL DE440 binary/linux_p1550p2650.440/-#a0c302f5",
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
        "corrections": ["light-time", "gravitational-deflection", "aberration"],
        "frame": "true equator/ecliptic and equinox of date",
        "coordinates": "ecliptic",
        "zodiac": {"token": "lahiri",
                   "doc": "docs/FRAMES.md (zodiacs) and docs/ENGINE.md (ayanamshas)"},
        "accuracy": {"statement": "JPL planetary ephemeris; positions agree with JPL Horizons to 6 µas",
                     "doc": "docs/VALIDATION.md"}
      },
      "error": null
    }
  ]
}
```

- **Every number is named with its unit.** No positional columns.
- **Provenance is per object:** the source, the corrections actually
  applied, the frame, and the zodiac.
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
  - This engine computes the first two.
  - The other three are refused as `unsupported`, never answered with
    another method.
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
  "Logging"). A birth chart is personal data.
- No request contents in error text.

## How the two projects divide the work

Astrolog stays a desktop application, and exposes no chart tools (the
Astrolog session, 2026-09-18, on its maintainer's instruction). An agent
gets positions here, and may ask Astrolog, through its own interface, to
*display* them: Astrolog draws what the agent already knows. That is why
the words above must be one vocabulary.

## Open questions

- Should `/llms.txt` live in both repositories, or should one project host a
  combined one?

# JSON and MCP interfaces (draft, under negotiation)

**Status: a proposal, 2026-09-18.** Nothing here is implemented.

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
  "time": {"utc": "1990-06-15T14:30:00+02:00"},
  "observer": {"topocentric": {"lon_deg": 8.55, "lat_deg": 47.37, "height_m": 500}},
  "zodiac": "lahiri",
  "objects": [
    {"body": "Sun"}, {"body": "Moon"}, {"body": "Mars"},
    {"point": "lunar-node", "method": "mean"},
    {"star": "Spica"}, {"asteroid": "Ceres"}, {"hypothetical": "cupido"}
  ]
}
```

- **Time:**
  - `time` takes `utc` (ISO 8601, with an offset or `Z`), `tt` or `jd_tt`.
  - `times` takes a list, or `{"start", "step", "count"}`.
  - UTC is converted with the leap-second table and the server's ΔT,
    exactly as the binary protocol's UT1 scale.
- **Defaults** are what a chart wants: apparent (all three corrections),
  geocentric, the ecliptic of date, rates on. Each can be overridden:
  `frame`, `coordinates`, `corrections`, `sidereal_plane`, `precision`.
- **Objects by name**, not NAIF numbers.
  - A name that is ambiguous or unknown is a per-object error that says so,
    never a silent guess.
  - `{"naif": 499}` stays available.

## An answer

```json
{
  "engine": "Prometheia 0.4.0, JPL DE440 binary",
  "dataset": "…#dc7c04b4",
  "results": [
    {
      "object": {"asked": {"body": "Mars"}, "resolved": "Mars (NAIF 4, system barycentre)"},
      "rows": [{"time": {"jd_tt": 2448058.10, "utc": "1990-06-15T12:30:00Z"},
                "longitude_deg": 12.345678, "latitude_deg": -1.234567, "distance_au": 1.234,
                "rates": {"longitude_deg_per_day": 0.61}, "ayanamsa_deg": 23.72}],
      "provenance": {
        "source": "JPL DE440 binary",
        "corrections": ["light-time", "deflection", "aberration"],
        "frame": "true ecliptic and equinox of date",
        "zodiac": {"token": "lahiri", "definition": "…", "doc": "docs/FRAMES.md#…"},
        "accuracy": {"statement": "6 µas against JPL Horizons", "doc": "docs/VALIDATION.md#…"}
      },
      "error": null
    }
  ]
}
```

- **Every number is named with its unit.** No positional columns.
- **Provenance is per object:** the source, the corrections actually
  applied, the frame, and the zodiac's published definition.
  - The accuracy statement is a measured number with the document that
    measured it, never a promise.
- **Flags stay flags:** `approximated`, `extrapolated` and `no_distance`
  appear as booleans. An agent must never receive an approximation
  unmarked.
- **Errors** are per object, with a code (`unknown-name`, `ambiguous-name`,
  `outside-coverage`, `unsupported`, `numerical-failure`) and a sentence.
  - Whole-request errors (malformed, over a limit, rate-limited) are HTTP
    4xx with the same codes and a `retry_after_s` where it applies.

## Words (to agree with Astrolog)

One vocabulary for both projects' JSON:
- **Bodies:** Sun, Moon, Mercury … Pluto, and the A.15 hypothetical tokens.
- **Zodiacs:** the A.11 tokens.
- **Points:** `lunar-node`, `lunar-apogee`, `node`/`perihelion`/… of a body,
  with `mean`/`osculating`.
- **Frames:** `true-of-date`, `mean-of-date`, `j2000`, `icrf`.
- **Sidereal planes:** `date`, `anchor`, `invariable`.
- **Corrections:** `light-time`, `deflection`, `aberration`.
- **Observers:** `geocentric`, `topocentric`, `heliocentric`, `barycentric`,
  `body-centre`.

## Guardrails

- The same limits, compute budget and tokens as the binary protocol. A
  token goes in `Authorization: Bearer`.
- The logs never carry instants, sites, names or tokens (SERVER.md,
  "Logging"). A birth chart is personal data.
- No request contents in error text.

## Open questions

- Should Astrolog's chart-as-data answers use the same answer shape, so one
  agent-facing schema covers both?
- Should `/llms.txt` and the OpenAPI schema live in both repositories, or
  should one project host a combined one?

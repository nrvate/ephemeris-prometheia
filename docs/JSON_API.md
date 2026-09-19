# JSON API (draft, under negotiation)

**Status: a proposal, 2026-09-18.** Nothing here is implemented. It is the
Prometheia side of the joint AI-forward plan agreed with the Astrolog project
over the agent channel (docs/HANDOFF.md). The shared vocabulary in "Words" is
the part the two projects must agree on; the rest is Prometheia's.

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

A JSON request is translated into protocol v4's `eph::Request` and answered
by the same session machinery: the same limits, cache, compute budget,
per-object errors and logging rules. The answer is the same `Answer`, written
as JSON. The two surfaces cannot disagree about what a question means, and
the cross-test can check that they do not.

## Endpoints (on prometheiad's existing port)

| method | path | what |
|---|---|---|
| `POST` | `/v1/positions` | positions of objects at instants |
| `GET` | `/v1/capabilities` | what this server answers: WELCOME, in words |
| `POST` | `/v1/lookup` | resolve names to objects (LOOKUP, in words) |
| `GET` | `/v1/openapi.json` | the schema |
| `GET` | `/llms.txt` | an agent-facing summary: what to ask, and how |

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

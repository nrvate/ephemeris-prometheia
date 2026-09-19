// SPDX-License-Identifier: GPL-2.0-or-later
//
// The JSON tools (docs/JSON_API.md): positions, lookup, capabilities and
// convert_time, as JSON in and JSON out over an Engine. prometheia-json serves
// them as MCP tools (stdio or streamable HTTP) and as plain JSON over HTTP; the
// request and the answer are the same JSON either way.
//
// These are convenience surfaces for AI agents and scripts, sized for the
// questions they ask: one chart or a short series. The engine's fast paths are
// the C++ library, the C API and prometheiad's binary protocol, and nothing
// here sits in them. Objects resolve through the binary server's own code
// (objects.hpp) and options map onto the same CalcOptions, so the two surfaces
// mean the same thing by the same question.
#ifndef PROMETHEIA_SERVER_JSON_TOOLS_HPP
#define PROMETHEIA_SERVER_JSON_TOOLS_HPP

#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"
#include "prometheia/engine.hpp"

namespace prometheia::server::jsontools {

using Json = nlohmann::json;

struct Tool {
    std::string name;
    std::string title;
    std::string description;
    Json input_schema; // JSON Schema of the arguments
};

// Every tool, with its schema: MCP's tools/list, and the JSON API's routes.
std::vector<Tool> tools();

// What one call may ask for: a chart or a short series, not bulk data.
struct Limits {
    size_t max_objects = 64;
    size_t max_times = 1000;
};

// What the tools say about the engine they run on.
struct Context {
    std::string engine;  // e.g. "Prometheia 0.6.0, JPL DE440 binary"
    std::string dataset; // the dataset identity, when there is one
    Limits limits;
};

// A tool's failure as a whole: the arguments were wrong, or the name is not a
// tool. Per-object failures are not this; they are part of a result.
struct ToolError {
    std::string code; // "invalid-arguments", "unknown-tool", "over-limit"
    std::string message;
};

// Runs one tool. The result is its JSON answer, or the whole call's error.
Result<Json> call(Engine& engine, const Context& ctx, std::string_view tool, const Json& args,
                  ToolError* error);

// The agent-facing summary of the tools (the MCP resource prometheia://llms.txt
// and GET /llms.txt).
std::string llms_txt();

} // namespace prometheia::server::jsontools

#endif // PROMETHEIA_SERVER_JSON_TOOLS_HPP

// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Model Context Protocol over the JSON tools (docs/JSON_API.md): a
// transport-free JSON-RPC 2.0 dispatcher. prometheia-json carries it over
// stdio (one message per line) and over streamable HTTP (POST /mcp); both
// only move bytes, and every answer comes from here.
//
// Served: initialize (version negotiation), ping, tools/list, tools/call,
// resources/list, resources/read, and the initialized notification. A tool's
// own failure (bad arguments) is a tools/call result with isError, as the
// specification asks, so the agent can read it and correct itself; JSON-RPC
// errors are for the protocol: an unknown method, a malformed request, an
// unknown tool.
#ifndef PROMETHEIA_SERVER_MCP_HPP
#define PROMETHEIA_SERVER_MCP_HPP

#include <optional>
#include <string>

#include "json_tools.hpp"

namespace prometheia::server::mcp {

using jsontools::Json;

// The MCP revisions this server speaks, newest first.
inline constexpr const char* kProtocolVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

// Whether a revision is one of kProtocolVersions.
bool supported_version(const std::string& v);

// A message's method for the log: "batch", or "?" when it has none.
std::string method_name(const Json& message);

class Dispatcher {
public:
    Dispatcher(Engine& engine, jsontools::Context ctx, std::string server_version)
        : engine_(engine), ctx_(std::move(ctx)), version_(std::move(server_version)) {}

    // One JSON-RPC message, or a batch (an array). The response, or nothing
    // for a notification (and for a batch of notifications only).
    std::optional<Json> handle(const Json& message);

    // A message that did not parse as JSON: JSON-RPC's parse error.
    static Json parse_error(const std::string& why);

private:
    std::optional<Json> one(const Json& message);
    Json result(const Json& id, Json result) const;
    static Json error(const Json& id, int code, const std::string& message);

    Engine& engine_;
    jsontools::Context ctx_;
    std::string version_;
};

} // namespace prometheia::server::mcp

#endif // PROMETHEIA_SERVER_MCP_HPP

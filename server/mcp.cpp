// SPDX-License-Identifier: GPL-2.0-or-later
#include "mcp.hpp"

namespace prometheia::server::mcp {
namespace {

// JSON-RPC 2.0 error codes.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;

constexpr const char* kLlmsUri = "prometheia://llms.txt";

// A string member, or empty when it is absent or not a string.
std::string str(const Json& o, const char* key) {
    return o.contains(key) && o[key].is_string() ? o[key].get<std::string>() : std::string();
}

} // namespace

bool supported_version(const std::string& v) {
    for (const char* s : kProtocolVersions)
        if (v == s)
            return true;
    return false;
}

std::string method_name(const Json& m) {
    if (m.is_array())
        return "batch";
    return m.is_object() && m.contains("method") && m["method"].is_string()
               ? m["method"].get<std::string>()
               : std::string("?");
}

Json Dispatcher::parse_error(const std::string& why) {
    return error(nullptr, kParseError, "not JSON: " + why);
}

Json Dispatcher::result(const Json& id, Json r) const {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(r)}};
}

Json Dispatcher::error(const Json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

std::optional<Json> Dispatcher::handle(const Json& message) {
    if (message.is_array()) {
        if (message.empty())
            return error(nullptr, kInvalidRequest, "an empty batch");
        Json out = Json::array();
        for (const Json& m : message)
            if (auto r = one(m))
                out.push_back(std::move(*r));
        if (out.empty())
            return std::nullopt;
        return out;
    }
    return one(message);
}

std::optional<Json> Dispatcher::one(const Json& m) {
    if (!m.is_object() || !m.contains("jsonrpc") || m["jsonrpc"] != "2.0" ||
        !m.contains("method") || !m["method"].is_string()) {
        // A response the client sent us (we send no requests), or not a message.
        if (m.is_object() && (m.contains("result") || m.contains("error")))
            return std::nullopt;
        return error(m.is_object() && m.contains("id") ? m["id"] : Json(nullptr), kInvalidRequest,
                     "not a JSON-RPC 2.0 request");
    }
    const std::string method = m["method"].get<std::string>();
    const bool notification = !m.contains("id");
    const Json id = notification ? Json(nullptr) : m["id"];
    const Json params = m.contains("params") ? m["params"] : Json::object();
    if (!params.is_object() && !notification)
        return error(id, kInvalidParams, "params is an object");
    if (notification)
        return std::nullopt; // notifications/initialized, cancelled: nothing to answer

    if (method == "initialize") {
        const std::string asked = str(params, "protocolVersion");
        return result(id, {{"protocolVersion",
                            supported_version(asked) ? asked : std::string(kProtocolVersions[0])},
                           {"capabilities",
                            {{"tools", {{"listChanged", false}}},
                             {"resources", {{"listChanged", false}, {"subscribe", false}}}}},
                           {"serverInfo",
                            {{"name", "prometheia-json"},
                             {"title", "Prometheia ephemeris"},
                             {"version", version_}}},
                           {"instructions",
                            "Astronomical positions for astrology and astronomy. Call "
                            "capabilities first to see what is served; positions takes objects by "
                            "name and times in ISO 8601 UTC. Read each result's resolved name and "
                            "provenance. The resource prometheia://llms.txt explains the "
                            "conventions."}});
    }
    if (method == "ping")
        return result(id, Json::object());
    if (method == "tools/list") {
        Json list = Json::array();
        for (const jsontools::Tool& t : jsontools::tools())
            list.push_back({{"name", t.name},
                            {"title", t.title},
                            {"description", t.description},
                            {"inputSchema", t.input_schema}});
        return result(id, {{"tools", list}});
    }
    if (method == "tools/call") {
        if (!params.is_object() || !params.contains("name") || !params["name"].is_string())
            return error(id, kInvalidParams, "tools/call needs a tool name");
        const std::string name = params["name"].get<std::string>();
        jsontools::ToolError err;
        auto r = jsontools::call(
            engine_, ctx_, name,
            params.contains("arguments") ? params["arguments"] : Json::object(), &err);
        if (!r) {
            if (err.code == "unknown-tool")
                return error(id, kInvalidParams, "no tool is called " + name);
            // The tool's own failure: a result the agent can read and act on.
            const Json body = {{"error", {{"code", err.code}, {"message", err.message}}}};
            return result(id, {{"content", {{{"type", "text"}, {"text", body.dump()}}}},
                               {"structuredContent", body},
                               {"isError", true}});
        }
        return result(id, {{"content", {{{"type", "text"}, {"text", r.value().dump()}}}},
                           {"structuredContent", r.value()},
                           {"isError", false}});
    }
    if (method == "resources/list")
        return result(id, {{"resources",
                            {{{"uri", kLlmsUri},
                              {"name", "llms.txt"},
                              {"title", "How to use the Prometheia tools"},
                              {"mimeType", "text/markdown"}}}}});
    if (method == "resources/read") {
        if (str(params, "uri") != kLlmsUri)
            return error(id, kInvalidParams, "no such resource");
        return result(id, {{"contents",
                            {{{"uri", kLlmsUri},
                              {"mimeType", "text/markdown"},
                              {"text", jsontools::llms_txt()}}}}});
    }
    if (method == "resources/templates/list")
        return result(id, {{"resourceTemplates", Json::array()}});
    if (method == "prompts/list")
        return result(id, {{"prompts", Json::array()}});
    return error(id, kMethodNotFound, "no method " + method);
}

} // namespace prometheia::server::mcp

// SPDX-License-Identifier: GPL-2.0-or-later
//
// libFuzzer target: prometheia-json's messages into a fresh MCP dispatcher
// (docs/SERVER.md, "Fuzzing"; docs/JSON_API.md). The sanitizers are one
// oracle. The others are what a client relies on:
//   - no exception escapes the dispatcher;
//   - every reply serialises (nlohmann's dump() throws on a string that is
//     not UTF-8, so a client string echoed back cut mid-character would take
//     the server down instead of answering);
//   - every reply is a JSON-RPC 2.0 response: an object with "jsonrpc" "2.0",
//     an "id", and exactly one of "result" and "error", the error with an
//     integer code and a string message; a tool's own failure is a result
//     with isError true.
//
// Input: one mode byte, then JSON text.
//   mode bit 0   the text is a tool's arguments: it is wrapped as a
//                tools/call to the tool picked by the mode's upper bits, so
//                the fuzzer reaches argument handling without having to
//                discover the JSON-RPC envelope; otherwise the text is the
//                whole message, as the stdio and HTTP transports pass it
// The text goes through mcp::parse, as every transport's does; what it
// refuses (not JSON, nested too deeply) is the transport's parse error and is
// skipped.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

#include "json_tools.hpp"
#include "mcp.hpp"
#include "synthetic_spk.hpp"

using namespace prometheia;
using namespace prometheia::server;
using jsontools::Json;

namespace {

// One engine for the whole run (building one per input would cost
// milliseconds); the tools keep no state between calls.
Engine& engine() {
    static synth::TempFile tf("fuzz-json");
    static Engine e = [] {
        Engine engine = synth::open_synthetic(tf);
        auto catalog =
            engine.add_catalog(std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/sample-100.epm");
        if (!catalog.ok()) {
            std::fprintf(stderr, "fuzz_json: %s\n", catalog.error().message.c_str());
            std::abort();
        }
        return engine;
    }();
    return e;
}

mcp::Dispatcher& dispatcher() {
    static mcp::Dispatcher d = [] {
        jsontools::Context ctx;
        ctx.engine = "Prometheia fuzz, synthetic kernel";
        ctx.dataset = "synthetic/fuzz#00000000";
        // Small bounds: each input computes microseconds, not seconds.
        ctx.limits.max_objects = 8;
        ctx.limits.max_times = 16;
        return mcp::Dispatcher(engine(), ctx, "fuzz");
    }();
    return d;
}

[[noreturn]] void fail(const char* what, const std::string& input, const std::string& detail) {
    std::fprintf(stderr, "fuzz_json: %s: %s\n  input: %.4000s\n", what, detail.c_str(),
                 input.c_str());
    std::abort();
}

void check_response(const Json& r, const std::string& input) {
    if (!r.is_object())
        fail("a reply that is not an object", input, r.dump());
    if (!r.contains("jsonrpc") || r["jsonrpc"] != "2.0")
        fail("a reply without jsonrpc 2.0", input, r.dump());
    if (!r.contains("id"))
        fail("a reply without an id", input, r.dump());
    const Json& id = r["id"];
    if (!(id.is_null() || id.is_string() || id.is_number()))
        fail("a reply whose id is not null, a string or a number", input, r.dump());
    const bool has_result = r.contains("result"), has_error = r.contains("error");
    if (has_result == has_error)
        fail("a reply without exactly one of result and error", input, r.dump());
    if (has_error) {
        const Json& e = r["error"];
        if (!e.is_object() || !e.contains("code") || !e["code"].is_number_integer() ||
            !e.contains("message") || !e["message"].is_string())
            fail("a malformed error object", input, r.dump());
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0)
        return 0;
    const uint8_t mode = data[0];
    const std::string text(reinterpret_cast<const char*>(data + 1), size - 1);
    std::string why;
    Json parsed = mcp::parse(text, &why);
    if (parsed.is_discarded())
        return 0;
    Json message;
    if (mode & 1) {
        static const auto tools = jsontools::tools();
        const std::string& name = tools[(mode >> 1) % tools.size()].name;
        message = {{"jsonrpc", "2.0"},
                   {"id", 1},
                   {"method", "tools/call"},
                   {"params", {{"name", name}, {"arguments", std::move(parsed)}}}};
    } else {
        message = std::move(parsed);
    }
    std::optional<Json> reply;
    try {
        reply = dispatcher().handle(message);
    } catch (const std::exception& e) {
        fail("an exception escaped the dispatcher", text, e.what());
    }
    if (!reply)
        return 0;
    std::string wire;
    try {
        wire = reply->dump();
    } catch (const std::exception& e) {
        fail("a reply that does not serialise", text, e.what());
    }
    if (reply->is_array()) {
        for (const Json& r : *reply)
            check_response(r, text);
    } else {
        check_response(*reply, text);
    }
    return 0;
}

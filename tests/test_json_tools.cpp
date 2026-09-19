// SPDX-License-Identifier: GPL-2.0-or-later
//
// The JSON tools (server/json_tools.hpp) on the synthetic kernel: the answers
// are the engine's, names resolve as documented, and failures are typed.
#include <algorithm>
#include <cmath>

#include "json_tools.hpp"
#include "mcp.hpp"
#include "synthetic_spk.hpp"
#include <doctest/doctest.h>

using namespace prometheia;
using namespace prometheia::server;
using jsontools::Json;

namespace {

Json run(Engine& e, const char* tool, const Json& args, jsontools::ToolError* err = nullptr) {
    jsontools::Context ctx;
    ctx.engine = "test";
    jsontools::ToolError scratch;
    auto r = jsontools::call(e, ctx, tool, args, err ? err : &scratch);
    return r.ok() ? r.value() : Json(nullptr);
}

} // namespace

TEST_CASE("json_positions_are_the_engines") {
    synth::TempFile tf("json-pos");
    Engine e = synth::open_synthetic(tf);
    const Json out = run(e, "positions", {{"time", {{"jd_tt", 2451545.0}}}, {"objects", {"Sun"}}});
    REQUIRE(out.is_object());
    const Json& r = out["results"][0];
    CHECK(r["error"].is_null());
    CHECK(r["object"]["resolved"] == "Sun");
    CHECK(r["object"]["naif"] == 10);
    auto want = e.calc(10, 2451545.0, CalcOptions{});
    REQUIRE(want.ok());
    CHECK(r["rows"][0]["longitude_deg"].get<double>() == want.value().pos.lon_deg);
    CHECK(r["rows"][0]["latitude_deg"].get<double>() == want.value().pos.lat_deg);
    // The Sun's own light is not deflected by the Sun.
    CHECK(r["provenance"]["corrections"] == Json({"light-time", "aberration"}));
}

TEST_CASE("json_times_and_names") {
    synth::TempFile tf("json-names");
    Engine e = synth::open_synthetic(tf);
    // UTC with an offset is the same instant as its Z form.
    const Json a = run(e, "convert_time", {{"time", "2000-01-01T14:00:00+02:00"}});
    const Json b = run(e, "convert_time", {{"time", "2000-01-01T12:00:00Z"}});
    CHECK(a["jd_tt"] == b["jd_tt"]);
    CHECK(std::fabs(a["jd_tt"].get<double>() - (2451545.0 + 64.184 / 86400.0)) < 1e-9);
    // A star by name; an unknown name is a typed per-object error, not a guess.
    const Json out = run(e, "positions",
                         {{"time", "2000-01-01T12:00:00Z"},
                          {"observer", "barycentric"},
                          {"objects", {"Spica", "Nosuchbody", Json{{"point", "sideways"}}}}});
    REQUIRE(out["results"].size() == 3);
    CHECK(out["results"][0]["object"]["kind"] == "star");
    CHECK(out["results"][0]["provenance"]["corrections"] ==
          Json({"gravitational-deflection", "aberration"}));
    CHECK(out["results"][1]["error"]["code"] == "unknown-name");
    CHECK(out["results"][2]["error"]["code"] == "invalid-arguments");
}

TEST_CASE("json_bad_arguments_are_whole_call_errors") {
    synth::TempFile tf("json-bad");
    Engine e = synth::open_synthetic(tf);
    jsontools::ToolError err;
    CHECK(run(e, "positions", {{"objects", {"Sun"}}}, &err).is_null());
    CHECK(err.code == "invalid-arguments");
    CHECK(run(e, "positions", {{"time", "yesterday"}, {"objects", {"Sun"}}}, &err).is_null());
    CHECK(err.code == "invalid-arguments");
    CHECK(run(e, "nosuchtool", Json::object(), &err).is_null());
    CHECK(err.code == "unknown-tool");
    CHECK(run(e, "positions",
              {{"time", {{"jd_tt", 2451545.0}}}, {"objects", {"Sun"}}, {"zodiac", "nosuchzodiac"}},
              &err)
              .is_null());
    CHECK(err.code == "invalid-arguments");
}

TEST_CASE("json_tools_list_their_schemas") {
    const auto t = jsontools::tools();
    REQUIRE(t.size() == 4);
    for (const auto& tool : t) {
        CHECK(!tool.description.empty());
        CHECK(tool.input_schema["type"] == "object");
    }
    CHECK(jsontools::llms_txt().find("positions") != std::string::npos);
}

// The MCP dispatcher (server/mcp.hpp): JSON-RPC 2.0, whatever the transport.
TEST_CASE("mcp_dispatcher") {
    synth::TempFile tf("json-mcp");
    Engine e = synth::open_synthetic(tf);
    jsontools::Context ctx;
    ctx.engine = "test";
    mcp::Dispatcher d(e, ctx, "0.0.0");
    const auto req = [](int id, const char* method, Json params = Json::object()) {
        return Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
    };
    // Version negotiation: a supported revision is echoed, another gets ours.
    auto init = d.handle(req(1, "initialize", {{"protocolVersion", "2025-03-26"}}));
    REQUIRE(init);
    CHECK((*init)["result"]["protocolVersion"] == "2025-03-26");
    CHECK((*init)["result"]["serverInfo"]["name"] == "prometheia-json");
    init = d.handle(req(2, "initialize", {{"protocolVersion", "1999-01-01"}}));
    CHECK((*init)["result"]["protocolVersion"] == mcp::kProtocolVersions[0]);
    // A notification has no answer.
    CHECK_FALSE(d.handle(Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}));
    // Tools: listed with schemas; a call answers text and structured content.
    auto list = d.handle(req(3, "tools/list"));
    CHECK((*list)["result"]["tools"].size() == 4);
    auto ok =
        d.handle(req(4, "tools/call",
                     {{"name", "positions"},
                      {"arguments", {{"time", {{"jd_tt", 2451545.0}}}, {"objects", {"Sun"}}}}}));
    CHECK((*ok)["result"]["isError"] == false);
    CHECK((*ok)["result"]["structuredContent"]["results"][0]["object"]["resolved"] == "Sun");
    CHECK((*ok)["result"]["content"][0]["type"] == "text");
    // A tool's own failure is a result the agent reads; a protocol error is not.
    auto bad =
        d.handle(req(5, "tools/call", {{"name", "positions"}, {"arguments", Json::object()}}));
    CHECK((*bad)["result"]["isError"] == true);
    auto unknown_tool = d.handle(req(6, "tools/call", {{"name", "nosuch"}}));
    CHECK((*unknown_tool)["error"]["code"] == -32602);
    auto unknown_method = d.handle(req(7, "nosuch/method"));
    CHECK((*unknown_method)["error"]["code"] == -32601);
    CHECK((*d.handle(Json{{"not", "rpc"}}))["error"]["code"] == -32600);
    // Resources: the agent-facing summary.
    auto read = d.handle(req(8, "resources/read", {{"uri", "prometheia://llms.txt"}}));
    CHECK((*read)["result"]["contents"][0]["text"].get<std::string>().find("positions") !=
          std::string::npos);
    // A batch answers each request, and skips the notifications.
    auto batch = d.handle(Json::array({req(9, "ping"), Json{{"jsonrpc", "2.0"}, {"method", "x"}}}));
    REQUIRE(batch);
    CHECK(batch->size() == 1);
}

TEST_CASE("json_wrong_types_are_errors_not_exceptions") {
    // Every argument of every tool, at top level and inside a site or a time,
    // given every JSON type: the call answers or refuses, and never throws.
    synth::TempFile tf("json-types");
    Engine e = synth::open_synthetic(tf);
    const Json kinds[] = {Json(3.5),
                          Json(-7),
                          Json("x"),
                          Json(true),
                          Json(nullptr),
                          Json::array({1, "a"}),
                          Json::object({{"k", 1}})};
    const Json base = {{"time", "2000-01-01T12:00:00Z"}, {"objects", {"Sun"}}};
    const char* nested[][2] = {{"site", "lon_deg"}, {"site", "lat_deg"}, {"site", "height_m"},
                               {"time", "utc"},     {"time", "jd_tt"},   {"time", "jd_ut1"},
                               {"series", "start"}, {"series", "count"}, {"series", "step_days"},
                               {"zodiac", "user"}};
    const char* object_keys[] = {"naif",         "body",  "star",   "asteroid",
                                 "hypothetical", "point", "method", "of"};
    size_t calls = 0;
    for (const jsontools::Tool& t : jsontools::tools()) {
        for (const auto& [key, schema] : t.input_schema["properties"].items()) {
            for (const Json& k : kinds) {
                Json args = base;
                args["observer"] = "topocentric";
                args["site"] = {{"lon_deg", 1.0}, {"lat_deg", 2.0}};
                args[key] = k;
                jsontools::ToolError err;
                CHECK_NOTHROW(run(e, t.name.c_str(), args, &err));
                ++calls;
            }
        }
    }
    for (const auto& n : nested) {
        for (const Json& k : kinds) {
            Json args = base;
            args["observer"] = "topocentric";
            args["site"] = {{"lon_deg", 1.0}, {"lat_deg", 2.0}};
            if (std::string(n[0]) == "series") {
                args.erase("time");
                args["series"] = {
                    {"start", "2000-01-01T00:00:00Z"}, {"count", 2}, {"step_days", 1}};
            } else if (std::string(n[0]) == "time") {
                args["time"] = Json::object();
            } else if (std::string(n[0]) == "zodiac") {
                args["zodiac"] = Json::object();
            }
            args[n[0]][n[1]] = k;
            CHECK_NOTHROW(run(e, "positions", args));
            // And each member of a user zodiac.
            Json z = base;
            z["zodiac"] = {{"user", {{"epoch_jd_tt", 2451545.0}, {"ayanamsa_deg", 23.0}}}};
            z["zodiac"]["user"][std::string(n[1]) == "utc" ? "epoch_jd_tt" : "ayanamsa_deg"] = k;
            CHECK_NOTHROW(run(e, "positions", z));
            calls += 2;
        }
    }
    for (const char* key : object_keys) {
        for (const Json& k : kinds) {
            Json args = base;
            args["objects"] = Json::array({Json{{key, k}}, Json{{"point", "mean node"}, {key, k}}});
            CHECK_NOTHROW(run(e, "positions", args));
            ++calls;
        }
    }
    CHECK(calls > 100);
    // The protocol layer: a method, params or id of the wrong type.
    mcp::Dispatcher d(e, jsontools::Context{}, "0.0.0");
    for (const Json& k : kinds) {
        CHECK_NOTHROW(d.handle(Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", k}}));
        CHECK_NOTHROW(d.handle(Json{{"jsonrpc", k}, {"id", 1}, {"method", "ping"}}));
        CHECK_NOTHROW(
            d.handle(Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", k}}));
        CHECK_NOTHROW(d.handle(Json{{"jsonrpc", "2.0"},
                                    {"id", 1},
                                    {"method", "tools/call"},
                                    {"params", {{"name", "positions"}, {"arguments", k}}}}));
        CHECK_NOTHROW(d.handle(Json{{"jsonrpc", "2.0"},
                                    {"id", 1},
                                    {"method", "initialize"},
                                    {"params", {{"protocolVersion", k}}}}));
        CHECK_NOTHROW(d.handle(Json{{"jsonrpc", "2.0"},
                                    {"id", 1},
                                    {"method", "resources/read"},
                                    {"params", {{"uri", k}}}}));
        CHECK_NOTHROW(mcp::method_name(Json{{"method", k}}));
    }
    auto r = d.handle(Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", 5}});
    REQUIRE(r);
    CHECK((*r)["error"]["code"] == -32600);
}

TEST_CASE("json_vocabulary_matches_the_registries") {
    synth::TempFile tf("json-words");
    Engine e = synth::open_synthetic(tf);
    // A.5's body observer: the engine's Center::Body, by name or NAIF id.
    CalcOptions o;
    o.center = Center::Body;
    o.center_body = 5;
    auto want = e.calc(10, 2451545.0, o);
    REQUIRE(want.ok());
    for (const Json& center : {Json("Jupiter"), Json(5)}) {
        const Json out = run(e, "positions",
                             {{"time", {{"jd_tt", 2451545.0}}},
                              {"objects", {"Sun"}},
                              {"observer", "body"},
                              {"center", center}});
        REQUIRE(out.is_object());
        CHECK(out["results"][0]["rows"][0]["longitude_deg"].get<double>() ==
              want.value().pos.lon_deg);
    }
    // Seen from the Sun, nothing is deflected by it.
    Json from_sun = run(e, "positions",
                        {{"time", {{"jd_tt", 2451545.0}}},
                         {"objects", {"Earth"}},
                         {"observer", "body"},
                         {"center", "Sun"}});
    CHECK(from_sun["results"][0]["provenance"]["corrections"] ==
          Json({"light-time", "aberration"}));
    jsontools::ToolError err;
    CHECK(run(e, "positions",
              {{"time", {{"jd_tt", 2451545.0}}}, {"objects", {"Sun"}}, {"observer", "body"}}, &err)
              .is_null());
    CHECK(err.code == "invalid-arguments");
    // A.7's word, and the old one still read.
    for (const char* word : {"gravitational-deflection", "deflection"}) {
        const Json out = run(
            e, "positions",
            {{"time", {{"jd_tt", 2451545.0}}}, {"objects", {"Jupiter"}}, {"corrections", {word}}});
        CHECK(out["results"][0]["provenance"]["corrections"] == Json({"gravitational-deflection"}));
    }
    // A.14: the three methods this engine does not compute are named, and
    // refused as unsupported; never answered with another method.
    for (const char* m : {"interpolated", "osculating-barycentric", "focal-point"}) {
        const Json out =
            run(e, "positions",
                {{"time", {{"jd_tt", 2451545.0}}},
                 {"objects", {{{"point", "aphelion"}, {"of", "Earth"}, {"method", m}}}}});
        CHECK(out["results"][0]["error"]["code"] == "unsupported");
        CHECK_FALSE(out["results"][0].contains("rows"));
    }
    // §3.5a: a zodiac defined at the instant has no anchor plane, and
    // capabilities says so before the agent asks.
    const Json caps = run(e, "capabilities", Json::object());
    bool saw_instant = false, saw_epoch = false;
    for (const Json& z : caps["zodiacs"]) {
        const bool anchor = std::find(z["sidereal_planes"].begin(), z["sidereal_planes"].end(),
                                      Json("anchor")) != z["sidereal_planes"].end();
        if (z["name"] == "true-citra") {
            saw_instant = true;
            CHECK_FALSE(anchor);
            CHECK(z["defined"] == "at the instant");
        }
        if (z["name"] == "lahiri") {
            saw_epoch = true;
            CHECK(anchor);
        }
    }
    CHECK(saw_instant);
    CHECK(saw_epoch);
    CHECK(run(e, "positions",
              {{"time", {{"jd_tt", 2451545.0}}},
               {"objects", {"Sun"}},
               {"zodiac", "true-citra"},
               {"sidereal_plane", "anchor"}},
              &err)
              .is_null());
    CHECK(err.message.find("no anchor") != std::string::npos);
    CHECK_FALSE(run(e, "positions",
                    {{"time", {{"jd_tt", 2451545.0}}},
                     {"objects", {"Sun"}},
                     {"zodiac", "lahiri"},
                     {"sidereal_plane", "anchor"}})
                    .is_null());
}

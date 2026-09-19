// SPDX-License-Identifier: GPL-2.0-or-later
//
// The JSON tools (server/json_tools.hpp) on the synthetic kernel: the answers
// are the engine's, names resolve as documented, and failures are typed.
#include <cmath>

#include "json_tools.hpp"
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
    CHECK(out["results"][0]["provenance"]["corrections"] == Json({"deflection", "aberration"}));
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

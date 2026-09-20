// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia-json: the JSON tools (docs/JSON_API.md) for agents and scripts,
// over MCP (stdio, or streamable HTTP at /mcp) and plain JSON (/v1/<tool>).
// A convenience surface in its own process: the fast paths are the C++
// library, the C API and prometheiad's binary protocol, and nothing here runs
// inside them.
#include <App.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "dataset.hpp"
#include "limits.hpp"
#include "log.hpp"
#include "mcp.hpp"
#include "prometheia/prometheia.hpp"

using namespace prometheia;
using namespace prometheia::server;
using jsontools::Json;

namespace {

constexpr const char* kUsage =
    "usage: prometheia-json --ephemeris FILE (--stdio | --http PORT) [options]\n"
    "  --ephemeris FILE      JPL DE binary or SPK kernel (required)\n"
    "  --catalog FILE        EPM1 small-body catalog (repeatable, newest wins)\n"
    "  --perturbers FILE     asteroid perturber SPK kernel\n"
    "  --hypotheticals FILE  element file of named hypothetical bodies (repeatable)\n"
    "  --stdio               MCP over stdin/stdout, one JSON-RPC message a line\n"
    "  --http PORT           MCP over streamable HTTP (POST /mcp) and plain JSON\n"
    "                        (POST /v1/<tool>, GET /v1/tools, GET /llms.txt)\n"
    "  --bind ADDR           HTTP listen address (default 127.0.0.1)\n"
    "  --allow-origin O      an Origin a browser may call from (repeatable; default:\n"
    "                        localhost and 127.0.0.1 only, against DNS rebinding)\n"
    "  --tokens FILE         require 'Authorization: Bearer <token>' from this list\n"
    "  --max-objects N       objects per call (default 64)\n"
    "  --max-times N         instants per call (default 1000)\n"
    "  --log-level L         quiet, info (default) or debug; logs go to stderr and\n"
    "                        never carry instants, sites, names or tokens\n"
    "This is a convenience surface for agents and scripts. Bulk and high-speed data\n"
    "use the C++ library, the C API or prometheiad's binary protocol.\n";

constexpr size_t kMaxBody = 1u << 20;

bool parse_uint(const char* s, unsigned long& out) {
    char* end = nullptr;
    out = std::strtoul(s, &end, 10);
    return s[0] != '\0' && s[0] != '-' && end && *end == '\0';
}

// The host of an Origin ("http://localhost:3000" -> "localhost").
std::string origin_host(const std::string& origin) {
    const size_t scheme = origin.find("://");
    std::string rest = scheme == std::string::npos ? origin : origin.substr(scheme + 3);
    if (!rest.empty() && rest[0] == '[')
        return rest.substr(0, rest.find(']') + 1);
    return rest.substr(0, rest.find_first_of(":/"));
}

struct Http {
    const Log& log;
    mcp::Dispatcher& mcp;
    Engine& engine;
    jsontools::Context ctx;
    std::unordered_set<std::string> origins; // extra allowed, exact
    std::unordered_set<std::string> tokens;  // empty: none required

    // Why a request may not proceed, or empty.
    std::string refuse(const std::string& origin, const std::string& auth) const {
        if (!origin.empty() && origins.count(origin) == 0) {
            const std::string host = origin_host(origin);
            if (host != "localhost" && host != "127.0.0.1" && host != "[::1]")
                return "403 Forbidden";
        }
        if (!tokens.empty()) {
            const std::string prefix = "Bearer ";
            if (auth.rfind(prefix, 0) != 0 || tokens.count(auth.substr(prefix.size())) == 0)
                return "401 Unauthorized";
        }
        return {};
    }
};

template <typename Res>
void send(Res* res, const char* status, const char* type, const std::string& body) {
    res->writeStatus(status);
    if (type)
        res->writeHeader("Content-Type", type);
    res->end(body);
}

// Reads a POST body whole (up to kMaxBody), then calls done(body).
template <typename Res, typename Done>
void read_body(Res* res, Done done) {
    auto body = std::make_shared<std::string>();
    auto finished = std::make_shared<bool>(false);
    res->onAborted([finished] { *finished = true; });
    res->onData([res, body, finished, done](std::string_view chunk, bool last) {
        if (*finished)
            return;
        body->append(chunk.data(), chunk.size());
        if (body->size() > kMaxBody) {
            *finished = true;
            send(res, "413 Payload Too Large", "application/json",
                 R"({"error":{"code":"over-limit","message":"the body is over 1 MiB"}})");
            return;
        }
        if (last) {
            *finished = true;
            done(*body);
        }
    });
}

int serve_http(Http& h, const std::string& bind, int port) {
    uWS::App app;
    app.post("/mcp", [&h](auto* res, auto* req) {
        const std::string origin(req->getHeader("origin")), auth(req->getHeader("authorization"));
        const std::string version(req->getHeader("mcp-protocol-version"));
        if (auto why = h.refuse(origin, auth); !why.empty()) {
            h.log.write(LogLevel::Info, "http mcp refused %s", why.c_str());
            send(res, why.c_str(), nullptr, "");
            return;
        }
        if (!version.empty() && !mcp::supported_version(version)) {
            send(res, "400 Bad Request", "application/json",
                 R"({"error":"unsupported MCP-Protocol-Version"})");
            return;
        }
        read_body(res, [&h, res](const std::string& body) {
            const auto t0 = std::chrono::steady_clock::now();
            std::string why;
            Json msg = mcp::parse(body, &why);
            if (msg.is_discarded()) {
                send(res, "400 Bad Request", "application/json",
                     mcp::wire(mcp::Dispatcher::parse_error("the body", why)));
                return;
            }
            const std::string method = mcp::method_for_log(msg);
            auto reply = h.mcp.handle(msg);
            h.log.write(
                LogLevel::Info, "http mcp %s ms=%.1f", method.c_str(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count());
            if (!reply)
                send(res, "202 Accepted", nullptr, "");
            else
                send(res, "200 OK", "application/json", mcp::wire(*reply));
        });
    });
    // No server-initiated messages, so no stream to open; sessions are not kept.
    app.get("/mcp", [](auto* res, auto*) {
        res->writeStatus("405 Method Not Allowed")->writeHeader("Allow", "POST")->end();
    });
    app.del("/mcp", [](auto* res, auto*) {
        res->writeStatus("405 Method Not Allowed")->writeHeader("Allow", "POST")->end();
    });
    app.get("/v1/tools", [&h](auto* res, auto* req) {
        if (auto why = h.refuse(std::string(req->getHeader("origin")),
                                std::string(req->getHeader("authorization")));
            !why.empty()) {
            send(res, why.c_str(), nullptr, "");
            return;
        }
        Json list = Json::array();
        for (const jsontools::Tool& t : jsontools::tools())
            list.push_back({{"name", t.name},
                            {"description", t.description},
                            {"input_schema", t.input_schema},
                            {"route", "POST /v1/" + t.name}});
        send(res, "200 OK", "application/json", mcp::wire(Json{{"tools", list}}));
    });
    app.post("/v1/:tool", [&h](auto* res, auto* req) {
        const std::string tool(req->getParameter(0));
        if (auto why = h.refuse(std::string(req->getHeader("origin")),
                                std::string(req->getHeader("authorization")));
            !why.empty()) {
            send(res, why.c_str(), nullptr, "");
            return;
        }
        read_body(res, [&h, res, tool](const std::string& body) {
            std::string why;
            Json args = body.empty() ? Json::object() : mcp::parse(body, &why);
            if (args.is_discarded()) {
                send(res, "400 Bad Request", "application/json",
                     mcp::wire(
                         Json{{"error",
                               {{"code", "invalid-arguments"}, {"message", "the body: " + why}}}}));
                return;
            }
            jsontools::ToolError err;
            auto r = jsontools::call(h.engine, h.ctx, tool, args, &err);
            h.log.write(LogLevel::Info, "http v1 %s %s",
                        err.code == "unknown-tool" ? "unknown" : tool.c_str(),
                        r ? "ok" : err.code.c_str());
            if (!r) {
                send(res, err.code == "unknown-tool" ? "404 Not Found" : "400 Bad Request",
                     "application/json",
                     mcp::wire(Json{{"error", {{"code", err.code}, {"message", err.message}}}}));
                return;
            }
            send(res, "200 OK", "application/json", mcp::wire(r.value()));
        });
    });
    app.get("/llms.txt", [](auto* res, auto*) {
        send(res, "200 OK", "text/markdown; charset=utf-8", jsontools::llms_txt());
    });
    app.get("/healthz", [](auto* res, auto*) { send(res, "200 OK", "text/plain", "ok\n"); });

    bool listening = false;
    app.listen(bind, port, [&](us_listen_socket_t* s) { listening = s != nullptr; });
    if (!listening) {
        h.log.always("cannot listen on %s:%d", bind.c_str(), port);
        return 1;
    }
    h.log.always("MCP at http://%s:%d/mcp, JSON at /v1/<tool>", bind.c_str(), port);
    app.run();
    return 0;
}

int serve_stdio(mcp::Dispatcher& mcp, const Log& log) {
    log.always("MCP on stdio");
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        std::string why;
        Json msg = mcp::parse(line, &why);
        std::optional<Json> reply =
            msg.is_discarded() ? mcp::Dispatcher::parse_error("the line", why) : mcp.handle(msg);
        if (!msg.is_discarded())
            log.write(LogLevel::Debug, "stdio %s", mcp::method_for_log(msg).c_str());
        if (reply) {
            std::cout << mcp::wire(*reply) << '\n';
            std::cout.flush();
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string ephemeris, perturbers, tokens_path, bind = "127.0.0.1";
    std::vector<std::string> catalogs, hypotheticals;
    std::unordered_set<std::string> origins;
    bool stdio = false;
    int port = -1;
    jsontools::Limits limits;
    LogLevel level = LogLevel::Info;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "prometheia-json: %s needs a value\n%s", arg.c_str(), kUsage);
                std::exit(2);
            }
            return argv[++i];
        };
        const auto number = [&](unsigned long lo, unsigned long hi) {
            unsigned long n = 0;
            if (!parse_uint(value(), n) || n < lo || n > hi) {
                std::fprintf(stderr, "prometheia-json: bad value for %s\n%s", arg.c_str(), kUsage);
                std::exit(2);
            }
            return n;
        };
        if (arg == "--ephemeris") {
            ephemeris = value();
        } else if (arg == "--catalog") {
            catalogs.emplace_back(value());
        } else if (arg == "--perturbers") {
            perturbers = value();
        } else if (arg == "--hypotheticals") {
            hypotheticals.emplace_back(value());
        } else if (arg == "--stdio") {
            stdio = true;
        } else if (arg == "--http") {
            port = int(number(0, 65535));
        } else if (arg == "--bind") {
            bind = value();
        } else if (arg == "--allow-origin") {
            origins.insert(value());
        } else if (arg == "--tokens") {
            tokens_path = value();
        } else if (arg == "--max-objects") {
            limits.max_objects = number(1, 4096);
        } else if (arg == "--max-times") {
            limits.max_times = number(1, 100000);
        } else if (arg == "--log-level") {
            const auto l = parse_log_level(value());
            if (!l) {
                std::fprintf(stderr, "prometheia-json: bad value for --log-level\n%s", kUsage);
                return 2;
            }
            level = *l;
        } else if (arg == "--help" || arg == "-h") {
            std::fputs(kUsage, stdout);
            return 0;
        } else {
            std::fprintf(stderr, "prometheia-json: bad option %s\n%s", arg.c_str(), kUsage);
            return 2;
        }
    }
    if (ephemeris.empty() || stdio == (port >= 0)) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    // Logs on stderr: on stdio, stdout carries only the protocol.
    const Log log(level, stderr, "prometheia-json");

    auto opened = Engine::open(ephemeris);
    if (!opened) {
        log.always("%s", opened.error().message.c_str());
        return 1;
    }
    Engine engine = std::move(opened).value();
    for (const std::string& c : catalogs)
        if (auto r = engine.add_catalog(c); !r) {
            log.always("%s: %s", c.c_str(), r.error().message.c_str());
            return 1;
        }
    if (!perturbers.empty())
        if (auto r = engine.add_perturbers(perturbers); !r) {
            log.always("%s: %s", perturbers.c_str(), r.error().message.c_str());
            return 1;
        }
    for (const std::string& h : hypotheticals)
        if (auto r = engine.add_hypotheticals(h); !r) {
            log.always("%s", r.error().message.c_str());
            return 1;
        }
    const Dataset dataset =
        make_dataset("Prometheia " PROMETHEIA_VERSION ", " + std::string(engine.source()),
                     ephemeris, catalogs, perturbers, hypotheticals);
    jsontools::Context ctx;
    ctx.engine = dataset.engine;
    ctx.dataset = dataset.id;
    ctx.limits = limits;
    ctx.catalogs = catalogs.size();
    mcp::Dispatcher dispatcher(engine, ctx, PROMETHEIA_VERSION);

    if (stdio)
        return serve_stdio(dispatcher, log);

    std::unordered_set<std::string> tokens;
    if (!tokens_path.empty()) {
        auto t = Limits::load_tokens(tokens_path);
        if (!t) {
            log.always("%s", t.error().message.c_str());
            return 1;
        }
        tokens = std::move(t).value();
        log.always("%zu token(s) required from %s", tokens.size(), tokens_path.c_str());
    }
    Http h{log, dispatcher, engine, ctx, std::move(origins), std::move(tokens)};
    return serve_http(h, bind, port);
}

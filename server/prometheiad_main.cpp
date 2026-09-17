// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheiad: the engine over Astrolog's ephemeris protocol (version 3) on
// WebSocket. docs/SERVER.md.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ws_server.hpp"

using namespace prometheia;
using namespace prometheia::server;

namespace {

constexpr const char* kUsage =
    "usage: prometheiad --ephemeris FILE [options]\n"
    "  --ephemeris FILE    JPL DE binary or SPK kernel (required)\n"
    "  --catalog FILE      EPM1 small-body catalog (repeatable, newest wins)\n"
    "  --perturbers FILE   asteroid perturber SPK kernel (e.g. sb441-n16.bsp)\n"
    "  --wire-map FILE     wire map from Astrolog's protocol specification;\n"
    "                      without one every object fails (docs/SERVER.md)\n"
    "  --bind ADDR         listen address (default: every interface)\n"
    "  --port N            default 47190; 0 picks a free port\n"
    "  --threads N         event loops, one engine each (default: hardware threads)\n"
    "  --max-cells N       objects x rows per REQUEST (default 100000)\n"
    "  --cache-mb N        result cache per loop (default 64)\n"
    "  --verbose           log connections\n";

volatile std::sig_atomic_t g_signal = 0;

void on_signal(int sig) {
    g_signal = sig;
}

bool parse_uint(const char* s, unsigned long& out) {
    char* end = nullptr;
    out = std::strtoul(s, &end, 10);
    return s[0] != '\0' && s[0] != '-' && end && *end == '\0';
}

} // namespace

int main(int argc, char** argv) {
    std::string ephemeris, perturbers, wire_map_path;
    std::vector<std::string> catalogs;
    WsOptions options;
    options.threads = std::max(1u, std::thread::hardware_concurrency());
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "prometheiad: %s needs a value\n%s", arg.c_str(), kUsage);
                std::exit(2);
            }
            return argv[++i];
        };
        unsigned long n = 0;
        if (arg == "--ephemeris") {
            ephemeris = value();
        } else if (arg == "--catalog") {
            catalogs.emplace_back(value());
        } else if (arg == "--perturbers") {
            perturbers = value();
        } else if (arg == "--wire-map") {
            wire_map_path = value();
        } else if (arg == "--bind") {
            options.bind = value();
        } else if (arg == "--port" && parse_uint(value(), n) && n <= 65535) {
            options.port = int(n);
        } else if (arg == "--threads" && parse_uint(value(), n) && n >= 1 && n <= 1024) {
            options.threads = unsigned(n);
        } else if (arg == "--max-cells" && parse_uint(value(), n) && n >= 1 && n <= 0xFFFFFFFFul) {
            options.config.max_cells = uint32_t(n);
        } else if (arg == "--cache-mb" && parse_uint(value(), n) && n <= 65536) {
            options.config.cache_bytes = size_t(n) << 20;
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            std::fputs(kUsage, stdout);
            return 0;
        } else {
            std::fprintf(stderr, "prometheiad: bad option %s\n%s", arg.c_str(), kUsage);
            return 2;
        }
    }
    if (ephemeris.empty()) {
        std::fputs(kUsage, stderr);
        return 2;
    }

    WireMap map;
    if (wire_map_path.empty()) {
        std::fprintf(stderr, "prometheiad: no --wire-map: every object will fail\n");
    } else {
        auto m = WireMap::load(wire_map_path);
        if (!m) {
            std::fprintf(stderr, "prometheiad: %s\n", m.error().message.c_str());
            return 1;
        }
        map = std::move(m).value();
    }

    const auto make_engine = [&]() -> Result<Engine> {
        auto e = Engine::open(ephemeris);
        if (!e) {
            return e.error();
        }
        for (const std::string& c : catalogs) {
            if (auto r = e.value().add_catalog(c); !r) {
                return make_error(r.error().code, c + ": " + r.error().message);
            }
        }
        if (!perturbers.empty()) {
            if (auto r = e.value().add_perturbers(perturbers); !r) {
                return make_error(r.error().code, perturbers + ": " + r.error().message);
            }
        }
        return e;
    };

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    WsServer server(options, std::move(map), make_engine);
    if (auto r = server.start(); !r) {
        std::fprintf(stderr, "prometheiad: %s\n", r.error().message.c_str());
        return 1;
    }
    std::fprintf(stderr, "prometheiad: listening on port %d, %u loop%s\n", server.port(),
                 options.threads, options.threads == 1 ? "" : "s");
    while (g_signal == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::fprintf(stderr, "prometheiad: signal %d, stopping\n", int(g_signal));
    server.stop();
    server.join();
    return 0;
}

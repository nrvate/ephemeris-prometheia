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

#include <pthread.h>

#include "tls.hpp"
#include "ws_server.hpp"

using namespace prometheia;
using namespace prometheia::server;

namespace {

constexpr const char* kUsage =
    "usage: prometheiad --ephemeris FILE [options]\n"
    "  --ephemeris FILE      JPL DE binary or SPK kernel (required)\n"
    "  --catalog FILE        EPM1 small-body catalog (repeatable, newest wins)\n"
    "  --perturbers FILE     asteroid perturber SPK kernel (e.g. sb441-n16.bsp)\n"
    "  --wire-map FILE       wire map from Astrolog's protocol specification;\n"
    "                        without one every object fails (docs/SERVER.md)\n"
    "  --bind ADDR           listen address (default: every interface)\n"
    "  --port N              default 47190; 0 picks a free port\n"
    "  --threads N           event loops, one engine each (default: hardware threads)\n"
    "  --max-cells N         objects x rows per REQUEST (default 100000)\n"
    "  --cache-mb N          result cache per loop (default 64)\n"
    "  --max-conns N         WebSockets open in total (default 10000; 0 = no cap)\n"
    "  --max-conns-per-ip N  from one address (default 64; 0 = no cap)\n"
    "  --cells-per-sec N     compute budget per address, or per token, refilling\n"
    "                        at N cells a second up to --max-cells (default 10000;\n"
    "                        0 = no budget)\n"
    "  --hello-seconds N     close a connection with no HELLO after N s (default\n"
    "                        10; 0 = never)\n"
    "  --tokens FILE         accepted tokens, one a line; a known token gets its\n"
    "                        own budget\n"
    "  --require-token       refuse a HELLO without a known token\n"
    "  --tls-cert FILE       serve wss:// with this PEM chain ...\n"
    "  --tls-key FILE        ... and key; SIGHUP reloads both\n"
    "  --drain-seconds N     on SIGTERM/SIGINT stop accepting and give answers in\n"
    "                        flight N s (default 10); a second signal exits at once\n"
    "  --verbose             log connections\n"
    "GET /healthz, /readyz and /metrics are served on the same port.\n";

bool parse_uint(const char* s, unsigned long& out) {
    char* end = nullptr;
    out = std::strtoul(s, &end, 10);
    return s[0] != '\0' && s[0] != '-' && end && *end == '\0';
}

} // namespace

int main(int argc, char** argv) {
    std::string ephemeris, perturbers, wire_map_path, tokens_path;
    std::vector<std::string> catalogs;
    WsOptions options;
    options.threads = std::max(1u, std::thread::hardware_concurrency());
    unsigned drain_seconds = 10;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "prometheiad: %s needs a value\n%s", arg.c_str(), kUsage);
                std::exit(2);
            }
            return argv[++i];
        };
        const auto number = [&](unsigned long lo, unsigned long hi) {
            unsigned long n = 0;
            if (!parse_uint(value(), n) || n < lo || n > hi) {
                std::fprintf(stderr, "prometheiad: bad value for %s\n%s", arg.c_str(), kUsage);
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
        } else if (arg == "--wire-map") {
            wire_map_path = value();
        } else if (arg == "--bind") {
            options.bind = value();
        } else if (arg == "--port") {
            options.port = int(number(0, 65535));
        } else if (arg == "--threads") {
            options.threads = unsigned(number(1, 1024));
        } else if (arg == "--max-cells") {
            options.config.max_cells = uint32_t(number(1, 0xFFFFFFFFul));
        } else if (arg == "--cache-mb") {
            options.config.cache_bytes = size_t(number(0, 65536)) << 20;
        } else if (arg == "--max-conns") {
            options.limits.max_conns = uint32_t(number(0, 0xFFFFFFFFul));
        } else if (arg == "--max-conns-per-ip") {
            options.limits.max_conns_per_addr = uint32_t(number(0, 0xFFFFFFFFul));
        } else if (arg == "--cells-per-sec") {
            options.limits.cells_per_sec = uint32_t(number(0, 0xFFFFFFFFul));
        } else if (arg == "--hello-seconds") {
            options.hello_timeout_ms = uint32_t(number(0, 3600)) * 1000;
        } else if (arg == "--tokens") {
            tokens_path = value();
        } else if (arg == "--require-token") {
            options.limits.require_token = true;
        } else if (arg == "--tls-cert") {
            options.tls_cert = value();
        } else if (arg == "--tls-key") {
            options.tls_key = value();
        } else if (arg == "--drain-seconds") {
            drain_seconds = unsigned(number(0, 3600));
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
    if (options.limits.require_token && tokens_path.empty()) {
        std::fprintf(stderr, "prometheiad: --require-token needs --tokens\n");
        return 2;
    }
    if (!options.tls_cert.empty() && !tls::available()) {
        std::fprintf(stderr, "prometheiad: this build has no TLS (OpenSSL was not found)\n");
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
    if (!tokens_path.empty()) {
        auto t = Limits::load_tokens(tokens_path);
        if (!t) {
            std::fprintf(stderr, "prometheiad: %s\n", t.error().message.c_str());
            return 1;
        }
        options.tokens = std::move(t).value();
        // Counted, never printed: the log is no place for credentials.
        std::fprintf(stderr, "prometheiad: %zu token(s) from %s%s\n", options.tokens.size(),
                     tokens_path.c_str(), options.limits.require_token ? ", required" : "");
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

    // Signals are taken by one thread in sigwait(), never by a handler: the
    // only uWS call safe from another thread is Loop::defer. Blocked before
    // any thread starts, so every thread inherits the mask.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    WsServer server(options, std::move(map), make_engine);
    if (auto r = server.start(); !r) {
        std::fprintf(stderr, "prometheiad: %s\n", r.error().message.c_str());
        return 1;
    }
    std::fprintf(stderr, "prometheiad: listening on port %d (%s, %s, %u loop%s)\n", server.port(),
                 server.tls() ? "wss://" : "ws://",
                 options.bind.empty() ? "every interface" : options.bind.c_str(), options.threads,
                 options.threads == 1 ? "" : "s");

    std::thread signal_thread([&] {
        for (;;) {
            int sig = 0;
            if (sigwait(&signals, &sig) != 0) {
                continue;
            }
            if (sig == SIGHUP) {
                if (auto r = server.reload_tls(); !r) {
                    std::fprintf(stderr, "prometheiad: SIGHUP: %s\n", r.error().message.c_str());
                } else {
                    std::fprintf(stderr, "prometheiad: SIGHUP: reloaded %s\n",
                                 options.tls_cert.c_str());
                }
                continue;
            }
            if (server.draining()) {
                std::fprintf(stderr, "prometheiad: second signal while draining; exiting now\n");
                std::_Exit(1);
            }
            std::fprintf(stderr, "prometheiad: signal %d: draining for up to %u s\n", sig,
                         drain_seconds);
            server.drain(drain_seconds);
        }
    });
    signal_thread.detach();
    server.join();
    std::fprintf(stderr, "prometheiad: stopped\n");
    return 0;
}

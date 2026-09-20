// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia-load: many concurrent connections against a v4 server for a
// while, measuring what one client in a short session cannot (docs/SERVER.md,
// "Load and soak"): throughput, latency under contention, errors by code,
// connections refused, and the server's memory and open files over time.
//
// Each connection sends HELLO once, then REQUESTs back to back, each for a
// fresh random instant between 1900 and 2100 unless --cached asks for the
// same one every time.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "ephproto.h"
#include "ws_client.hpp"

using namespace prometheia;
using namespace prometheia::server;
using Clock = std::chrono::steady_clock;

namespace {

constexpr const char* kUsage =
    "usage: prometheia-load [options]\n"
    "  --host H           default 127.0.0.1\n"
    "  --port N           default 47190\n"
    "  --conns N          concurrent connections (default 8)\n"
    "  --seconds S        how long (default 30)\n"
    "  --rows R           instants per REQUEST, daily (default 1: a chart)\n"
    "  --cached           the same instant every time (cache hits)\n"
    "  --pid PID          the server's process: sample its memory and open files\n"
    "  --report S         a progress line every S seconds (default 10)\n"
    "Exit 1 if any connection failed for a reason other than a refusal the\n"
    "server announced (HTTP 503, ERROR 6), or a message did not parse.\n";

// The chart a client sends: the Sun, Moon, planets and Pluto.
const int kBodies[] = {10, 301, 199, 299, 4, 5, 6, 7, 8, 9};

std::vector<uint8_t> frame(uint16_t type, uint32_t id, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    eph::WriteEnvelope(&out, type, id, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

struct Stats {
    std::mutex mu;
    std::vector<double> latency_ms;
    std::map<unsigned, uint64_t> errors; // ERROR code -> count
    uint64_t requests = 0;
    uint64_t connect_refused = 0; // the upgrade answered 503 (a cap)
    uint64_t failures = 0;        // anything else: a transport error, a bad message
    std::vector<std::string> failure_samples;
    std::atomic<uint64_t> done{0}; // requests answered, for the progress lines
};

void note_failure(Stats& st, const std::string& what) {
    std::lock_guard<std::mutex> lock(st.mu);
    ++st.failures;
    if (st.failure_samples.size() < 5) {
        st.failure_samples.push_back(what);
    }
}

void connection(int index, const std::string& host, int port, int rows, bool cached,
                Clock::time_point deadline, Stats& st) {
    std::mt19937_64 rng(uint64_t(index) * 0x9E3779B97F4A7C15ull + 1);
    std::uniform_real_distribution<double> when(2415020.5, 2488069.5);
    WsClient ws;
    if (auto r = ws.connect(host, port); !r) {
        if (r.error().message.find("503") != std::string::npos) {
            std::lock_guard<std::mutex> lock(st.mu);
            ++st.connect_refused;
        } else {
            note_failure(st, "connect: " + r.error().message);
        }
        return;
    }
    eph::Hello hello;
    hello.clientName = "prometheia-load/0.7.0";
    std::vector<uint8_t> payload;
    eph::EncodeHello(&payload, hello);
    if (auto r = ws.send(frame(eph::kMsgHello, 0, payload)); !r) {
        note_failure(st, "HELLO: " + r.error().message);
        return;
    }
    if (auto w = ws.receive(); !w) {
        note_failure(st, "WELCOME: " + w.error().message);
        return;
    }
    std::vector<double> local;
    uint32_t id = 1;
    while (Clock::now() < deadline) {
        eph::Request req;
        req.profiles.emplace_back();
        for (int b : kBodies) {
            eph::Object o;
            o.kind = eph::kObjBody;
            o.naif = b;
            req.objs.push_back(o);
        }
        req.start.jd1 = cached ? 2461300.5 : when(rng);
        req.nTime = uint32_t(rows);
        req.stepNs = rows > 1 ? int64_t(86400) * 1000000000 : 0;
        payload.clear();
        eph::EncodeRequest(&payload, req);
        const auto t0 = Clock::now();
        if (auto r = ws.send(frame(eph::kMsgRequest, id, payload)); !r) {
            note_failure(st, "REQUEST: " + r.error().message);
            break;
        }
        bool open = true;
        for (;;) {
            auto m = ws.receive(60000);
            if (!m) {
                note_failure(st, "answer: " + m.error().message);
                open = false;
                break;
            }
            eph::Envelope env{};
            std::string why;
            if (eph::ParseFrame(m.value().data(), m.value().size(), &env, &why) != eph::kOk) {
                note_failure(st, "a message that does not parse: " + why);
                open = false;
                break;
            }
            if (env.requestId != id) {
                continue;
            }
            const uint8_t* p = m.value().data() + eph::kEnvelopeSize;
            if (env.type == eph::kMsgError) {
                eph::Error e;
                eph::ParseError(p, env.payloadLen, &e, &why);
                {
                    std::lock_guard<std::mutex> lock(st.mu);
                    ++st.errors[e.code];
                }
                if (e.code == eph::kErrRateLimited) {
                    // Honour the server's retry hint, as a client should.
                    std::this_thread::sleep_for(std::chrono::milliseconds(e.retryAfterMs));
                }
                break;
            }
            if (env.type == eph::kMsgData) {
                eph::DataChunk d;
                eph::ParseData(p, env.payloadLen, &d, &why);
                if (d.flags & eph::kChunkLast) {
                    local.push_back(
                        std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
                    ++st.done;
                    break;
                }
            }
        }
        if (!open) {
            break;
        }
        ++id;
    }
    ws.close();
    std::lock_guard<std::mutex> lock(st.mu);
    st.requests += local.size();
    st.latency_ms.insert(st.latency_ms.end(), local.begin(), local.end());
}

// VmRSS in MB and the count of open file descriptors, or -1 when unreadable.
std::pair<double, long> sample(long pid) {
    double rss = -1.0;
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    for (std::string line; std::getline(f, line);) {
        if (line.rfind("VmRSS:", 0) == 0) {
            rss = std::strtod(line.c_str() + 6, nullptr) / 1024.0;
        }
    }
    long fds = -1;
    if (DIR* d = opendir(("/proc/" + std::to_string(pid) + "/fd").c_str())) {
        fds = 0;
        while (const dirent* e = readdir(d)) {
            fds += e->d_name[0] != '.';
        }
        closedir(d);
    }
    return {rss, fds};
}

double percentile(std::vector<double>& v, double q) {
    if (v.empty()) {
        return 0.0;
    }
    const size_t k = std::min(v.size() - 1, size_t(q * double(v.size())));
    std::nth_element(v.begin(), v.begin() + long(k), v.end());
    return v[k];
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = eph::kDefaultPort, conns = 8, seconds = 30, rows = 1, report = 10;
    long pid = 0;
    bool cached = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n%s", arg.c_str(), kUsage);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--host") {
            host = value();
        } else if (arg == "--port") {
            port = std::atoi(value());
        } else if (arg == "--conns") {
            conns = std::max(1, std::atoi(value()));
        } else if (arg == "--seconds") {
            seconds = std::max(1, std::atoi(value()));
        } else if (arg == "--rows") {
            rows = std::max(1, std::atoi(value()));
        } else if (arg == "--cached") {
            cached = true;
        } else if (arg == "--pid") {
            pid = std::atol(value());
        } else if (arg == "--report") {
            report = std::max(1, std::atoi(value()));
        } else {
            std::fprintf(stderr, "bad option %s\n%s", arg.c_str(), kUsage);
            return 2;
        }
    }

    Stats st;
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::seconds(seconds);
    std::vector<std::thread> threads;
    threads.reserve(size_t(conns));
    for (int c = 0; c < conns; ++c) {
        threads.emplace_back(connection, c, host, port, rows, cached, deadline, std::ref(st));
    }

    double rss_first = -1.0, rss_peak = -1.0, rss_last = -1.0;
    long fds_first = -1, fds_peak = -1, fds_last = -1;
    int tick = 0;
    uint64_t done_before = 0;
    while (Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ++tick;
        if (pid > 0) {
            const auto [rss, fds] = sample(pid);
            if (rss_first < 0.0) {
                rss_first = rss;
                fds_first = fds;
            }
            rss_peak = std::max(rss_peak, rss);
            fds_peak = std::max(fds_peak, fds);
            rss_last = rss;
            fds_last = fds;
        }
        if (tick % report == 0) {
            const uint64_t done = st.done.load();
            std::printf("t=%4ds  %8.0f req/s  answered %llu", tick,
                        double(done - done_before) / report, (unsigned long long)done);
            if (pid > 0) {
                std::printf("  rss %.1f MB  fds %ld", rss_last, fds_last);
            }
            std::printf("\n");
            std::fflush(stdout);
            done_before = done;
        }
    }
    for (std::thread& t : threads) {
        t.join();
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    if (pid > 0) {
        // After every connection closed: what the server kept.
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto [rss, fds] = sample(pid);
        rss_last = rss;
        fds_last = fds;
    }

    std::printf("\nconnections %d, %d s, %d row%s per request%s\n", conns, seconds, rows,
                rows == 1 ? "" : "s", cached ? ", cached" : "");
    std::printf("answered   %llu (%.0f req/s)\n", (unsigned long long)st.requests,
                double(st.requests) / elapsed);
    std::printf("latency    p50 %.2f  p90 %.2f  p99 %.2f  max %.2f ms\n",
                percentile(st.latency_ms, 0.50), percentile(st.latency_ms, 0.90),
                percentile(st.latency_ms, 0.99), percentile(st.latency_ms, 1.0));
    std::printf("errors    ");
    if (st.errors.empty()) {
        std::printf(" none");
    }
    for (const auto& [code, n] : st.errors) {
        std::printf(" ERROR %u x %llu", code, (unsigned long long)n);
    }
    std::printf("\nrefused    %llu at the upgrade (503)\n", (unsigned long long)st.connect_refused);
    std::printf("failures   %llu\n", (unsigned long long)st.failures);
    for (const std::string& f : st.failure_samples) {
        std::printf("  %s\n", f.c_str());
    }
    if (pid > 0) {
        std::printf("server     rss %.1f -> peak %.1f -> %.1f MB after close; fds %ld -> peak %ld "
                    "-> %ld\n",
                    rss_first, rss_peak, rss_last, fds_first, fds_peak, fds_last);
    }
    return st.failures ? 1 : 0;
}

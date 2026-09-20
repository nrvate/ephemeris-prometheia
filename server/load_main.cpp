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
//
// Counting arrivals cannot see a wrong answer, so one request in --canary-every
// re-asks an instant whose answer was taken from this same server before the
// load started, and grades the numbers. That is a consistency check and not a
// correctness one -- the baseline is the server's own idle answer -- but it is
// the bug load testing exists to find: contention changing an answer.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <limits>
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
    "  --canaries N       instants graded under load (default 8, 0 to disable)\n"
    "  --canary-every K   one request in K is a canary (default 16)\n"
    "  --memory-bound MB  fail if the server's resident memory grows by more\n"
    "                     than MB, AND if its result cache is not really\n"
    "                     filling (/metrics cache hits must rise). Needs\n"
    "                     --pid; refuses to run if /metrics is unreadable,\n"
    "                     because the bound alone passes a server that\n"
    "                     caches nothing at all\n"
    "  --sabotage WHAT    corrupt this client's own copy of a canary answer\n"
    "                     before grading it, so the grading can be shown to\n"
    "                     go red: values, shape or identity. For\n"
    "                     tools/check/loadselftest.py; never for a real run\n"
    "  --chunk-rows N     the load asks for answers in chunks of N rows; the\n"
    "                     baseline always takes the server's own chunking, so\n"
    "                     this grades an answer against a differently chunked\n"
    "                     copy of itself (default: the server chooses, both)\n"
    "Exit 1 if any connection failed for a reason other than a refusal the\n"
    "server announced (HTTP 503, ERROR 6), a message did not parse, or a\n"
    "canary answer differed from the same server's answer before the load.\n";

std::vector<uint8_t> frame(uint16_t type, uint32_t id, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    eph::WriteEnvelope(&out, type, id, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// The chart a client sends: the Sun, Moon, planets and Pluto.
const int kBodies[] = {10, 301, 199, 299, 4, 5, 6, 7, 8, 9};

// One server's whole answer to one request, reassembled from its chunks into
// a layout that does not depend on how the server chose to chunk it: a server
// under load may split an answer differently from the same server when idle,
// and that is not a difference in the answer.
struct Answer {
    uint16_t nObj = 0;
    uint32_t totalRows = 0, columnsPresent = 0;
    int cols = 0;
    std::vector<double> values;    // nObj x totalRows x cols
    std::vector<int32_t> resolved; // meta[i].resolvedNaif, chunk 0
    std::string shape() const {
        return std::to_string(nObj) + " objects x " + std::to_string(totalRows) + " rows x " +
               std::to_string(cols) + " cols, columns 0x" + std::to_string(columnsPresent);
    }
};

// Same number, counting a NaN as equal to a NaN: an object the server cannot
// answer for is a canonical NaN in every row, and that is a stable answer.
bool same(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) || a == b;
}

enum class Ask {
    kOk,          // a complete answer, assembled into *out
    kServerError, // the server said ERROR; *code and *retry_ms are set
    kBroken,      // the connection or the framing failed; *why says how
};

// Sends one REQUEST and reads until the last chunk or an ERROR.
Ask ask(WsClient& ws, uint32_t id, double jd, int rows, int chunk_rows, Answer* out, unsigned* code,
        unsigned* retry_ms, std::string* why) {
    eph::Request req;
    req.profiles.emplace_back();
    for (int b : kBodies) {
        eph::Object o;
        o.kind = eph::kObjBody;
        o.naif = b;
        req.objs.push_back(o);
    }
    req.start.jd1 = jd;
    req.chunkRows = uint32_t(chunk_rows);
    req.nTime = uint32_t(rows);
    req.stepNs = rows > 1 ? int64_t(86400) * 1000000000 : 0;
    std::vector<uint8_t> payload;
    eph::EncodeRequest(&payload, req);
    if (auto r = ws.send(frame(eph::kMsgRequest, id, payload)); !r) {
        *why = "REQUEST: " + r.error().message;
        return Ask::kBroken;
    }
    *out = Answer{};
    bool sized = false;
    for (;;) {
        auto m = ws.receive(60000);
        if (!m) {
            *why = "answer: " + m.error().message;
            return Ask::kBroken;
        }
        eph::Envelope env{};
        if (eph::ParseFrame(m.value().data(), m.value().size(), &env, why) != eph::kOk) {
            *why = "a message that does not parse: " + *why;
            return Ask::kBroken;
        }
        if (env.requestId != id) {
            continue;
        }
        const uint8_t* p = m.value().data() + eph::kEnvelopeSize;
        if (env.type == eph::kMsgError) {
            eph::Error e;
            eph::ParseError(p, env.payloadLen, &e, why);
            *code = e.code;
            *retry_ms = e.retryAfterMs;
            return Ask::kServerError;
        }
        if (env.type != eph::kMsgData) {
            continue;
        }
        eph::DataChunk d;
        if (eph::ParseData(p, env.payloadLen, &d, why) != eph::kOk) {
            *why = "DATA does not parse: " + *why;
            return Ask::kBroken;
        }
        if (!sized) {
            out->nObj = d.nObj;
            out->totalRows = d.totalRows;
            out->columnsPresent = d.columnsPresent;
            out->cols = d.Cols();
            out->values.assign(size_t(d.nObj) * d.totalRows * size_t(d.Cols()),
                               std::numeric_limits<double>::quiet_NaN());
            for (const eph::Meta& mt : d.meta) {
                out->resolved.push_back(mt.resolvedNaif);
            }
            sized = true;
        }
        // Place the chunk's rows where they belong, so two different
        // chunkings of one answer compare equal.
        if (d.nObj == out->nObj && d.Cols() == out->cols &&
            uint64_t(d.iTime) + d.nRows <= out->totalRows &&
            d.values.size() >= size_t(d.nObj) * d.nRows * size_t(d.Cols())) {
            for (uint32_t o = 0; o < d.nObj; ++o) {
                for (uint32_t r = 0; r < d.nRows; ++r) {
                    const double* from = &d.values[(size_t(o) * d.nRows + r) * size_t(d.Cols())];
                    double* to = &out->values[(size_t(o) * out->totalRows + d.iTime + r) *
                                              size_t(out->cols)];
                    std::copy(from, from + out->cols, to);
                }
            }
        }
        if (d.flags & eph::kChunkLast) {
            return Ask::kOk;
        }
    }
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
    // The canaries: answers graded against the same server's answer before
    // the load started.
    uint64_t canary_checks = 0, canary_mismatch = 0;
    double canary_max_delta = 0.0; // largest |difference| seen, mismatch or not
    std::vector<std::string> canary_samples;
};

void note_failure(Stats& st, const std::string& what) {
    std::lock_guard<std::mutex> lock(st.mu);
    ++st.failures;
    if (st.failure_samples.size() < 5) {
        st.failure_samples.push_back(what);
    }
}

// Grades one canary answer against its baseline, and records how far off it
// was. The magnitude matters as much as the verdict: a last-bit difference
// and an answer for the wrong body are both "not equal", and they are not the
// same finding.
// Damages one canary answer the way a broken server would, so that the
// grading above can be shown to catch each kind. This lives beside grade()
// and inside the binary that runs, rather than in a copy of the check: what
// gets falsified has to be the code that ships.
void sabotage(const std::string& what, Answer* a) {
    if (what == "values" && !a->values.empty()) {
        a->values[0] += 1e-6;
    } else if (what == "shape") {
        a->totalRows += 1;
    } else if (what == "identity" && !a->resolved.empty()) {
        a->resolved[0] = 499; // Mars, where the Sun was asked for
    }
}

void grade(Stats& st, int canary, double jd, const Answer& got, const Answer& want) {
    std::string bad;
    double worst = 0.0;
    if (got.nObj != want.nObj || got.totalRows != want.totalRows || got.cols != want.cols ||
        got.columnsPresent != want.columnsPresent) {
        bad = "shape: " + got.shape() + ", idle it was " + want.shape();
    } else {
        for (size_t i = 0; i < want.values.size(); ++i) {
            if (!same(got.values[i], want.values[i])) {
                const double d = std::fabs(got.values[i] - want.values[i]);
                if (bad.empty() || d > worst) {
                    const size_t obj = i / (size_t(want.totalRows) * size_t(want.cols));
                    const size_t col = i % size_t(want.cols);
                    bad = "object " +
                          std::to_string(kBodies[std::min(obj, std::size(kBodies) - 1)]) +
                          " column " + std::to_string(col) + ": " + std::to_string(got.values[i]) +
                          ", idle it was " + std::to_string(want.values[i]);
                }
                worst = std::max(worst, d);
            }
        }
        // The objects must come back as the ones that were asked for, in
        // order. This needs no baseline: it is the answer disagreeing with
        // its own request, which is what a contaminated answer looks like.
        for (size_t i = 0; i < got.resolved.size() && i < std::size(kBodies); ++i) {
            if (got.resolved[i] != eph::kNaifNone && got.resolved[i] != kBodies[i]) {
                bad = "object " + std::to_string(i) + " came back as NAIF " +
                      std::to_string(got.resolved[i]) + ", asked for " + std::to_string(kBodies[i]);
            }
        }
    }
    std::lock_guard<std::mutex> lock(st.mu);
    ++st.canary_checks;
    st.canary_max_delta = std::max(st.canary_max_delta, worst);
    if (!bad.empty()) {
        ++st.canary_mismatch;
        if (st.canary_samples.size() < 5) {
            char jds[32];
            std::snprintf(jds, sizeof jds, "%.6f", jd);
            st.canary_samples.push_back("canary " + std::to_string(canary) + " (JD " + jds + ") " +
                                        bad);
        }
    }
}

// Connects and says HELLO. Empty on success, else why not.
std::string open_session(WsClient& ws, const std::string& host, int port) {
    if (auto r = ws.connect(host, port); !r) {
        return "connect: " + r.error().message;
    }
    eph::Hello hello;
    hello.clientName = "prometheia-load/0.7.0";
    std::vector<uint8_t> payload;
    eph::EncodeHello(&payload, hello);
    if (auto r = ws.send(frame(eph::kMsgHello, 0, payload)); !r) {
        return "HELLO: " + r.error().message;
    }
    if (auto w = ws.receive(); !w) {
        return "WELCOME: " + w.error().message;
    }
    return "";
}

void connection(int index, const std::string& host, int port, int rows, int chunk_rows, bool cached,
                const std::vector<double>& canary_jd, const std::vector<Answer>& canary_want,
                int canary_every, const std::string& sabotage_what, Clock::time_point deadline,
                Stats& st) {
    std::mt19937_64 rng(uint64_t(index) * 0x9E3779B97F4A7C15ull + 1);
    std::uniform_real_distribution<double> when(2415020.5, 2488069.5);
    WsClient ws;
    if (const std::string why = open_session(ws, host, port); !why.empty()) {
        if (why.find("503") != std::string::npos) {
            std::lock_guard<std::mutex> lock(st.mu);
            ++st.connect_refused;
        } else {
            note_failure(st, why);
        }
        return;
    }
    std::vector<double> local;
    uint32_t id = 1;
    uint64_t sent = 0;
    while (Clock::now() < deadline) {
        // Every canary_every'th request re-asks an instant whose answer was
        // taken from this same server before the load started.
        const bool is_canary =
            !canary_want.empty() && canary_every > 0 && sent % uint64_t(canary_every) == 0;
        const int canary =
            is_canary ? int((sent / uint64_t(canary_every)) % canary_want.size()) : 0;
        const double jd = is_canary ? canary_jd[size_t(canary)] : (cached ? 2461300.5 : when(rng));
        ++sent;
        Answer got;
        unsigned code = 0, retry_ms = 0;
        std::string why;
        const auto t0 = Clock::now();
        // A canary asks exactly what the load asks, --rows and all, so that a
        // run with a chunked answer grades a chunked answer.
        const Ask outcome = ask(ws, id, jd, rows, chunk_rows, &got, &code, &retry_ms, &why);
        if (outcome == Ask::kBroken) {
            note_failure(st, why);
            break;
        }
        if (outcome == Ask::kServerError) {
            {
                std::lock_guard<std::mutex> lock(st.mu);
                ++st.errors[code];
            }
            if (code == eph::kErrRateLimited) {
                // Honour the server's retry hint, as a client should.
                std::this_thread::sleep_for(std::chrono::milliseconds(retry_ms));
            }
            ++id;
            continue;
        }
        local.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        ++st.done;
        if (is_canary) {
            if (!sabotage_what.empty()) {
                sabotage(sabotage_what, &got);
            }
            grade(st, canary, jd, got, canary_want[size_t(canary)]);
        }
        ++id;
    }
    ws.close();
    std::lock_guard<std::mutex> lock(st.mu);
    st.requests += local.size();
    st.latency_ms.insert(st.latency_ms.end(), local.begin(), local.end());
}

// One Prometheus counter from the server's /metrics, or -1 when it is not
// there. The whole point of reading it is the pairing below: a bound on
// memory is passed by a server that stores nothing, so something has to say
// the cache is really filling.
double counter(const std::string& host, int port, const char* name) {
    auto body = WsClient::http_get(host, port, "/metrics");
    if (!body) {
        return -1.0;
    }
    const std::string needle = std::string("\n") + name + " ";
    const size_t at = ("\n" + body.value()).find(needle);
    if (at == std::string::npos) {
        return -1.0;
    }
    return std::strtod(body.value().c_str() + at + needle.size() - 1, nullptr);
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
    int canaries = 8, canary_every = 16, chunk_rows = 0;
    double memory_bound_mb = -1.0;
    std::string sabotage_what;
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
        } else if (arg == "--canaries") {
            canaries = std::max(0, std::atoi(value()));
        } else if (arg == "--memory-bound") {
            memory_bound_mb = std::strtod(value(), nullptr);
        } else if (arg == "--sabotage") {
            sabotage_what = value();
            if (sabotage_what != "values" && sabotage_what != "shape" &&
                sabotage_what != "identity") {
                std::fprintf(stderr, "--sabotage takes values, shape or identity\n");
                return 2;
            }
        } else if (arg == "--chunk-rows") {
            chunk_rows = std::max(0, std::atoi(value()));
        } else if (arg == "--canary-every") {
            canary_every = std::max(1, std::atoi(value()));
        } else {
            std::fprintf(stderr, "bad option %s\n%s", arg.c_str(), kUsage);
            return 2;
        }
    }

    // Both halves of the memory check, or neither. A bound on its own is
    // passed by a server whose cache stores nothing -- the same way a row
    // that grades two observers as equal is passed by two observers that
    // never arrived. So refuse the run rather than assert half of it.
    double hits_before = -1.0;
    if (memory_bound_mb >= 0.0) {
        if (pid <= 0) {
            std::fprintf(stderr, "--memory-bound needs --pid: nothing else samples the server's "
                                 "resident memory\n");
            return 2;
        }
        hits_before = counter(host, port, "prometheiad_cache_hits_total");
        if (hits_before < 0.0) {
            std::fprintf(stderr,
                         "--memory-bound needs the server's /metrics, and "
                         "prometheiad_cache_hits_total was not readable at %s:%d. A bound on "
                         "memory alone is passed by a server that caches nothing, so this run "
                         "would assert half of the check and report a pass.\n",
                         host.c_str(), port);
            return 2;
        }
    }

    // The baseline: what this server answers for the canary instants with
    // nothing else asking. The instants are fixed, so two runs and two
    // servers grade the same questions.
    std::vector<double> canary_jd;
    std::vector<Answer> canary_want;
    if (canaries > 0) {
        std::mt19937_64 rng(0xCA11A21Eull);
        std::uniform_real_distribution<double> when(2415020.5, 2488069.5);
        for (int i = 0; i < canaries; ++i) {
            canary_jd.push_back(when(rng));
        }
        WsClient ws;
        if (const std::string why = open_session(ws, host, port); !why.empty()) {
            std::fprintf(stderr, "the canary baseline could not be taken: %s\n", why.c_str());
            return 2;
        }
        for (int i = 0; i < canaries; ++i) {
            // chunkRows 0: the baseline takes whatever chunking the server
            // prefers, so that --chunk-rows grades across two of them.
            Answer a;
            unsigned code = 0, retry_ms = 0;
            std::string why;
            const Ask outcome =
                ask(ws, uint32_t(i + 1), canary_jd[size_t(i)], rows, 0, &a, &code, &retry_ms, &why);
            if (outcome != Ask::kOk) {
                std::fprintf(stderr, "the canary baseline could not be taken: %s\n",
                             outcome == Ask::kServerError
                                 ? ("the server answered ERROR " + std::to_string(code)).c_str()
                                 : why.c_str());
                return 2;
            }
            canary_want.push_back(std::move(a));
        }
        ws.close();
        std::printf("baseline   %d canary instants, %s\n", canaries,
                    canary_want.front().shape().c_str());
    }

    double rss_first = -1.0, rss_peak = -1.0, rss_last = -1.0;
    long fds_first = -1, fds_peak = -1, fds_last = -1;
    if (pid > 0) {
        // Before the load, not a second into it: a small --cache-mb can be
        // full within the first second, and growth measured from there is
        // growth already missed.
        const auto [rss, fds] = sample(pid);
        rss_first = rss_peak = rss_last = rss;
        fds_first = fds_peak = fds_last = fds;
    }
    Stats st;
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::seconds(seconds);
    std::vector<std::thread> threads;
    threads.reserve(size_t(conns));
    for (int c = 0; c < conns; ++c) {
        threads.emplace_back(connection, c, host, port, rows, chunk_rows, cached,
                             std::cref(canary_jd), std::cref(canary_want), canary_every,
                             std::cref(sabotage_what), deadline, std::ref(st));
    }

    int tick = 0;
    uint64_t done_before = 0;
    while (Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ++tick;
        if (pid > 0) {
            const auto [rss, fds] = sample(pid);
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
    if (canaries > 0) {
        std::printf("canaries   %llu answers graded, %llu differed",
                    (unsigned long long)st.canary_checks, (unsigned long long)st.canary_mismatch);
        if (st.canary_mismatch) {
            std::printf(", worst |difference| %.17g", st.canary_max_delta);
        }
        std::printf("\n");
        for (const std::string& c : st.canary_samples) {
            std::printf("  %s\n", c.c_str());
        }
        if (st.canary_checks == 0) {
            std::printf("  nothing was graded: no canary request completed\n");
        }
    }
    if (pid > 0) {
        std::printf("server     rss %.1f -> peak %.1f -> %.1f MB after close; fds %ld -> peak %ld "
                    "-> %ld\n",
                    rss_first, rss_peak, rss_last, fds_first, fds_peak, fds_last);
    }
    bool memory_failed = false;
    if (memory_bound_mb >= 0.0) {
        const double grew = rss_last - rss_first;
        const double hits_after = counter(host, port, "prometheiad_cache_hits_total");
        const double hits = hits_after - hits_before;
        std::printf("memory     grew %.1f MB against a bound of %.1f; cache hits rose by %.0f\n",
                    grew, memory_bound_mb, hits);
        if (grew > memory_bound_mb) {
            std::printf("  OVER: resident memory grew %.1f MB, more than the %.1f MB bound\n", grew,
                        memory_bound_mb);
            memory_failed = true;
        }
        // The pairing. Without this a --cache-mb 0 server grows by nothing
        // and passes the bound while caching nothing at all.
        if (hits <= 0.0) {
            std::printf("  NOT CACHING: the cache answered nothing during the run, so the bound "
                        "above says only that an empty cache stays empty. The canaries alone "
                        "re-asked %llu instants.\n",
                        (unsigned long long)st.canary_checks);
            memory_failed = true;
        }
    }

    // A canary that differed is a failure of the run, not a statistic: the
    // server answered, and answered differently under load than it does
    // idle. Zero graded answers is one too, when canaries were asked for --
    // silence is not a pass.
    return (st.failures || st.canary_mismatch || (canaries > 0 && st.canary_checks == 0) ||
            memory_failed)
               ? 1
               : 0;
}

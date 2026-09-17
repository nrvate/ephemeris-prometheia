// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheiad's counters: one Metrics per event loop, written by that loop
// and read by any loop serving /metrics (atomics, so the read is safe),
// rendered as Prometheus text summed over the loops. Nothing here names a
// client or a request.
#ifndef PROMETHEIA_SERVER_METRICS_HPP
#define PROMETHEIA_SERVER_METRICS_HPP

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace prometheia::server {

// Upper bounds of the compute-time histogram, in milliseconds.
inline constexpr double kComputeBucketsMs[] = {1, 5, 10, 50, 100, 250, 500, 1000, 5000};
inline constexpr int kComputeBuckets =
    int(sizeof(kComputeBucketsMs) / sizeof(kComputeBucketsMs[0]));
inline constexpr int kErrorCodes = 13; // index 0 (other) and protocol codes 1-12

struct Metrics {
    using Counter = std::atomic<uint64_t>;
    Counter connections_open{0};
    Counter connections_total{0};
    Counter refused_connections{0};
    Counter hello_timeouts{0};
    Counter hellos{0};
    Counter requests{0};
    Counter cells_computed{0};
    Counter cache_hits{0};
    Counter cache_misses{0};
    Counter bytes_sent{0};
    Counter backpressure_waits{0};
    Counter errors[kErrorCodes]{};
    Counter compute_count{0};
    Counter compute_micros{0};
    Counter compute_buckets[kComputeBuckets]{};

    void error(int code) { ++errors[code > 0 && code < kErrorCodes ? code : 0]; }
    void computed(uint64_t cells, double ms);
};

struct ServerInfo {
    const char* server_version = "";
    unsigned protocol = 0;
    bool tls = false;
    bool draining = false;
};

std::string metrics_text(const std::vector<const Metrics*>& loops, const ServerInfo& info);

} // namespace prometheia::server

#endif // PROMETHEIA_SERVER_METRICS_HPP

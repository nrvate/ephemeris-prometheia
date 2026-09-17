// SPDX-License-Identifier: GPL-2.0-or-later
#include "metrics.hpp"

#include <cinttypes>
#include <cstdio>

namespace prometheia::server {

void Metrics::computed(uint64_t cells, double ms) {
    ++cache_misses;
    cells_computed += cells;
    ++compute_count;
    compute_micros += uint64_t(ms * 1000.0);
    for (int b = 0; b < kComputeBuckets; ++b) {
        if (ms <= kComputeBucketsMs[b]) {
            ++compute_buckets[b];
        }
    }
}

std::string metrics_text(const std::vector<const Metrics*>& loops, const ServerInfo& info) {
    const auto sum = [&](const Metrics::Counter Metrics::*field) {
        uint64_t v = 0;
        for (const Metrics* m : loops) {
            v += (m->*field).load();
        }
        return v;
    };
    std::string out;
    char line[512];
    const auto metric = [&](const char* name, const char* type, const char* help, uint64_t v) {
        std::snprintf(line, sizeof line, "# HELP %s %s\n# TYPE %s %s\n%s %" PRIu64 "\n", name, help,
                      name, type, name, v);
        out += line;
    };
    std::snprintf(line, sizeof line,
                  "# HELP prometheiad_build_info Server version, protocol and transport.\n"
                  "# TYPE prometheiad_build_info gauge\n"
                  "prometheiad_build_info{server=\"%s\",protocol=\"%u\",tls=\"%d\"} 1\n",
                  info.server_version, info.protocol, info.tls ? 1 : 0);
    out += line;
    metric("prometheiad_connections_open", "gauge", "WebSocket connections open.",
           sum(&Metrics::connections_open));
    metric("prometheiad_connections_total", "counter", "WebSocket connections accepted.",
           sum(&Metrics::connections_total));
    metric("prometheiad_refused_connections_total", "counter",
           "Connections refused at the upgrade by the connection caps.",
           sum(&Metrics::refused_connections));
    metric("prometheiad_hello_timeouts_total", "counter",
           "Connections closed for sending no HELLO in time.", sum(&Metrics::hello_timeouts));
    metric("prometheiad_hellos_total", "counter", "HELLOs answered.", sum(&Metrics::hellos));
    metric("prometheiad_requests_total", "counter", "REQUESTs answered with data.",
           sum(&Metrics::requests));
    metric("prometheiad_cells_computed_total", "counter",
           "Cells (objects x rows) computed, cache misses only.", sum(&Metrics::cells_computed));
    metric("prometheiad_cache_hits_total", "counter", "REQUESTs answered from the result cache.",
           sum(&Metrics::cache_hits));
    metric("prometheiad_cache_misses_total", "counter", "REQUESTs computed.",
           sum(&Metrics::cache_misses));
    metric("prometheiad_bytes_sent_total", "counter", "Bytes of WebSocket messages sent.",
           sum(&Metrics::bytes_sent));
    metric("prometheiad_backpressure_waits_total", "counter",
           "Times a connection's replies waited for the client to read.",
           sum(&Metrics::backpressure_waits));

    out += "# HELP prometheiad_errors_total ERRORs sent, by protocol code (0 other).\n"
           "# TYPE prometheiad_errors_total counter\n";
    for (int code = 0; code < kErrorCodes; ++code) {
        uint64_t v = 0;
        for (const Metrics* m : loops) {
            v += m->errors[code].load();
        }
        std::snprintf(line, sizeof line, "prometheiad_errors_total{code=\"%d\"} %" PRIu64 "\n",
                      code, v);
        out += line;
    }

    out += "# HELP prometheiad_compute_seconds Time computing one REQUEST (cache misses).\n"
           "# TYPE prometheiad_compute_seconds histogram\n";
    for (int b = 0; b < kComputeBuckets; ++b) {
        uint64_t v = 0;
        for (const Metrics* m : loops) {
            v += m->compute_buckets[b].load();
        }
        std::snprintf(line, sizeof line,
                      "prometheiad_compute_seconds_bucket{le=\"%g\"} %" PRIu64 "\n",
                      kComputeBucketsMs[b] / 1000.0, v);
        out += line;
    }
    const uint64_t count = sum(&Metrics::compute_count);
    std::snprintf(line, sizeof line,
                  "prometheiad_compute_seconds_bucket{le=\"+Inf\"} %" PRIu64 "\n"
                  "prometheiad_compute_seconds_sum %.6f\n"
                  "prometheiad_compute_seconds_count %" PRIu64 "\n",
                  count, double(sum(&Metrics::compute_micros)) / 1e6, count);
    out += line;
    metric("prometheiad_draining", "gauge", "1 while draining after SIGTERM.",
           info.draining ? 1 : 0);
    return out;
}

} // namespace prometheia::server

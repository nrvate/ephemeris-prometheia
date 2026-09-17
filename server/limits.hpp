// SPDX-License-Identifier: GPL-2.0-or-later
//
// Server-wide limits, shared by every event loop behind one mutex (taken
// once per connection and once per REQUEST, nothing beside a computation):
//   - connection caps, in total and per peer address, checked at the
//     WebSocket upgrade;
//   - access tokens (HELLO's version-3 token), optionally required;
//   - a compute budget per peer address, or per token when HELLO gave a
//     known one: a bucket of cells refilling at cells_per_sec up to the
//     REQUEST cell bound, so one full request is always possible from a
//     full bucket. Over budget, the REQUEST is answered with ERROR 6.
#ifndef PROMETHEIA_SERVER_LIMITS_HPP
#define PROMETHEIA_SERVER_LIMITS_HPP

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "prometheia/error.hpp"

namespace prometheia::server {

struct LimitsConfig {
    uint32_t max_conns = 10000;       // WebSockets open in total; 0 = no cap
    uint32_t max_conns_per_addr = 64; // from one address; 0 = no cap
    uint32_t cells_per_sec = 10000;   // budget refill; 0 = no budget
    uint32_t burst_cells = 100000;    // budget capacity (the REQUEST cell bound)
    bool require_token = false;
};

class Limits {
public:
    using Clock = std::chrono::steady_clock;

    Limits(LimitsConfig config, std::unordered_set<std::string> tokens = {});

    // Accepted tokens, one a line; blank lines and '#' lines are skipped,
    // surrounding blanks trimmed. A token longer than 128 bytes is an error.
    static Result<std::unordered_set<std::string>> load_tokens(const std::string& path);

    // Takes a place for a connection from `addr`, or returns why not.
    const char* admit(const std::string& addr);
    void release(const std::string& addr);

    bool token_known(const std::string& token) const;
    bool require_token() const { return config_.require_token; }
    const LimitsConfig& config() const { return config_; }

    // Charges `cells` to the budget `key`. Returns 0 when charged, else the
    // seconds until the budget would cover it (nothing is charged then).
    double charge(const std::string& key, uint64_t cells, Clock::time_point now = Clock::now());

    uint32_t connections() const;

private:
    struct Bucket {
        double cells;
        Clock::time_point t;
    };
    LimitsConfig config_;
    std::unordered_set<std::string> tokens_;
    mutable std::mutex mu_;
    uint32_t conns_ = 0;
    std::unordered_map<std::string, uint32_t> conns_by_addr_;
    std::unordered_map<std::string, Bucket> buckets_;
};

} // namespace prometheia::server

#endif // PROMETHEIA_SERVER_LIMITS_HPP

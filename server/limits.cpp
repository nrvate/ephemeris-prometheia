// SPDX-License-Identifier: GPL-2.0-or-later
#include "limits.hpp"

#include <algorithm>
#include <fstream>

namespace prometheia::server {

constexpr size_t kTokenMaxBytes = 128; // eph::kTokenMax

Limits::Limits(LimitsConfig config, std::unordered_set<std::string> tokens)
    : config_(config), tokens_(std::move(tokens)) {}

Result<std::unordered_set<std::string>> Limits::load_tokens(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return make_error(ErrorCode::IoError, "cannot read tokens file " + path);
    }
    std::unordered_set<std::string> tokens;
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        const size_t b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos || line[b] == '#') {
            continue;
        }
        const size_t e = line.find_last_not_of(" \t\r");
        std::string token = line.substr(b, e - b + 1);
        if (token.size() > kTokenMaxBytes) {
            return make_error(ErrorCode::FormatError, path + " line " + std::to_string(line_no) +
                                                          ": a token is longer than 128 bytes");
        }
        tokens.insert(std::move(token));
    }
    return tokens;
}

const char* Limits::admit(const std::string& addr) {
    std::lock_guard lock(mu_);
    if (config_.max_conns && conns_ >= config_.max_conns) {
        return "the server is at its connection limit";
    }
    uint32_t& from = conns_by_addr_[addr];
    if (config_.max_conns_per_addr && from >= config_.max_conns_per_addr) {
        if (from == 0) {
            conns_by_addr_.erase(addr);
        }
        return "too many connections from this address";
    }
    ++conns_;
    ++from;
    return nullptr;
}

void Limits::release(const std::string& addr) {
    std::lock_guard lock(mu_);
    if (conns_) {
        --conns_;
    }
    const auto it = conns_by_addr_.find(addr);
    if (it != conns_by_addr_.end() && --it->second == 0) {
        conns_by_addr_.erase(it);
    }
}

bool Limits::token_known(const std::string& token) const {
    return !token.empty() && tokens_.count(token) != 0;
}

uint32_t Limits::connections() const {
    std::lock_guard lock(mu_);
    return conns_;
}

double Limits::charge(const std::string& key, uint64_t cells, Clock::time_point now) {
    if (!config_.cells_per_sec) {
        return 0.0;
    }
    const double capacity = double(config_.burst_cells);
    const double rate = double(config_.cells_per_sec);
    std::lock_guard lock(mu_);
    auto it = buckets_.find(key);
    if (it == buckets_.end()) {
        it = buckets_.emplace(key, Bucket{capacity, now}).first;
    }
    Bucket& b = it->second;
    const double dt = std::max(0.0, std::chrono::duration<double>(now - b.t).count());
    b.cells = std::min(capacity, b.cells + dt * rate);
    b.t = now;
    if (double(cells) <= b.cells) {
        b.cells -= double(cells);
        return 0.0;
    }
    const double wait = (double(cells) - b.cells) / rate;
    // Buckets that have refilled carry no information: drop them now and
    // then so the map cannot grow without bound.
    if (buckets_.size() > 100000) {
        for (auto i = buckets_.begin(); i != buckets_.end();) {
            const double idle = std::chrono::duration<double>(now - i->second.t).count();
            i = i->second.cells + idle * rate >= capacity ? buckets_.erase(i) : std::next(i);
        }
    }
    return wait;
}

} // namespace prometheia::server

// SPDX-License-Identifier: GPL-2.0-or-later
//
// The protocol core of prometheiad: Astrolog's ephemeris protocol version 4
// (third_party/ephproto/v4/ephproto.h, the locked pair) as a byte-in,
// message-out state machine with no sockets in it. The WebSocket head
// (ws_server.cpp) feeds each binary message to Session::on_message and sends
// what Session::next hands back while the socket is not backed up; the tests
// drive the same calls directly.
//
// Bodies are NAIF/SPK-IDs, options are typed profile fields, zodiacs are
// tokens: there is no wire map (the version 3 numbering is gone with the
// clean break, kProtoMin 4). datasetId, not a client's word, identifies what
// the server answers from; the result cache keys on it plus the question
// block's bytes (3.7).
//
// Threading: one LoopContext per event-loop thread (it owns that thread's
// Engine and result cache); every Session of the loop borrows it.
#ifndef PROMETHEIA_SERVER_SESSION_HPP
#define PROMETHEIA_SERVER_SESSION_HPP

#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ephproto.h"
#include "limits.hpp"
#include "metrics.hpp"
#include "prometheia/engine.hpp"

namespace prometheia::server {

inline constexpr const char* kServerVersion = "prometheiad/0.2.0";
// What the ΔT-model capability names; the engine's default model (USNO
// observed table, refreshed at release) is what serves the canonical-NaN
// deltaTSec.
inline constexpr const char* kDeltaTModelName = "usno-observed";

struct ServerConfig {
    // The WELCOME bounds. max_cells bounds objects x rows per REQUEST; the
    // rest name their field.
    uint32_t max_objs = 64;
    uint32_t max_rows = 20000;
    uint32_t max_chunk_rows = 500;
    uint32_t max_cells = 100000;
    uint8_t max_profiles = 16;
    uint32_t max_payload = 4u * 1024u * 1024u;
    size_t cache_bytes = 64u << 20; // per loop
    size_t max_queued_answers = 4;  // computed, not yet sent, per connection
    std::string server_name = kServerVersion;
    std::string engine;     // WELCOME's engine string, from Engine::source()
    std::string dataset_id; // from make_dataset(); the cache key's prefix
    // The pin targets (REQUEST TLVs 0x8001-0x8002): the answer comes only
    // from the named snapshot, or ERROR 5 (3.5).
    std::string ephemeris_name;
    std::vector<std::string> catalog_names;
};

// One computed answer, in the server's own units (f64 throughout; the f32
// delivery precision rounds at the last step, 3.9). Object-major columns of
// 6 + popcount(columns_present) values, and one eph::Meta per object.
struct Answer {
    uint32_t n_obj = 0;
    uint32_t n_time = 0;
    uint32_t columns_present = 0;
    std::vector<std::string> sources;
    std::vector<eph::Meta> meta;
    std::vector<double> cols;
    size_t bytes() const { return cols.size() * sizeof(double) + meta.size() * 64; }
};

// Least-recently-used answers under a byte budget, keyed by datasetId plus
// the question block's bytes, exactly as received (3.7): the delivery block
// is outside the key, so the same question in any precision or chunking is
// computed once.
class ResultCache {
public:
    explicit ResultCache(size_t budget_bytes) : budget_(budget_bytes) {}
    std::shared_ptr<const Answer> get(const std::string& key);
    void put(const std::string& key, std::shared_ptr<const Answer> answer);
    size_t entries() const { return map_.size(); }
    size_t used_bytes() const { return used_; }
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }

private:
    using Lru = std::list<std::pair<std::string, std::shared_ptr<const Answer>>>;
    size_t budget_;
    size_t used_ = 0;
    uint64_t hits_ = 0, misses_ = 0;
    Lru lru_;
    std::unordered_map<std::string, Lru::iterator> map_;
};

class LoopContext {
public:
    // `limits` is shared by every loop and may be null (no tokens, no budget).
    // `metrics` may live outside the context so other threads can read it
    // for its whole life; null keeps the context's own.
    LoopContext(Engine engine, const ServerConfig& config, Limits* limits = nullptr,
                Metrics* metrics = nullptr);

    // A request's answer from the cache, or computed and cached. The cache
    // key is the dataset id plus the question block's bytes.
    std::shared_ptr<const Answer> answer(const eph::Request& req, std::string_view question);
    // Computed, never cached.
    std::shared_ptr<const Answer> compute(const eph::Request& req);

    const ServerConfig& config() const { return config_; }
    Engine& engine() { return engine_; }
    ResultCache& cache() { return cache_; }
    Limits* limits() const { return limits_; }
    Metrics& metrics() { return *metrics_; }

private:
    Engine engine_;
    ServerConfig config_;
    ResultCache cache_;
    Limits* limits_;
    Metrics own_metrics_;
    Metrics* metrics_;
};

class Session {
public:
    // `addr` is the peer's address, the default compute-budget key.
    explicit Session(LoopContext& ctx, std::string addr = {})
        : ctx_(ctx), budget_key_("a:" + addr) {}

    // Handles one WebSocket message. Returns false when the connection is to
    // be closed once the replies queued so far have been sent.
    bool on_message(std::string_view message, bool binary);

    // The next whole message to send, if any: control replies first, then
    // the DATA chunks of computed answers in request order.
    bool next(std::vector<uint8_t>& out);

    // Whether next() has anything to hand out.
    bool pending() const { return !control_.empty() || !streams_.empty(); }

    // The session's protocol version: fixed by the first HELLO, 0 before.
    uint8_t version() const { return version_; }
    size_t queued_answers() const { return streams_.size(); }

private:
    struct Stream {
        uint32_t request_id = 0;
        uint8_t precision = eph::kPrecF64;
        uint32_t chunk_rows = 0;
        uint32_t next_row = 0;
        uint32_t chunk_index = 0;
        bool ignored_ext = false;
        std::shared_ptr<const Answer> answer;
    };

    void send(uint16_t type, uint32_t request_id, const std::vector<uint8_t>& payload);
    void send_error(uint32_t request_id, eph::ErrCode code, uint16_t flags, uint32_t retry_ms,
                    const std::string& text);
    bool on_request(const eph::Envelope& env, const uint8_t* payload, size_t len);
    bool on_lookup(const eph::Envelope& env, const uint8_t* payload, size_t len);

    LoopContext& ctx_;
    std::string budget_key_; // "a:" address, or "t:" token once HELLO gave a known one
    uint8_t version_ = 0;
    bool ignored_ext_ = false; // a non-critical extension this server ignored
    std::deque<std::vector<uint8_t>> control_;
    std::deque<Stream> streams_;
};

} // namespace prometheia::server

#endif // PROMETHEIA_SERVER_SESSION_HPP

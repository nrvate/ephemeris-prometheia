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
// Requests are computed in blocks of rows across the owner's turns
// (Session::work), not in one callback, so a CANCEL that arrives is read
// before the rest of the work is done, and one large request does not block
// its loop for everyone. A request works priority 0 before priority 1;
// within one request the chunks stay in order.
//
// Threading: one LoopContext per event-loop thread (it owns that thread's
// Engine, result cache and segment caches); every Session of the loop borrows
// it, and work() is only ever called on that thread.
#ifndef PROMETHEIA_SERVER_SESSION_HPP
#define PROMETHEIA_SERVER_SESSION_HPP

#include <chrono>
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
#include "log.hpp"
#include "metrics.hpp"
#include "prometheia/engine.hpp"
#include "segcache.hpp"

namespace prometheia::server {

inline constexpr const char* kServerVersion = "prometheiad/0.5.0";
// What the ΔT-model capability names; the engine's default model (USNO
// observed table, refreshed at release) is what serves the canonical-NaN
// deltaTSec.
inline constexpr const char* kDeltaTModelName = "usno-observed";

// The largest Chebyshev degree a segments answer carries, and the most
// segments one object may take (WELCOME's A.3 0x000F): the cap keeps one
// object's chunks inside WELCOME's payload bound at the largest degree.
inline constexpr int kSegMaxDegree = 16;
inline constexpr size_t kSegMaxPerObject = 8192;

struct ServerConfig {
    // The WELCOME bounds. max_cells bounds objects x rows per REQUEST; the
    // rest name their field.
    uint32_t max_objs = 64;
    uint32_t max_rows = 20000;
    uint32_t max_chunk_rows = 500;
    uint32_t max_cells = 100000;
    uint8_t max_profiles = 16;
    uint32_t max_payload = 4u * 1024u * 1024u;
    // The widest span one segments REQUEST may ask (WELCOME's segMaxSpanDays);
    // the honest unit for a segments bound is the time window (3.9).
    uint32_t max_seg_span_days = 1024;
    size_t cache_bytes = 64u << 20;     // per loop, samples answers
    size_t seg_cache_bytes = 16u << 20; // per loop, fitted lattice cells
    size_t max_queued_answers = 4;      // computed, not yet sent, per connection
    std::string server_name = kServerVersion;
    std::string engine;     // WELCOME's engine string, from Engine::source()
    std::string dataset_id; // from make_dataset(); the cache key's prefix
    // The pin targets (REQUEST TLVs 0x8001-0x8002): the answer comes only
    // from the named snapshot, or ERROR 5 (3.5).
    std::string ephemeris_name;
    std::vector<std::string> catalog_names;
    // The named hypothetical bodies every loop's engine defines (A.15 tokens
    // and any others its element files add), advertised in WELCOME (A.3
    // 0x0011). Filled at startup from a probe engine, as the dataset is.
    std::vector<std::string> hypotheticals;
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

// One segments answer (a representation = 1 REQUEST): the wire's own shapes.
// Coefficients are tropical in each object's profile frame; the ayanamsa of
// every sidereal profile rides as its own series, which the client subtracts
// (3.4). rowsOk counts the segments served, and firstFailedRow is unused.
struct SegAnswer {
    uint32_t n_obj = 0;
    std::vector<std::string> sources;
    std::vector<eph::Meta> meta;
    std::vector<eph::AyanSeries> ayan;
    std::vector<std::vector<eph::Segment>> segs;
};

// Least-recently-used answers under a byte budget, keyed by datasetId plus
// the question block's bytes, exactly as received (3.7): the delivery block
// is outside the key, so the same question in any precision or chunking is
// computed once. Segments answers do not live here: their sharing unit is the
// lattice cell, whose key carries the rung (a delivery-block field), so they
// have their own cache in LoopContext.
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

// A compute that yields between blocks of rows, so a CANCEL that arrives is
// read before the rest of the work is done (3.4). Defined in session.cpp.
class SamplesComputer;
class SegmentsComputer;

class LoopContext {
public:
    // `limits` is shared by every loop and may be null (no tokens, no budget).
    // `metrics` may live outside the context so other threads can read it
    // for its whole life; null keeps the context's own.
    // `log` may be null: nothing is logged.
    LoopContext(Engine engine, const ServerConfig& config, Limits* limits = nullptr,
                Metrics* metrics = nullptr, const Log* log = nullptr);

    // A request's answer from the cache, or computed and cached, in one call.
    // The session uses this only on its synchronous paths; a REQUEST it will
    // stream goes through SamplesComputer and finish() below.
    std::shared_ptr<const Answer> answer(const eph::Request& req, std::string_view question);
    // Computed, never cached.
    std::shared_ptr<const Answer> compute(const eph::Request& req);
    // A blockwise compute that finished: cached whole — a cancelled or
    // unfinished answer never reaches here — and measured.
    void finish(const std::string& key, const std::shared_ptr<const Answer>& answer, uint64_t cells,
                double ms);

    const ServerConfig& config() const { return config_; }
    Engine& engine() { return engine_; }
    ResultCache& cache() { return cache_; }
    SegmentCache& seg_cache() { return seg_cache_; }
    ScalarSegmentCache& ayan_cache() { return ayan_cache_; }
    Limits* limits() const { return limits_; }
    Metrics& metrics() { return *metrics_; }
    const Log* log() const { return log_; }

private:
    Engine engine_;
    ServerConfig config_;
    ResultCache cache_;
    SegmentCache seg_cache_;
    ScalarSegmentCache ayan_cache_;
    Limits* limits_;
    Metrics own_metrics_;
    Metrics* metrics_;
    const Log* log_;
};

class Session {
public:
    // `addr` is the peer's address, the default compute-budget key; `conn`
    // the connection's id in log lines ("<loop>.<n>").
    explicit Session(LoopContext& ctx, std::string addr = {}, std::string conn = {})
        : ctx_(ctx), budget_key_("a:" + addr), conn_(conn.empty() ? "-" : std::move(conn)) {}
    // Out of line: a Stream holds unique_ptr to computers that are only
    // defined in session.cpp.
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Handles one WebSocket message. Returns false when the connection is to
    // be closed once the replies queued so far have been sent.
    bool on_message(std::string_view message, bool binary);

    // The next whole message to send, if any: control replies first, then
    // the chunks of answers ready to stream, lower priority number first and
    // in request order within one priority.
    bool next(std::vector<uint8_t>& out);

    // Whether next() has anything to hand out right now. A request still
    // computing has nothing to send yet; it says so through has_work().
    bool pending() const;

    // Whether an accepted request still needs computing. The owner works the
    // session between its own turns.
    bool has_work() const;
    // One bounded slice of the most urgent compute (a few milliseconds): the
    // span between slices is where a CANCEL is read.
    void work();

    // The session's protocol version: fixed by the first HELLO, 0 before.
    uint8_t version() const { return version_; }
    size_t queued_answers() const { return streams_.size(); }
    // REQUESTs and LOOKUPs this connection sent, for its close line.
    uint32_t requests_seen() const { return requests_seen_; }
    const std::string& conn() const { return conn_; }

private:
    struct Stream {
        uint32_t request_id = 0;
        uint8_t priority = 0; // 0 interactive, 1 prefetch (3.4)
        uint8_t precision = eph::kPrecF64;
        uint32_t chunk_rows = 0; // samples chunks
        uint32_t chunk_index = 0;
        bool ignored_ext = false;
        bool segments = false;
        // Samples: the answer and the rows already sent.
        std::shared_ptr<const Answer> answer;
        uint32_t next_row = 0;
        // Segments: the answer and the objects already sent.
        std::shared_ptr<const SegAnswer> seg_answer;
        uint32_t next_obj = 0;
        // While the compute runs. Dropping the stream drops these, which is
        // what stops the work when a CANCEL arrives.
        std::unique_ptr<SamplesComputer> computing;
        std::unique_ptr<SegmentsComputer> fitting;
        std::string cache_key; // the samples answer is cached under this whole
        // For the log's "done" line.
        std::chrono::steady_clock::time_point accepted = std::chrono::steady_clock::now();
        bool cache_hit = false;

        // Out of line, like ~Session: the computers are only defined in
        // session.cpp.
        ~Stream();
        Stream() = default;
        Stream(Stream&&) = default;
        Stream& operator=(Stream&&) = default;

        bool sendable() const {
            if (segments) {
                return seg_answer && next_obj < seg_answer->n_obj;
            }
            return answer && next_row < answer->n_time;
        }
        bool sent_all() const {
            return segments ? (seg_answer && next_obj >= seg_answer->n_obj)
                            : (answer && next_row >= answer->n_time);
        }
    };

    void send(uint16_t type, uint32_t request_id, const std::vector<uint8_t>& payload);
    // One log line prefixed with this connection's id, when the level allows.
    void note(LogLevel level, const char* fmt, ...) const __attribute__((format(printf, 3, 4)));
    // The "done" line for a stream whose last chunk just went out.
    void note_done(const Stream& s) const;
    void send_error(uint32_t request_id, eph::ErrCode code, uint16_t flags, uint32_t retry_ms,
                    const std::string& text);
    bool on_request(const eph::Envelope& env, const uint8_t* payload, size_t len);
    bool on_lookup(const eph::Envelope& env, const uint8_t* payload, size_t len);
    void on_cancel(uint32_t request_id);
    // The stream to work on (computing) or send from (sendable): the lowest
    // priority number, and the earliest arrival among equals — the deque
    // holds arrival order.
    Stream* pick(bool computing);

    LoopContext& ctx_;
    std::string budget_key_; // "a:" address, or "t:" token once HELLO gave a known one
    std::string conn_;
    uint32_t requests_seen_ = 0;
    uint8_t version_ = 0;
    std::deque<std::vector<uint8_t>> control_;
    std::deque<Stream> streams_;
};

} // namespace prometheia::server

#endif // PROMETHEIA_SERVER_SESSION_HPP

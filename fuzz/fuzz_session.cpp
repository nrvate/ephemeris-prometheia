// SPDX-License-Identifier: GPL-2.0-or-later
//
// libFuzzer target: a WebSocket connection's messages into a fresh Session
// (docs/SERVER.md, "Fuzzing"). The sanitizers are one oracle; the other is
// the protocol's own: every message the server sends must parse as a v4 frame
// (eph::ParseFrame) and re-encode to the same bytes (eph::ReencodeFrame,
// 3.10), whatever it was sent.
//
// Input: one flags byte, then records {u8 kind, u16 length (LE), bytes}.
//   flags bit 0   start with a valid HELLO, so the fuzzer reaches the session
//                 state without having to discover one
//   kind bit 0    a text frame rather than a binary one
// A record's length is cut to the bytes that remain.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "ephproto.h"
#include "session.hpp"
#include "synthetic_spk.hpp"

using namespace prometheia;
using namespace prometheia::server;

namespace {

// libFuzzer calls a C entry point per input, so the engine lives in a
// function-local static: building one per input would cost milliseconds.
// Caches are off, so no input sees another's answers.
LoopContext& context() {
    static synth::TempFile tf("fuzz-session");
    static LoopContext ctx = [] {
        Engine engine = synth::open_synthetic(tf);
        auto catalog =
            engine.add_catalog(std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/sample-100.epm");
        if (!catalog.ok()) {
            std::fprintf(stderr, "fuzz_session: %s\n", catalog.error().message.c_str());
            std::abort();
        }
        ServerConfig config;
        // Small bounds: each input computes microseconds, not seconds.
        config.max_objs = 8;
        config.max_rows = 16;
        config.max_chunk_rows = 4;
        config.max_cells = 64;
        config.max_profiles = 4;
        config.max_payload = 64 * 1024;
        config.max_seg_span_days = 16;
        config.cache_bytes = 0;
        config.seg_cache_bytes = 0;
        config.hypotheticals = engine.hypothetical_tokens();
        config.engine = "Prometheia fuzz, synthetic kernel";
        config.dataset_id = "synthetic/fuzz#00000000";
        config.ephemeris_name = "synthetic.bsp";
        config.catalog_names = {"sample-100.epm"};
        return LoopContext(std::move(engine), config);
    }();
    return ctx;
}

std::vector<uint8_t> hello_frame() {
    eph::Hello h;
    h.protoMax = eph::kProtoVersion;
    h.protoMin = eph::kProtoMin;
    h.caps = 0x3FF; // every A.2 bit assigned today
    h.clientName = "fuzz";
    std::vector<uint8_t> payload;
    eph::EncodeHello(&payload, h);
    std::vector<uint8_t> out;
    eph::WriteEnvelope(&out, eph::kMsgHello, 0, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

[[noreturn]] void fail(const char* what, const std::vector<uint8_t>& msg, const std::string& why) {
    std::fprintf(stderr, "fuzz_session: %s (%s), %zu bytes:", what, why.c_str(), msg.size());
    for (size_t i = 0; i < msg.size() && i < 64; ++i) {
        std::fprintf(stderr, " %02x", msg[i]);
    }
    std::fprintf(stderr, "\n");
    std::abort();
}

// Every reply the session has ready, each checked against the protocol.
void drain(Session& s) {
    std::vector<uint8_t> msg;
    while (s.next(msg)) {
        eph::Envelope env;
        std::string why;
        const eph::Outcome o = eph::ParseFrame(msg.data(), msg.size(), &env, &why);
        if (o != eph::kOk) {
            fail("the server sent a frame that does not parse", msg, why);
        }
        std::vector<uint8_t> again;
        if (eph::ReencodeFrame(msg.data(), msg.size(), &again) && again != msg) {
            fail("the server sent a frame that is not canonical", msg, "re-encodes differently");
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) {
        return 0;
    }
    Session s(context(), "10.0.0.1", "0.0");
    if (data[0] & 1) {
        const std::vector<uint8_t> h = hello_frame();
        if (!s.on_message(std::string_view(reinterpret_cast<const char*>(h.data()), h.size()), true)) {
            std::abort(); // a valid HELLO must be accepted
        }
        drain(s);
    }
    size_t i = 1;
    while (i + 3 <= size) {
        const uint8_t kind = data[i];
        size_t len = size_t(data[i + 1]) | (size_t(data[i + 2]) << 8);
        i += 3;
        if (len > size - i) {
            len = size - i;
        }
        const bool open =
            s.on_message(std::string_view(reinterpret_cast<const char*>(data + i), len), !(kind & 1));
        i += len;
        drain(s);
        // Bounded: the config's small limits keep a request to a few slices.
        for (int slice = 0; slice < 256 && s.has_work(); ++slice) {
            s.work();
            drain(s);
        }
        if (!open) {
            break;
        }
    }
    return 0;
}

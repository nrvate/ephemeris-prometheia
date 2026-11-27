// SPDX-License-Identifier: GPL-2.0-or-later
//
// libFuzzer target: the protocol v4 codec alone (the vendored ephproto.h,
// docs/SERVER.md, "Fuzzing"). Any bytes go to eph::ParseFrame, which must not
// crash, read out of bounds or overflow; a frame it accepts must re-encode to
// exactly the input (3.1 canonical input, 3.10), or the codec accepted a
// non-canonical frame.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "ephproto.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    eph::Envelope env;
    std::string why;
    if (eph::ParseFrame(data, size, &env, &why) != eph::kOk) {
        return 0;
    }
    std::vector<uint8_t> again;
    if (eph::ReencodeFrame(data, size, &again) &&
        (again.size() != size || !std::equal(again.begin(), again.end(), data))) {
        std::fprintf(stderr, "fuzz_frame: type %u parsed ok but re-encodes differently\n",
                     unsigned(env.type));
        std::abort();
    }
    return 0;
}

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fuzz the protocol v4 codec, prometheiad's session and prometheia-json's MCP
# dispatcher with libFuzzer, under
# ASan and UBSan (docs/SERVER.md, "Fuzzing"). Never part of tools/gate.sh:
# it runs for as long as it is given.
#
#   tools/fuzz.sh [SECONDS]        each target for SECONDS (default 60)
#
# Needs clang (CLANG, default clang-14 / clang++-14). Seeds come from the
# protocol's conformance set in the Astrolog tree ($PROMETHEIA_ASTROLOG,
# default /nvmraid/shares/Astrolog); without it the fuzzers start empty.
# Corpora grow in fuzz-corpus/ and failures land in fuzz-artifacts/, both
# gitignored. A failure becomes a regression test in tests/test_server.cpp.
set -euo pipefail
cd "$(dirname "$0")/.."

seconds=${1:-60}
cc=${CLANG:-clang-14}
cxx=${CLANGXX:-clang++-14}
flags="-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer"
astrolog=${PROMETHEIA_ASTROLOG:-/nvmraid/shares/Astrolog}

cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPROMETHEIA_FUZZ=ON \
    -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" >/dev/null
cmake --build build-fuzz --target fuzz_session fuzz_frame fuzz_json -j "$(nproc)" >/dev/null

mkdir -p fuzz-corpus/frame fuzz-corpus/session fuzz-corpus/json fuzz-artifacts
# json: a mode byte (bit 0: the text is a tool's arguments, the tool picked by
# the upper bits; fuzz/fuzz_json.cpp), then the text. One seed per method and
# per tool, from the examples in docs/JSON_API.md.
python3 - <<'EOF'
seeds = {
    "initialize": (0, '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}}'),
    "tools-list": (0, '{"jsonrpc":"2.0","id":"a","method":"tools/list"}'),
    "resources-read": (0, '{"jsonrpc":"2.0","id":2,"method":"resources/read","params":{"uri":"prometheia://llms.txt"}}'),
    "batch": (0, '[{"jsonrpc":"2.0","id":1,"method":"ping"},{"jsonrpc":"2.0","method":"notifications/initialized"}]'),
    "positions": (1, '{"time":"1990-06-15T14:30:00+02:00","observer":"topocentric",'
                     '"site":{"lon_deg":8.55,"lat_deg":47.37,"height_m":500},"zodiac":"lahiri",'
                     '"objects":["Mars",{"point":"ascending-node","of":"Moon","method":"mean"},'
                     '"Spica","Ceres"],"rates":true}'),
    "positions-series": (1, '{"objects":["Mars"],"series":{"start":"1999-12-31","step_days":1.5,'
                            '"count":4},"frame":"j2000","observer":"body","center":"Jupiter",'
                            '"corrections":["light-time"]}'),
    "lookup": (3, '{"query":"ceres"}'),
    "capabilities": (5, '{}'),
    "convert-time": (7, '{"time":"2016-12-31T23:59:60Z"}'),
}
for name, (mode, text) in seeds.items():
    open(f"fuzz-corpus/json/seed-{name}", "wb").write(bytes([mode]) + text.encode())
EOF
conformance="$astrolog/ephsrv/conformance"
if [[ -f "$conformance/MANIFEST.tsv" ]]; then
    # frame: every fixture as it is. session: each client-to-server fixture as
    # one binary record after the fuzzer's own HELLO (flags 1, kind 0).
    python3 - "$conformance" <<'EOF'
import os, sys
src = sys.argv[1]
for line in open(os.path.join(src, "MANIFEST.tsv")):
    if line.startswith("#") or not line.strip():
        continue
    name, direction = line.split("\t")[:2]
    raw = bytes.fromhex("".join(open(os.path.join(src, name)).read().split()))
    stem = os.path.splitext(name)[0]
    open(f"fuzz-corpus/frame/seed-{stem}", "wb").write(raw)
    if direction == "c2s":
        rec = bytes([0]) + len(raw).to_bytes(2, "little") + raw
        open(f"fuzz-corpus/session/seed-{stem}", "wb").write(bytes([1]) + rec)
EOF
else
    echo "fuzz.sh: no conformance set at $conformance; starting from empty corpora" >&2
fi

status=0
for target in frame session json; do
    dict=fuzz/protocol.dict
    [[ $target == json ]] && dict=fuzz/json.dict
    echo "== fuzz_$target, ${seconds} s"
    # setarch -R: clang 14's sanitizer runtime can spin forever at start-up
    # under this kernel's address-space randomisation (measured: 7 of 12
    # starts, 12 of 12 with it off).
    if ! setarch "$(uname -m)" -R build-fuzz/fuzz_$target fuzz-corpus/$target -max_total_time="$seconds" -timeout=10 \
        -dict="$dict" \
        -rss_limit_mb=2048 -artifact_prefix=fuzz-artifacts/$target- -print_final_stats=1 \
        2>fuzz-artifacts/$target.log; then
        status=1
        echo "fuzz_$target FAILED: fuzz-artifacts/$target.log" >&2
    fi
    grep -E "^(stat::number_of_executed_units|stat::peak_rss_mb)|^#[0-9]+ *DONE" \
        fuzz-artifacts/$target.log || true
done
exit $status

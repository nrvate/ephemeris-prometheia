#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fuzz the protocol v4 codec and prometheiad's session with libFuzzer, under
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
cmake --build build-fuzz --target fuzz_session fuzz_frame -j "$(nproc)" >/dev/null

mkdir -p fuzz-corpus/frame fuzz-corpus/session fuzz-artifacts
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
for target in frame session; do
    echo "== fuzz_$target, ${seconds} s"
    # setarch -R: clang 14's sanitizer runtime can spin forever at start-up
    # under this kernel's address-space randomisation (measured: 7 of 12
    # starts, 12 of 12 with it off).
    if ! setarch "$(uname -m)" -R build-fuzz/fuzz_$target fuzz-corpus/$target -max_total_time="$seconds" -timeout=10 \
        -dict=fuzz/protocol.dict \
        -rss_limit_mb=2048 -artifact_prefix=fuzz-artifacts/$target- -print_final_stats=1 \
        2>fuzz-artifacts/$target.log; then
        status=1
        echo "fuzz_$target FAILED: fuzz-artifacts/$target.log" >&2
    fi
    grep -E "^(stat::number_of_executed_units|stat::peak_rss_mb)|^#[0-9]+ *DONE" \
        fuzz-artifacts/$target.log || true
done
exit $status

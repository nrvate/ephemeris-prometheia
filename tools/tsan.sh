#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run the tests that share files or a Corpus between threads under
# ThreadSanitizer, in build-tsan/ (gitignored). Never part of tools/gate.sh:
# a TSan build is slow and is not needed for every commit. Run it after any
# change to what engines share (docs/ENGINE.md, "Threads").
#
#   tools/tsan.sh [-j N]
#
# The real-data case (test_engine_threads) needs DE440 like the gate's tests
# do (PROMETHEIA_DE440, else ephe/); without it, it skips and says so.
set -euo pipefail
cd "$(dirname "$0")/.."

jobs=2
if [[ "${1:-}" == "-j" ]]; then
    jobs=$2
fi
targets=(test_io test_de test_spk test_catalog test_engine_threads test_c_dataset)

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" >/dev/null
cmake --build build-tsan --target "${targets[@]}" -j"$jobs" >/dev/null

# The TSan runtimes of GCC 11 and clang 14 cannot map their shadow memory
# under the address randomization of recent kernels ("unexpected memory
# mapping"); setarch -R runs the tests without it.
norandom=()
if command -v setarch >/dev/null; then
    norandom=(setarch "$(uname -m)" -R)
fi

status=0
for t in "${targets[@]}"; do
    echo "== $t"
    if ! TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" "${norandom[@]}" "build-tsan/$t" >"build-tsan/$t.log" 2>&1; then
        status=1
        echo "FAILED (build-tsan/$t.log):"
        tail -40 "build-tsan/$t.log"
    else
        tail -2 "build-tsan/$t.log" | head -1
    fi
done
[[ $status == 0 ]] && echo "== tsan clean"
exit $status

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The pre-commit gate, run locally (the project uses no hosted CI):
#   1. clang-format check over include/ src/ server/ tests/ tools/ fuzz/ (third_party excluded)
#   1a. the v4 registries, and crosstest.py's adjudicators against hand-built tables
#   2. Release build + ctest in build/
#   3. ASan+UBSan build + ctest in build-asan/
# Tests run serially: the integrator benchmark asserts a wall-clock bound.
# Real-data tests run when the JPL files are present under ephe/ (or the
# PROMETHEIA_* environment variables point at them) and SKIP otherwise.
#
# Usage: tools/gate.sh [-j N]      CLANG_FORMAT=... to pick the binary
set -euo pipefail

cd "$(dirname "$0")/.."
jobs="$(nproc)"
if [[ "${1:-}" == "-j" && -n "${2:-}" ]]; then
    jobs="$2"
fi
clang_format="${CLANG_FORMAT:-clang-format}"

echo "== format ($("$clang_format" --version | head -1))"
find include src server tests tools fuzz \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' -o -name '*.h' \
    -o -name '*.inl' \) -print0 | xargs -0 "$clang_format" --dry-run -Werror

# Runs a step quietly; on failure prints its full output and stops.
step() {
    local log
    log="$(mktemp)"
    if "$@" >"$log" 2>&1; then
        rm -f "$log"
    else
        cat "$log"
        rm -f "$log"
        echo "== gate FAILED: $*"
        exit 1
    fi
}

# The vendored protocol v4 pair must agree by name (instant, no build).
echo "== registries (protocol v4, by name)"
step python3 tools/check/ephproto4_registries.py

# The cross-test's adjudicators decide what a disagreement means, and they
# are what speaks to the other project. They are pure functions of the
# results table, so they can be falsified here with no daemon and no
# ephemeris: 29 cases in under a tenth of a second, plus a line-coverage
# check that fails if a branch of one has no case.
echo "== adjudicators (crosstest.py, by case)"
step python3 tools/check/adjudicatetest.py

tests() {
    local log
    log="$(mktemp)"
    if ctest --test-dir "$1" --output-on-failure >"$log" 2>&1; then
        grep -E 'tests passed|Total Test time' "$log"
        rm -f "$log"
    else
        cat "$log"
        rm -f "$log"
        echo "== gate FAILED: tests in $1"
        exit 1
    fi
}

echo "== build/ (Release)"
step cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
step cmake --build build -j"$jobs"
tests build

echo "== build-asan/ (ASan+UBSan)"
step cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DPROMETHEIA_SANITIZE=ON
step cmake --build build-asan -j"$jobs"
tests build-asan

echo "== gate passed"

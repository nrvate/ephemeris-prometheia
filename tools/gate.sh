#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The pre-commit gate, run locally (the project uses no hosted CI):
#   1. clang-format check over include/ src/ server/ tests/ tools/ fuzz/ (third_party excluded)
#   1a. the v4 registries, crosstest.py's adjudicators, and ratesweep.py's
#       assertions -- each against scripted inputs, no daemon and no ephemeris
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

# A failing step's output used to go to a mktemp file that was deleted after
# being printed once, so a failure that scrolled away left nothing behind:
# the gate failure of 2026-09-19 is recorded in docs/HANDOFF.md as "test name
# unknown" for exactly that reason, and no amount of re-running found it.
# The log now survives the run that produced it.
failure_log="$PWD/build/gate-failure.log"
mkdir -p "$PWD/build"
rm -f "$failure_log"

# What this run could see. Real-data suites SKIP when their files are absent,
# so two runs of the same commit can check different things; a green means
# nothing without knowing which. PROMETHEIA_ASTROLOG in particular points the
# drift check at another project's working tree, which moves under us.
# `|| true` is load-bearing: under `set -e` a grep that matches nothing
# fails, and a command substitution that fails aborts the script -- which it
# did, silently and with no output at all, the first time this ran on a
# machine with no PROMETHEIA_* set. A step added to make failures legible
# that makes the whole gate illegible.
gated="$(env | grep -o '^PROMETHEIA_[A-Z0-9_]*' | sort | tr '\n' ' ' || true)"
echo "== env: ${gated:-no PROMETHEIA_* set (real-data suites will SKIP)}"

# Runs a step quietly; on failure prints its full output and stops.
step() {
    local log
    log="$(mktemp)"
    if "$@" >"$log" 2>&1; then
        rm -f "$log"
    else
        cat "$log"
        { echo "== gate FAILED: $*"; cat "$log"; } >"$failure_log"
        rm -f "$log"
        echo "== gate FAILED: $*"
        echo "== the full output is kept at $failure_log"
        exit 1
    fi
}

format_check() {
    find include src server tests tools fuzz \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' \
        -o -name '*.h' -o -name '*.inl' \) -print0 | xargs -0 "$clang_format" --dry-run -Werror
}

echo "== format ($("$clang_format" --version | head -1))"
step format_check

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

# ratesweep.py decides whether another project's server misses the rate
# bound it advertises, and five of its six assertions are about the sweep
# having happened at all. Seven scripted servers, no daemon, no ephemeris,
# under two seconds. corrtest.py makes the same promise for corrapplied.py
# but spends fourteen, so it runs from tools/scheduled.sh instead.
echo "== rate sweep (ratesweep.py, by assertion)"
step python3 tools/check/ratestest.py

tests() {
    local log
    log="$(mktemp)"
    if ctest --test-dir "$1" --output-on-failure >"$log" 2>&1; then
        grep -E 'tests passed|Total Test time' "$log"
        rm -f "$log"
    else
        cat "$log"
        { echo "== gate FAILED: tests in $1"; cat "$log"; } >"$failure_log"
        rm -f "$log"
        echo "== gate FAILED: tests in $1"
        echo "== the full output is kept at $failure_log"
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

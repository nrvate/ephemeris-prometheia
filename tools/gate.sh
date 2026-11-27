#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The pre-commit gate, run locally (the project uses no hosted CI):
#   1. clang-format check over include/ src/ server/ tests/ tools/ fuzz/ (third_party excluded)
#   1a. the v4 registries; then the assertions of the registries checker,
#       crosstest.py's adjudicators, ratesweep.py and the v4 fixture reader --
#       each against scripted or mutated inputs, no daemon and no ephemeris
#   1b. the Python package's tests (python/tests)
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

# The vendored protocol v4 pair must agree by name (instant, no build),
# and the checker that says so is itself graded against a mutated copy of
# the pair -- never the pair, which is the byte-level authority for v4.
echo "== registries (protocol v4, by name)"
step python3 tools/check/ephproto4_registries.py
echo "== registries checker (by assertion)"
step python3 tools/check/registriestest.py

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

# ephproto4_fixtures.py is our second, independent reading of protocol v4 --
# section 3 is locked on its verdict -- and it cannot run here: it reads
# Astrolog's conformance directory, which is not in this tree. Its assertions
# can, against a set tools/check/fakefixtures.py builds, which is why the
# selftest is in the gate although the tool it grades is not.
echo "== fixture reader (ephproto4_fixtures.py, by assertion)"
step python3 tools/check/fixturestest.py

# sbdb_fetch.py's covariance overlay against scripted SBDB answers: one
# body's unusable covariance or refusal costs only that body. No request
# leaves the machine.
echo "== covariance fetch (sbdb_fetch.py, by assertion)"
step python3 tools/check/fetchtest.py

# Everything above asks whether a check can go red. This asks whether
# anything runs it: every tool under tools/check/ is either invoked by a
# runner or carries a reason why nothing invokes it. The Astrolog side found
# their own structural selftest wired into no runner on 2026-09-20 -- a rule
# existing is not a rule running -- and this is the reciprocal. It reads
# files and starts nothing.
echo "== runners (is every check run by something?)"
step python3 tools/check/runners.py
echo "== runners audit (by assertion)"
step python3 tools/check/runnerstest.py

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

# The Python package (python/, docs/PYTHON.md), after the Release build it
# runs against: the native extension built over build/libprometheia.a, then
# the tests -- the native binding held bit for bit to prometheia-json, the
# transports, and the live runs on DE440 when it is present (they SKIP
# otherwise). Until 2026-10-27 this ran before the build, against whatever
# build/ held from the last one. Cython comes from .venv-build
# (docs/PYTHON.md, "Native": python3 -m venv .venv-build &&
# .venv-build/bin/pip install Cython==3.0.11 setuptools==75.3.0 wheel==0.44.0),
# or $PROMETHEIA_CYTHON_PYTHON.
# ResourceWarnings are errors, so a leaked child process or pipe fails here.
cython_python="${PROMETHEIA_CYTHON_PYTHON:-$PWD/.venv-build/bin/python}"
native_ext() {
    if ! "$cython_python" -c "import Cython" 2>/dev/null; then
        echo "no Cython at $cython_python: python3 -m venv .venv-build &&"
        echo "  .venv-build/bin/pip install Cython==3.0.11 setuptools==75.3.0 wheel==0.44.0 (docs/PYTHON.md, \"Native\")"
        return 1
    fi
    (cd python && PKG_CONFIG_PATH="$OLDPWD/build" "$cython_python" setup.py -q build_ext \
        --inplace -t "$OLDPWD/build/python-temp")
}
echo "== python native extension (python/prometheia/_native)"
step native_ext
echo "== python package (python/tests)"
step env PYTHONPATH=python python3 -W error::ResourceWarning -m unittest discover -s python/tests

echo "== build-asan/ (ASan+UBSan)"
step cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DPROMETHEIA_SANITIZE=ON
step cmake --build build-asan -j"$jobs"
tests build-asan

echo "== gate passed"

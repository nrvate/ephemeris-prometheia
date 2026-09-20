#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The checks tools/gate.sh cannot run: the ones that need a live daemon, an
# ephemeris, or more seconds than a pre-commit gate may spend. The gate must
# stay hermetic and fast (docs/HANDOFF.md, CLAUDE.md), so these have been
# run by hand since they were written -- which means they were run when
# somebody remembered them, and docs/HANDOFF.md has carried "nothing
# schedules this" as an open item for each of them.
#
#   tools/scheduled.sh                 the checks that need only our own tree
#   tools/scheduled.sh --with-cross    also the cross-test (needs the Astrolog
#                                      side's spare daemon to be up)
#
# Every run writes a timestamped log under build/scheduled/ and prints one
# summary. The exit status is the number of steps that failed, so a timer
# unit reports a real failure and a green run leaves a file saying what it
# checked and when.
#
# The cross-test is opt-in on purpose. It reads another project's server,
# and a thing that reaches outside this tree should be asked for explicitly
# rather than happening on a timer because a default said so.
#
# There is no hosted CI and there will not be (CLAUDE.md). tools/systemd/
# holds a user-level service and timer for whoever wants this on a schedule;
# nothing installs them.
set -uo pipefail

cd "$(dirname "$0")/.."
repo="$PWD"
with_cross=0
ephemeris="${PROMETHEIA_EPHEMERIS:-$repo/ephe/linux_p1550p2650.440}"
port="${PROMETHEIA_SCHEDULED_PORT:-47193}"
oracle="${PROMETHEIA_ORACLE_PYTHON:-$repo/.venv-oracle/bin/python}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --with-cross) with_cross=1 ;;
        --ephemeris) ephemeris="$2"; shift ;;
        --port) port="$2"; shift ;;
        *) echo "usage: tools/scheduled.sh [--with-cross] [--ephemeris FILE] [--port N]" >&2
           exit 2 ;;
    esac
    shift
done

stamp="$(date -u +%Y-%m-%dT%H%M%SZ)"
logdir="$repo/build/scheduled"
mkdir -p "$logdir"
log="$logdir/$stamp.log"
: >"$log"

say() { echo "$*" | tee -a "$log"; }

failed=0
ran=0
skipped=()

# One step: quiet unless it fails, everything kept in the log either way.
step() {
    local name="$1"; shift
    ran=$((ran + 1))
    echo "=== $name" >>"$log"
    echo "    \$ $*" >>"$log"
    local start; start="$(date +%s)"
    if "$@" >>"$log" 2>&1; then
        say "ok      $name ($(($(date +%s) - start)) s)"
    else
        failed=$((failed + 1))
        say "FAILED  $name ($(($(date +%s) - start)) s) -- see $log"
    fi
}

say "scheduled checks, $stamp, $(git -C "$repo" rev-parse --short HEAD)"

# The gate first: everything below assumes the tree builds and its own tests
# pass, and a scheduled run that reports a soak failure on a tree that does
# not compile has told nobody anything.
step "gate" "$repo/tools/gate.sh"

# corrapplied.py's own assertions. No daemon and no ephemeris -- it is here
# rather than in the gate only because eight scripted servers take about
# twenty seconds, which a pre-commit gate should not spend.
step "corrapplied selftest" python3 "$repo/tools/check/corrtest.py"

# stars_fk5.py's assertions, each against a mutated copy of the catalogue and
# a scripted client. It is the only check that compares a server with an
# outside catalogue, so a quiet catalogue -- truncated, or columns moved -- is
# its real failure mode, and that is what these six cases inject. Needs pyerfa
# and stars-raw/, which is why it is not in the gate.
if [[ -x "$oracle" ]] && [[ -d "$repo/stars-raw" ]]; then
    step "stars_fk5 selftest" "$oracle" "$repo/tools/check/starstest.py"
else
    skipped+=("stars_fk5 selftest: needs .venv-oracle (pyerfa) and stars-raw/")
fi

# prometheia-load's assertions, each against a daemon started for that case.
if [[ -f "$ephemeris" ]]; then
    step "load selftest" python3 "$repo/tools/check/loadselftest.py" --ephemeris "$ephemeris"
else
    skipped+=("load selftest: no ephemeris at $ephemeris")
fi

# The soak and the memory bound against a fresh daemon. Fresh matters: the
# bound measures growth *during* the run, so a long-running server whose
# cache is already at its plateau passes any bound (docs/SERVER.md).
if [[ -f "$ephemeris" ]] && [[ -x "$repo/build/prometheiad" ]]; then
    # --log-level takes quiet|info|debug. It was `warn` here for one run, the
    # daemon exited on the bad value, and the soak reported SKIP rather than
    # failing -- a skip whose precondition was met is a green that could not
    # have been red, which is the shape the rest of tools/check/ exists to
    # catch. Below: once the binary and the ephemeris are both here, a daemon
    # that does not come up is a FAILURE. Only a missing precondition skips.
    # The port has to be empty BEFORE we start, and that is the whole check.
    # prometheiad refuses a busy port and exits, so a run on an occupied one
    # would have `prometheia-load` driving whoever was already there while
    # `--pid` measured this script's corpse -- rows from one process, memory
    # from another, and a pass. Asking "is our pid alive" after the wait loop
    # does not catch it: the loop breaks on the first healthz, the other
    # server answers it instantly, and the child has not exited yet. That
    # version was written here and the injection below went green.
    if curl -fsS --max-time 1 "http://127.0.0.1:$port/healthz" >/dev/null 2>&1; then
        ran=$((ran + 1))
        failed=$((failed + 1))
        say "FAILED  soak: something already answers /healthz on port $port;" \
            "pass --port N for a free one"
    else
        "$repo/build/prometheiad" --ephemeris "$ephemeris" --port "$port" --log-level quiet \
            >>"$log" 2>&1 &
        daemon=$!
        for _ in $(seq 1 50); do
            if curl -fsS --max-time 1 "http://127.0.0.1:$port/healthz" >/dev/null 2>&1; then
                break
            fi
            sleep 0.2
        done
        if curl -fsS --max-time 1 "http://127.0.0.1:$port/healthz" >/dev/null 2>&1; then
            step "soak and memory bound" "$repo/build/prometheia-load" --port "$port" \
                --pid "$daemon" --seconds 60 --conns 8 --rows 10 --memory-bound 64
        else
            ran=$((ran + 1))
            failed=$((failed + 1))
            say "FAILED  soak: prometheiad never answered /healthz on $port -- see $log"
        fi
        # Only ever the process this script started, by the pid it captured:
        # `pkill -x prometheiad` matches by name across every process on the
        # machine, and killed this project's serving daemon on 2026-09-20.
        kill "$daemon" 2>/dev/null
        wait "$daemon" 2>/dev/null
    fi
else
    skipped+=("soak: needs build/prometheiad and an ephemeris")
fi

if [[ "$with_cross" == 1 ]]; then
    step "cross-test" python3 "$repo/tools/check/crossrun.py" --record
else
    skipped+=("cross-test: --with-cross not given (it reads another project's server)")
fi

say ""
for s in "${skipped[@]:-}"; do
    [[ -n "$s" ]] && say "SKIP    $s"
done
say "$ran step(s) run, $failed failed; log $log"
exit "$failed"

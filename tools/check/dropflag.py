#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A wire client that drops one argument, for blindspots.py.

crosstest.py takes one executable for `--client` and appends the arguments
each leg wants.  This stands in that place: it removes one named flag (and
its value, when the next argument is not itself a flag) from every
invocation and execs the real client with what is left.

The flag to drop comes from `$PROMETHEIA_DROP_FLAG`, the real client from
`$PROMETHEIA_DROP_CLIENT`.  With neither set it is the real client exactly,
which is how blindspots.py takes its control run through the same path as
every other run -- a control that skipped the wrapper would be measuring the
wrapper as well as the leg.

Every run appends its argv to `$PROMETHEIA_DROP_LOG` when that is set, which
is how the flags are discovered: blindspots.py reads what the leg actually
sent rather than a list written beside it.

`$PROMETHEIA_DROP_SUB` is the other half of the same idea: a JSON map
`{flag: value}` that **replaces** a flag's value instead of removing the
flag.  Dropping a whole option asks whether a leg depends on the option at
all; substituting a value asks whether it depends on *this part* of it.  An
option can be reported covered while half of what it means reaches nothing
-- one option, two effects, one untested -- and a whole-option drop cannot
tell (the Astrolog side, 2026-09-20).  Substitutions are counted the same
way, and a substitution that changed no argument is not a measurement.

It also appends how many arguments it removed to `$PROMETHEIA_DROP_COUNT`.
That number is the difference between "the leg did not notice" and "nothing
was taken away from it": a drop that dropped nothing leaves the leg green
and proves nothing, which is the shape this repository keeps finding.
"""
import os
import sys


def strip(argv, flag):
    """argv without `flag`, and without the value that follows it."""
    out, i, dropped = [], 0, 0
    while i < len(argv):
        if argv[i] == flag:
            i += 1
            dropped += 1
            # A value is anything that is not itself a flag. The client's
            # values are ports, ids, tokens and numbers, none of which begin
            # with "--"; a negative number would, if the client had one.
            if i < len(argv) and not argv[i].startswith("--"):
                i += 1
            continue
        out.append(argv[i])
        i += 1
    return out, dropped


def main():
    real = os.environ.get("PROMETHEIA_DROP_CLIENT")
    if not real:
        sys.exit("PROMETHEIA_DROP_CLIENT is not set: this is a stand-in, not a client")
    argv = sys.argv[1:]
    log = os.environ.get("PROMETHEIA_DROP_LOG")
    if log:
        with open(log, "a") as f:
            f.write("\t".join(argv) + "\n")
    flag = os.environ.get("PROMETHEIA_DROP_FLAG")
    dropped = 0
    if flag:
        argv, dropped = strip(argv, flag)
    subs = os.environ.get("PROMETHEIA_DROP_SUB")
    if subs:
        import json
        for name, value in json.loads(subs).items():
            for i, a in enumerate(argv):
                if a == name and i + 1 < len(argv) and argv[i + 1] != value:
                    argv[i + 1] = value
                    dropped += 1
    if flag or subs:
        counter = os.environ.get("PROMETHEIA_DROP_COUNT")
        if counter:
            with open(counter, "a") as f:
                f.write(f"{dropped}\n")
    os.execv(real, [real] + argv)


if __name__ == "__main__":
    sys.exit(main())

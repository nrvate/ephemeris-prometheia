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
    if flag:
        argv, dropped = strip(argv, flag)
        counter = os.environ.get("PROMETHEIA_DROP_COUNT")
        if counter:
            with open(counter, "a") as f:
                f.write(f"{dropped}\n")
    os.execv(real, [real] + argv)


if __name__ == "__main__":
    sys.exit(main())

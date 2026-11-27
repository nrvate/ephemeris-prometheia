#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Falsify every assertion stars_fk5.py makes, against a mutated catalogue.

stars_fk5.py is the only check here that compares a server with something
outside the two projects, and the FK5 is the whole of its authority.  Its
failure mode is therefore not a wrong number but a quiet one: a truncated
`stars-raw/` file, a catalogue whose columns moved, a match that drifted --
and until 2026-09-20 each of those printed "all within band" and exited 0,
because the count it printed came from the same file that had lost the stars.
A star the server never answered for was reported as DIFFERS, which accuses
another project's server of disagreeing with the FK5 when it said nothing.

Each case copies `stars-raw/` (by symlink; the sources are large), mutates one
file of the copy, and points the tool at it with `--raw-dir`.  The mutations
assert that they changed something: a `replace` that matches nothing leaves
the input alone, the run then goes green, and the green proves nothing.

The server is `fakestars.py`, which answers from the **pristine** catalogue,
so a mutated record moves the reference alone.  No daemon, no ephemeris, no
port -- but it does need pyerfa and `stars-raw/`, so this runs from
tools/scheduled.sh and not from the gate.  Without the catalogue it exits 2
rather than 0: a check that could not run must not be able to report a pass.

assertlib.drive requires the named assertion to fire and no other, reads the
declared list from `--list-assertions`, and refuses an assertion no case
exercises.

**What this cannot see:** whether the FK5's columns are read correctly, or
whether the band is the right band.  fakestars.py answers through the same
`reference()`, so a wrong column moves both sides at once.  That is a question
for the live run against a real server; this grades the assertions.

Usage:
  .venv-oracle/bin/python tools/check/starstest.py [-v]
"""

import argparse
import gzip
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assertlib  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

FK5 = "I_149A_catalog.gz"
BSC = "V_50_catalog.gz"

VEGA_FK5 = 699     # the FK5 number the BSC gives HR 7001
REGULUS_FK5 = 380  # HR 3982; not in BINARIES, so its band is the plain 1"
CASTOR_HR = 2891   # the BSC gives it no FK5 number: see stars_fk5.NO_FK5


def edit(filename, fn):
    """A mutation of one gzipped catalogue that must actually change it."""
    def apply(work, pristine):
        with gzip.open(os.path.join(pristine, filename), "rt", encoding="latin-1") as f:
            before = f.readlines()
        after = fn(list(before))
        if after == before:
            raise SystemExit(f"this case did not change {filename}: it would have run "
                             "against the pristine catalogue and passed for no reason")
        path = os.path.join(work, filename)
        os.remove(path)  # the symlink, not the source
        with gzip.open(path, "wt", encoding="latin-1") as f:
            f.writelines(after)
    return apply


def drop(number):
    """Remove one FK5 record: the star loses its reference."""
    return edit(FK5, lambda lines: [l for l in lines if int(l[0:4]) != number])


def empty(lines):
    return []


def give_castor_an_fk5_number(lines):
    """HR 2891 gains HR 2890's FK5 number -- the exception list's rot, exactly."""
    out = []
    for l in lines:
        if l[0:4].strip() and int(l[0:4]) == CASTOR_HR:
            l = l[:37] + " 287" + l[41:]
        out.append(l)
    return out


def move_regulus(lines):
    """Shift one record's declination by a whole arcminute, sixty times the band."""
    out = []
    for l in lines:
        if int(l[0:4]) == REGULUS_FK5:
            l = l[:30] + f"{(int(l[30:32]) + 1) % 60:02d}" + l[32:]
        out.append(l)
    return out


def cases():
    """name, mutation, extra client flags, the assertions that must fire, exit code."""
    return [
        # The control: the catalogue, copied and not touched. Without it the
        # table shows only that a broken catalogue reds, never that the real
        # one stays green.
        ("the catalogue, unmodified", None, [], set(), 0),

        # The failure this tool shipped with: every star falls out, the count
        # printed comes from the same file that lost them, and "all within
        # band: 0 stars" reads as a pass.
        ("the FK5 catalogue is empty", edit(FK5, empty), [], {"nothing-compared"}, 1),

        # One star falls out. The run still compares twenty-eight and would
        # still print "all within band" of them.
        ("one star loses its FK5 record", drop(VEGA_FK5), [], {"star-without-fk5"}, 1),

        # The other direction: a star NO_FK5 excuses turns out to have an
        # entry. Giving HR 2891 the FK5 number of HR 2890 is the mistake the
        # exception exists to prevent, so this is the shape of the rot and not
        # an arbitrary edit.
        ("a NO_FK5 star gains an entry", edit(BSC, give_castor_an_fk5_number), [],
         {"expected-absence-is-present"}, 1),

        # A server that accepts the request and produces no row. It must not
        # read out as a disagreement with the FK5.
        ("the server answers nothing", None, ["--silent"], {"unanswered"}, 1),

        # A record that moved. This is the only case about the sky.
        ("one record's declination moves an arcminute", edit(FK5, move_regulus), [],
         {"differs"}, 1),
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", default=os.path.join(ROOT, "tools/check/stars_fk5.py"))
    ap.add_argument("--raw-dir", default=os.path.join(ROOT, "stars-raw"))
    ap.add_argument("-v", "--verbose", action="store_true", help="echo each run's output")
    args = ap.parse_args()

    if not os.path.isdir(args.raw_dir):
        # Not 0: a missing precondition is "this did not run", and a check
        # that cannot run must not be able to report a pass. tools/scheduled.sh
        # tests for the catalogue itself and records a SKIP.
        print(f"cannot run: {args.raw_dir} is absent "
              "(tools/fetch/stars_fetch.py --only fk5 --only bsc5)")
        return 2

    tmp = os.environ.get("CLAUDE_JOB_DIR")
    tmp = os.path.join(tmp, "tmp") if tmp else os.path.join(ROOT, "build")
    work = os.path.join(tmp, "starstest")

    table = cases()
    by_name = {c[0]: c for c in table}
    fake = os.path.join(ROOT, "tools/check/fakestars.py")

    def run_one(name):
        _, mutate, flags, _, _ = by_name[name]
        shutil.rmtree(work, ignore_errors=True)
        os.makedirs(work)
        for entry in os.listdir(args.raw_dir):
            os.symlink(os.path.join(args.raw_dir, entry), os.path.join(work, entry))
        if mutate is not None:
            mutate(work, args.raw_dir)
        # stars_fk5.py takes one executable for --client and appends its own
        # arguments, so the fake's own flags ride in a wrapper.
        client = os.path.join(work, "client.sh")
        with open(client, "w") as f:
            f.write("#!/bin/sh\nexec %s %s --raw-dir %s %s \"$@\"\n"
                    % (sys.executable, fake, args.raw_dir, " ".join(flags)))
        os.chmod(client, 0o755)
        out = subprocess.run([sys.executable, args.tool, "--raw-dir", work, "--client", client],
                             capture_output=True, text=True, timeout=600)
        return out.returncode, out.stdout + out.stderr

    code = assertlib.drive(args.tool, [(c[0], c[3], c[4]) for c in table],
                           run_one, verbose=args.verbose)
    shutil.rmtree(work, ignore_errors=True)
    return code


if __name__ == "__main__":
    sys.exit(main())

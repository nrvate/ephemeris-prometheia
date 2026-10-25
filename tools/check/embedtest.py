#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The embedding contract, exercised the way a parent project would.

docs/EMBEDDING.md promises that a project with its own makefiles can build
libprometheia from embed/sources.txt through embed/prometheia.mk, with no
CMake and no pkg-config, and get the same numbers. This is that promise run:
a parent makefile is written into a scratch directory, includes the fragment
before its own first target, and builds tools/ephem/ephem.c against the
library at the fragment's -O2. Then both ephem binaries -- that one and
CMake's build/ephem at -O3 -- answer the same questions in CSV at %.17g, and
every row must agree as text.

Assertions (each fires on the defect it names):
  * no-contract     the fragment did not read the manifest's "contract N";
  * sources-differ  PROMETHEIA_SRCS is not the manifest's "src/" lines, or
                    one of them is not a file;
  * goal-taken      including the fragment first made its archive the
                    parent's default goal;
  * flags-leaked    the parent's CXXFLAGS reached a library compile line (a
                    -ffast-math there must never touch the library);
  * build-failed    the parent's binary was not built;
  * numbers-differ  its answers are not build/ephem's, row for row, on DE440
                    (not evaluated, and said so, when DE440 or build/ephem is
                    absent).

Usage: embedtest.py [--cxx CXX] [--cc CC] [--keep DIR] [-j N] [--list-assertions]
Run by tools/scheduled.sh: compiling the library takes too long for the gate.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assertlib  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
POISON = "-DPROMETHEIA_EMBED_PARENT_CXXFLAGS_LEAKED"

PARENT = """\
CXXFLAGS = {poison} -ffast-math
PROMETHEIA_DIR := {root}
PROMETHEIA_OBJDIR := obj-prometheia
include $(PROMETHEIA_DIR)/embed/prometheia.mk
CPPFLAGS += $(PROMETHEIA_CPPFLAGS)

all: ephem

ephem: ephem.o $(PROMETHEIA_LIB)
\t$(CXX) -o $@ ephem.o $(PROMETHEIA_LIB) $(PROMETHEIA_LDLIBS)

ephem.o: $(PROMETHEIA_DIR)/tools/ephem/ephem.c
\t$(CC) -std=c99 -O2 $(CPPFLAGS) -c $< -o $@

print-%:
\t@echo '$($*)'
"""

# Paths every leg of the library takes: planets apparent and geometric, a
# span of instants, topocentric with a sidereal zodiac, a heliocentric and an
# equatorial frame, orbit points, hypotheticals, a star, catalog bodies
# (zstd), and an instant before 1972 on UT1.
QUERIES = [
    ["-t", "2026-09-16T12:00", "-n", "4", "-s", "37d"],
    ["-t", "1600-03-01T00:00", "--scale", "tt", "--geometric"],
    ["-t", "2400-07-04T06:30", "--scale", "tt", "--equatorial", "--frame", "j2000"],
    ["-t", "2000-01-01T12:00", "--site", "8.55,47.37,500", "--sidereal", "lahiri", "sun", "moon"],
    ["-t", "1999-08-11T11:00", "--center", "helio", "earth", "mars", "jupiter"],
    ["-t", "2026-01-01", "--orbit-point", "asc:mean", "moon"],
    ["-t", "2026-01-01", "--orbit-point", "peri", "moon", "mars"],
    ["-t", "1900-01-01T12:00", "--scale", "ut1", "--delta-t", "-2.7", "moon"],
    ["-t", "2026-03-20", "hyp:all"],
    ["-t", "2026-03-20", "star:Regulus", "star:Spica"],
    ["-t", "2026-03-20", "-c", "tests/data/sample-100.epm", "--no-sigma", "@1", "@2", "@4"],
]


ASSERTIONS = ("no-contract", "sources-differ", "goal-taken", "flags-leaked", "build-failed",
              "numbers-differ")


def run(cmd, cwd, check=True):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, check=check)


def answers(ephem, de):
    out = []
    env = dict(os.environ, PROMETHEIA_EPHEMERIS=de)
    for q in QUERIES:
        p = subprocess.run([ephem, "-f", "csv"] + q, cwd=ROOT, env=env, capture_output=True,
                           text=True)
        # The header names the binary's version, never its path, so the two
        # agree in full; stderr and the exit status are compared too.
        out.append((q, p.returncode, p.stdout, p.stderr))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    ap.add_argument("--keep", help="build in this directory and leave it")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--ephemeris", default=os.path.join(ROOT, "ephe", "linux_p1550p2650.440"))
    ap.add_argument("--list-assertions", action="store_true")
    args = ap.parse_args()
    if args.list_assertions:
        return assertlib.Assertions(ASSERTIONS).list_and_exit()

    asserts = assertlib.Assertions(ASSERTIONS)
    fails = []
    work = args.keep or tempfile.mkdtemp(prefix="prometheia-embed-")
    os.makedirs(work, exist_ok=True)
    try:
        with open(os.path.join(work, "Makefile"), "w") as f:
            f.write(PARENT.format(root=ROOT, poison=POISON))
        make = ["make", "--no-print-directory", f"CXX={args.cxx}", f"CC={args.cc}"]

        # A fragment make cannot read at all is the first thing a parent
        # would see, so it is reported here rather than raised.
        c = run(make + ["-s", "print-PROMETHEIA_EMBED_CONTRACT"], work, check=False)
        contract = c.stdout.strip()
        if asserts.judge("no-contract", c.returncode != 0 or not contract.isdigit(),
                         f"no-contract: PROMETHEIA_EMBED_CONTRACT={contract!r} "
                         f"{c.stderr.strip()[-500:]}", into=fails):
            for msg in fails:
                print(msg)
            asserts.report()
            return 1

        with open(os.path.join(ROOT, "embed", "sources.txt")) as f:
            listed = [ln.strip() for ln in f if ln.startswith("src/")]
        srcs = run(make + ["-s", "print-PROMETHEIA_SRCS"], work).stdout.split()
        want = [os.path.join(ROOT, s) for s in listed]
        missing = [s for s in want if not os.path.isfile(s)]
        asserts.judge("sources-differ", srcs != want or not listed or bool(missing),
                      f"sources-differ: {len(srcs)} from the fragment, {len(listed)} listed, "
                      f"missing {missing}", into=fails)

        goal = run(make + ["-s", "print-.DEFAULT_GOAL"], work).stdout.strip()
        asserts.judge("goal-taken", goal != "all", f"goal-taken: .DEFAULT_GOAL={goal!r}",
                      into=fails)

        dry = run(make + ["-n", "-B", "all"], work).stdout.replace("\\\n", " ").splitlines()
        lib_lines = [ln for ln in dry if "/src/" in ln and " -c " in ln]
        # The marker, not -ffast-math itself: that one is numbers-differ's to
        # catch, wherever it came from, and one defect fires one assertion.
        leaked = [ln for ln in lib_lines if POISON in ln]
        # Fewer compile lines than sources would make "none leaked" vacuous.
        asserts.judge("flags-leaked", len(lib_lines) != len(listed) or bool(leaked),
                      f"flags-leaked: {len(lib_lines)} library compile lines for {len(listed)} "
                      f"sources, {len(leaked)} with the parent's flags", into=fails)

        b = run(make + [f"-j{args.j}", "all"], work, check=False)
        ephem = os.path.join(work, "ephem")
        asserts.judge("build-failed", b.returncode != 0 or not os.access(ephem, os.X_OK),
                      "build-failed: " + (b.stdout + b.stderr)[-2000:], into=fails)

        cmake_ephem = os.path.join(ROOT, "build", "ephem")
        if b.returncode or not os.path.isfile(args.ephemeris) or not os.access(cmake_ephem,
                                                                                os.X_OK):
            print(f"SKIP numbers-differ: needs {args.ephemeris} and {cmake_ephem}")
        else:
            ours, theirs = answers(ephem, args.ephemeris), answers(cmake_ephem, args.ephemeris)
            diff = [a[0] for a, c in zip(ours, theirs) if a[1:] != c[1:]]
            rows = sum(a[2].count("\n") for a in ours)
            failed = [a[0] for a in ours if a[1] != 0]
            asserts.judge("numbers-differ", bool(diff) or bool(failed) or rows <= len(QUERIES),
                          f"numbers-differ: {len(QUERIES)} queries, {rows} lines; differ: {diff}; "
                          f"failed: {failed}", into=fails)
            print(f"{args.cxx}: {len(QUERIES)} queries, {rows} CSV lines compared with build/ephem")
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
    for msg in fails:
        print(msg)
    asserts.report()
    return 1 if asserts.any_fired() else 0


if __name__ == "__main__":
    sys.exit(main())

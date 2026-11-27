#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Falsify every assertion ephproto4_fixtures.py makes, against a built set.

That reader is Prometheia's second, independent reading of protocol v4: it
shares no code with Astrolog's codec or with their fixture generator, so a
disagreement between the two readings is a place where the spec, one parser
or one fixture is wrong.  Every drop has been judged by it, and §3 is locked
on its verdict -- which makes its silent failures expensive.  It had three:
a manifest listing no fixture printed "0/0 agree" and exited 0; a manifest
with no checksum printed a note and exited 0, so nothing said the directory
had been read whole; and a missing JUDGEMENTS.tsv skipped the per-kind
drop's section 2 without a word.

The set here is built by `fakefixtures.py`, not Astrolog's.  Theirs is the
authority for what the bytes mean and is not in this tree -- on this machine
it is not on disk at all, so a selftest that needed it would skip, and a skip
whose precondition is met is a green that could not have been red.

assertlib.drive requires the named assertion to fire and no other, reads the
declared list from `--list-assertions`, and refuses an assertion no case
exercises.

**What this cannot see:** whether the reader reads the protocol correctly.
The built set's verdicts are what this reader should say, so a shared
misunderstanding stays invisible.  That is what the real conformance set is
for, and this does not replace it: it grades the reader's reaction to a set
that is empty, unsigned, inconsistent or wrong.

Usage:
  python3 tools/check/fixturestest.py [-v]
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assertlib  # noqa: E402
import fakefixtures  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

SERVED = [("request-7", "welcome-ok", "served"), ("request-3", "welcome-kind", "served")]


def rewrite(filename, fn):
    """A textual mutation of one table that must actually change it."""
    def apply(d):
        path = os.path.join(d, filename)
        with open(path) as f:
            before = f.read()
        after = fn(before)
        if after == before:
            raise SystemExit(f"this case did not change {filename}: it would have run "
                             "against the built set and passed for no reason")
        with open(path, "w") as f:
            f.write(after)
    return apply


def flip_a_verdict(text):
    out = []
    for line in text.splitlines(keepends=True):
        if line.startswith("hello-trailing.hex\t"):
            line = line.replace("\tmalformed\t", "\tok\t")
        out.append(line)
    return "".join(out)


def break_sha(text):
    out = []
    for line in text.splitlines(keepends=True):
        if line.startswith("# set-sha256 "):
            line = "# set-sha256 " + "0" * 64 + "\n"
        out.append(line)
    return "".join(out)


def flip_a_judgement(text):
    return text.replace("request-3\twelcome-ok\terror11", "request-3\twelcome-ok\tserved")


def resign(text):
    """Re-sign JUDGEMENTS.tsv after its body changed, so the case is about the
    judgement and not about the checksum -- two assertions on one input is how
    a deleted check stays invisible."""
    body = "".join(l for l in text.splitlines(keepends=True)
                   if l.strip() and not l.startswith("#"))
    return f"# set-sha256 {hashlib.sha256(body.encode()).hexdigest()}\n" + body


def cases():
    """name, how to build the directory, the assertions that must fire, exit."""
    return [
        # The control: the set as built, untouched. Without it the table shows
        # only that a broken set reds, never that a whole one stays green.
        ("the set as built", lambda d: None, set(), 0),

        # The failure this reader shipped with. The count printed comes from
        # the same manifest that lists nothing.
        ("the manifest lists no fixture",
         lambda d: fakefixtures.write(d, rows=[]), {"nothing-read"}, 1),

        # No checksum: the directory may have been read while the generator
        # was writing it, and nothing would say so.
        ("the manifest carries no checksum",
         lambda d: fakefixtures.write(d, set_sha=False), {"no-set-sha"}, 1),

        # A fixture reads differently here than the manifest says. This is the
        # finding the tool exists to produce.
        ("a fixture's verdict differs",
         rewrite("MANIFEST.tsv", flip_a_verdict), {"verdict-differs"}, 1),

        # The per-kind drop's section 2 goes unchecked, silently.
        ("JUDGEMENTS.tsv is absent",
         lambda d: fakefixtures.write(d, with_judgements=False), {"judgements-absent"}, 1),

        # A judgement differs. The table is re-signed so this is about the
        # judgement alone.
        ("a judgement differs",
         lambda d: rewrite("JUDGEMENTS.tsv", lambda t: resign(flip_a_judgement(t)))(d),
         {"judgement-differs"}, 1),

        # Both outcomes removed but one: a table like this passes a reader
        # that always answers "served", which is the one thing it must not do.
        ("the judgements table is one-sided",
         lambda d: fakefixtures.write(d, judge_rows=SERVED), {"judgements-one-sided"}, 1),

    ]


# The two refusals. An inconsistent set is not a finding: the directory may
# have been read while the generator was writing it, so the tool reports no
# verdict and exits 2. That is not an assertion, and it cannot be graded by
# the case table above -- both paths red identically there (no assertion,
# exit 2), so assertlib refuses them as one case, which is the truth: the
# table cannot tell them apart. What tells them apart is what they say, so
# they are graded on the message, and on the two messages differing from
# each other.
def refusals():
    """name, how to break it, the text that must appear."""
    return [
        ("the manifest's checksum does not match",
         rewrite("MANIFEST.tsv", break_sha), "set-sha256 MISMATCH"),
        ("the judgements checksum does not match",
         rewrite("JUDGEMENTS.tsv", break_sha), "JUDGEMENTS set-sha256 MISMATCH"),
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", default=os.path.join(ROOT, "tools/check/ephproto4_fixtures.py"))
    ap.add_argument("-v", "--verbose", action="store_true", help="echo each run's output")
    args = ap.parse_args()

    tmp = os.environ.get("CLAUDE_JOB_DIR")
    tmp = os.path.join(tmp, "tmp") if tmp else os.path.join(ROOT, "build")
    work = os.path.join(tmp, "fixturestest")

    table = cases()
    by_name = {c[0]: c for c in table}

    def run_one(name):
        _, build, _, _ = by_name[name]
        shutil.rmtree(work, ignore_errors=True)
        fakefixtures.write(work)
        build(work)
        out = subprocess.run([sys.executable, args.tool, "--dir", work],
                             capture_output=True, text=True, timeout=120)
        return out.returncode, out.stdout + out.stderr

    code = assertlib.drive(args.tool, [(c[0], c[2], c[3]) for c in table],
                           run_one, verbose=args.verbose)

    said = {}
    for name, build, want in refusals():
        shutil.rmtree(work, ignore_errors=True)
        fakefixtures.write(work)
        build(work)
        out = subprocess.run([sys.executable, args.tool, "--dir", work],
                             capture_output=True, text=True, timeout=120)
        text = out.stdout + out.stderr
        note = []
        if out.returncode != 2:
            note.append(f"exit {out.returncode}, wanted 2 (an inconsistent set is not a "
                        "finding, and not a pass either)")
        if want not in text:
            note.append(f"said nothing containing {want!r}")
        if "agree" in text.split(want)[-1]:
            note.append("went on to report verdicts about a set it had just called "
                        "inconsistent")
        said[name] = text
        print(f"{'ok' if not note else 'FAILED':6}  {name}")
        for line in note:
            print(f"          {line}")
        if note:
            code = 1
    # Two faults, two messages: if they read the same, either check could be
    # deleted and the other would cover for it.
    if len(set(said.values())) != len(said):
        print("FAILED  the two refusals print the same thing, so one could be deleted "
              "and the other would answer for it")
        code = 1

    shutil.rmtree(work, ignore_errors=True)
    return code


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Falsify every assertion ephproto4_registries.py makes, against a mutated pair.

That checker is the pin on another project's protocol: it says the vendored
`ephproto.h` and `registries.json` still agree, and `tools/gate.sh` runs it
before every commit.  What it defends against is the pair moving under us --
a value changed, a registry grown, a symbol renamed -- so the only honest
oracle is a pair that has moved.

**The vendored pair is never touched.** It is the byte-level authority for
protocol v4 (CLAUDE.md), so each case here copies both files to a temporary
directory, mutates the copy, and points the checker at it with `--header`
and `--registries`.  Every mutation asserts that it changed the file:
a `str.replace` that matches nothing leaves the file alone, the run then goes
green, and the green proves nothing.  That happened once on 2026-09-20 in
this repository and is why the assertion is here rather than in a comment.

assertlib.drive requires the named assertion to fire and no other, reads the
declared list from `--list-assertions`, and refuses an assertion no case
exercises.  That last refusal earned its keep immediately: a `nothing-checked`
assertion drafted for this tool turned out to be unfirable, because a
registries.json empty enough to make the count zero fires
`pin-lost-a-registry` twenty-two times first.  It was dropped rather than
shipped as a check that could not fail.

**What this cannot see:** whether the vendored pair matches what the Astrolog
project actually generates.  That is a question about their tree, answered by
re-vendoring, and it is the hole their own generator finding lives in (their
Appendix A scan, 2026-09-20).  This grades the checker, not the pin.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assertlib  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
V4 = os.path.join(ROOT, "third_party", "ephproto", "v4")


def edit_header(old, new):
    """A textual mutation of ephproto.h that must actually change it."""
    def apply(text, reg):
        if old not in text:
            raise SystemExit(f"the header no longer contains {old!r}: this case would have "
                             "run against an unmodified file and passed for no reason")
        return text.replace(old, new), reg
    return apply


def edit_json(fn):
    """A mutation of registries.json's parsed form; must change it."""
    def apply(text, reg):
        before = json.dumps(reg, sort_keys=True)
        fn(reg["registries"])
        if json.dumps(reg, sort_keys=True) == before:
            raise SystemExit("this case did not change registries.json: it would have run "
                             "against the vendored pair and passed for no reason")
        return text, reg
    return apply


def cases():
    """name, mutation, the assertions that must fire, exit code."""
    return [
        # The control: the vendored pair, copied and not touched. Without it
        # the table shows only that a broken pair reds, not that the real one
        # stays green -- and this check gates every commit.
        ("the vendored pair, unmodified", None, set(), 0),

        # A registry the JSON stopped carrying. It would simply drop out of
        # the loop, and the count printed comes from the same file that lost
        # it, so nothing else can notice.
        ("registries.json loses a registry",
         edit_json(lambda r: r.pop("frames")), {"pin-lost-a-registry"}, 1),

        # A registry added upstream that no table here pins. The pair is
        # consistent; our coverage of it is not.
        ("registries.json grows a registry this checker does not know",
         edit_json(lambda r: r.update({"tea_preferences": {"entries": []}})),
         {"registry-unknown"}, 1),

        # The header renames a constant. The entry it named must not also be
        # reported as "the registry grew" -- that would tell the reader to
        # re-vendor when the header is what moved.
        ("the header renames a symbol",
         edit_header("kFrameIcrf", "kFrameIcrfNg"), {"header-symbol-missing"}, 1),

        # One value differs. The pair disagrees about a number that is on the
        # wire.
        ("a value differs between the pair",
         edit_json(lambda r: _set_value(r["frames"], "ICRF", 9)), {"value-disagrees"}, 1),

        # An entry the header table does not cover: the registry grew and the
        # pin has not been re-vendored.
        ("the registry grows an entry",
         edit_json(lambda r: r["frames"]["entries"].append(
             {"name": "galactic", "value": 7})), {"registry-grew"}, 1),

        # A message type reserved by design -- the registry holds the value,
        # the header implements nothing -- that the header now implements.
        # The checker skips reserved values, so this is the one way its
        # coverage can shrink without any count changing.
        ("a reserved value the header now spells",
         edit_header("kMsgHello = 1,", "kMsgReserved11 = 11, kMsgHello = 1,"),
         {"reserved-now-implemented"}, 1),
    ]


def _set_value(registry, name, value):
    for e in registry["entries"]:
        if e.get("name") == name:
            e["value"] = value
            return
    raise SystemExit(f"no entry named {name!r} to change: the registry's shape has moved")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", default=os.path.join(ROOT, "tools/check/ephproto4_registries.py"))
    ap.add_argument("-v", "--verbose", action="store_true", help="echo each run's output")
    args = ap.parse_args()

    tmp = os.environ.get("CLAUDE_JOB_DIR")
    tmp = os.path.join(tmp, "tmp") if tmp else os.path.join(ROOT, "build")
    work = os.path.join(tmp, "registriestest")
    os.makedirs(work, exist_ok=True)

    with open(os.path.join(V4, "ephproto.h")) as f:
        header0 = f.read()
    with open(os.path.join(V4, "registries.json")) as f:
        json0 = f.read()

    table = cases()
    by_name = {c[0]: c for c in table}

    def run_one(name):
        _, mutate, _, _ = by_name[name]
        text, reg = header0, json.loads(json0)
        if mutate is not None:
            text, reg = mutate(text, reg)
        h = os.path.join(work, "ephproto.h")
        j = os.path.join(work, "registries.json")
        with open(h, "w") as f:
            f.write(text)
        with open(j, "w") as f:
            json.dump(reg, f, indent=2)
        out = subprocess.run([sys.executable, args.tool, "--header", h, "--registries", j],
                             capture_output=True, text=True, timeout=120)
        return out.returncode, out.stdout + out.stderr

    code = assertlib.drive(args.tool, [(c[0], c[2], c[3]) for c in table],
                           run_one, verbose=args.verbose)
    shutil.rmtree(work, ignore_errors=True)
    return code


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""sbdb_fetch.py's covariance overlay against scripted SBDB answers.

Astrolog found (2026-10-25) that one body's covariance JPL publishes with
no element values at its epoch -- every "A/" object's -- raised out of the
loop and cost the whole run, and so did one designation SBDB refused. This
drives `covariance()` with `fetch_single` replaced by a script, so no
request leaves the machine, over four designations: a body with a usable
covariance, an "A/"-style one, one SBDB refuses, and one with none.

Assertions (each fires on the defect it names):
  * run-stopped          a later designation was not reached;
  * unusable-written     the "A/" body's unusable covariance was written as
                         cells, or its row is missing;
  * reason-missing       the manifest does not say why it was not used;
  * refusal-silent       the refused designation did not make the exit
                         status 1;
  * good-cells-wrong     the usable covariance's cells were not written.

Hermetic and instant. In tools/gate.sh.
"""

import argparse
import contextlib
import io
import json
import os
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fetch"))
import assertlib  # noqa: E402
import sbdb_fetch  # noqa: E402

ASSERTIONS = ("run-stopped", "unusable-written", "reason-missing", "refusal-silent",
              "good-cells-wrong")

DATA = [[float(10 * i + j + 1) for j in range(6)] for i in range(6)]


def doc(spkid, cov):
    return {"object": {"spkid": str(spkid), "fullname": str(spkid)},
            "orbit": {"orbit_id": "1", "covariance": cov}}


GOOD_COV = {"epoch": "2460000.5", "labels": list(sbdb_fetch.COV_LABELS), "data": DATA,
            "elements": [{"name": n, "value": str(v)} for n, v in
                         (("e", 0.1), ("q", 2.5), ("tp", 2460100.0), ("om", 80.0),
                          ("w", 73.0), ("i", 10.0))]}
# What JPL sends for A/2018 V3: labels, a full matrix, an epoch, no elements.
A_COV = {"epoch": "2458644.5", "labels": list(sbdb_fetch.COV_LABELS), "data": DATA,
         "elements": []}

SCRIPT = {
    "good": doc(20000001, GOOD_COV),
    "A/2018 V3": doc(1003001, A_COV),
    "missing": None,  # SBDB refuses it
    "plain": doc(20000002, None),
}


def fake_fetch(des, covariance=False):
    answer = SCRIPT[des]
    if answer is None:
        raise sbdb_fetch.SbdbRefusal(f"SBDB found no unique object for {des!r}")
    return answer


def fake_row(d):
    return ["a", d["object"]["spkid"]] + [""] * (len(sbdb_fetch.DEFAULT_FIELDS) - 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list-assertions", action="store_true")
    if ap.parse_args().list_assertions:
        return assertlib.Assertions(ASSERTIONS).list_and_exit()
    asserts = assertlib.Assertions(ASSERTIONS)
    fails = []
    sbdb_fetch.fetch_single = fake_fetch
    sbdb_fetch.sbdb_single_to_row = fake_row
    with tempfile.TemporaryDirectory() as out:
        args = types.SimpleNamespace(covariance=list(SCRIPT), covariance_file=None,
                                     out_dir=out, delay=0)
        with contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            try:
                status = sbdb_fetch.covariance(args)
            except Exception as e:  # the defect itself: the loop raised
                status = f"raised {type(e).__name__}: {e}"
        with open(os.path.join(out, "covariance.tsv")) as f:
            rows = [ln.rstrip("\n").split("\t") for ln in f if not ln.startswith("#")]
        manifest = {}
        if os.path.exists(os.path.join(out, "manifest-covariance.json")):
            with open(os.path.join(out, "manifest-covariance.json")) as f:
                manifest = json.load(f)["fetched"]
    by_spkid = {r[1]: r for r in rows}
    ncov = len(sbdb_fetch.COV_FIELDS)

    asserts.judge("run-stopped", "20000002" not in by_spkid,
                  f"run-stopped: status {status!r}, rows for {sorted(by_spkid)}", into=fails)
    a_row = by_spkid.get("1003001")
    asserts.judge("unusable-written", a_row is None or any(a_row[-ncov:]),
                  f"unusable-written: row {a_row}", into=fails)
    asserts.judge("reason-missing", "covariance_unusable" not in manifest.get("A/2018 V3", {}),
                  f"reason-missing: manifest entry {manifest.get('A/2018 V3')}", into=fails)
    asserts.judge("refusal-silent", status != 1, f"refusal-silent: status {status!r}",
                  into=fails)
    good = by_spkid.get("20000001")
    asserts.judge("good-cells-wrong",
                  good is None or good[-ncov:][0] != "2460000.5" or "" in good[-ncov:],
                  f"good-cells-wrong: row {good}", into=fails)
    for msg in fails:
        print(msg)
    asserts.report()
    return 1 if asserts.any_fired() else 0


if __name__ == "__main__":
    sys.exit(main())

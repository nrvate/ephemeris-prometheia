# SPDX-License-Identifier: GPL-2.0-or-later
"""Run prometheia-wire-client and read what it prints.

One reader for the tools that drive a live v4 server through the reference
client (corrapplied.py, crosstest.py), so the client's output format is
parsed in exactly one place.  The client prints '#' lines for WELCOME, its
capabilities, the request id it chose (what prometheiad's log calls
req=<id>, so a verbose run can be matched to the server's lines) and each
object's metadata, and one line of six values per object per row.
"""

import re
import subprocess
import sys

WELCOME_RE = re.compile(r'^# WELCOME (\S+) protocol (\d+) engine "(.*)" dataset (.*) maxCells (\d+)$')
CORRMASK_RE = re.compile(r"^# corrmask observers (\d+) corrections (\d+)$")
CORRKIND_RE = re.compile(r"^# corrkind observers (\d+) kinds (\d+) corrections (\d+)$")
CAPS_RE = re.compile(r"^# caps (.*)$")
REQUEST_RE = re.compile(r"^# request (\d+)$")
META_RE = re.compile(r'^# object (\d+) name "(.*)" rowsOk (-?\d+) corr (\d+) err (\d+) "(.*)"$')
ROW_RE = re.compile(r"^(\d+) (\d+) (.+)$")


class Meta:
    """One object's metadata line."""

    def __init__(self, index, name, rows_ok, corr, err, err_text):
        self.index = index
        self.name = name
        self.rows_ok = rows_ok
        self.corr = corr
        self.err = err
        self.err_text = err_text


class Reply:
    """Everything one run of the client printed, parsed."""

    def __init__(self):
        self.returncode = None
        self.stderr = ""
        self.server = None
        self.engine = None
        self.dataset = None
        self.caps = {}
        self.corrmasks = []  # (observer bitmask, exact mask), A.3 0x0004
        self.request_id = None  # what prometheiad's log calls req=<id>
        self.corrkinds = []  # (observer bitmask, kind bitmask, exact mask), A.3 0x0014
        self.objects = []  # Meta, in request order
        self.rows = {}  # (object, row) -> [six floats]

    def ok(self):
        return self.returncode == 0 and bool(self.objects)

    def permitted(self, observer_bit, kind, mask):
        """Whether this server lists mask for an (observer, kind) pair: 0x0004
        for the observer, union every 0x0014 entry naming the pair (the
        per-kind drop, 1.1). A pair no 0x0014 entry names falls back to 0x0004."""
        if any(o & (1 << observer_bit) and m == mask for o, m in self.corrmasks):
            return True
        return any(o & (1 << observer_bit) and k & (1 << kind) and m == mask
                   for o, k, m in self.corrkinds)

    def row(self, obj, r=0):
        return self.rows.get((obj, r))


def _caps(text):
    """'kinds 63 observers 31 ... zodiacs a,b hypotheticals -' -> dict."""
    words = text.split()
    out = {}
    for k in range(0, len(words) - 1, 2):
        key, val = words[k], words[k + 1]
        if key in ("zodiacs", "hypotheticals"):
            out[key] = [] if val == "-" else val.split(",")
        else:
            out[key] = int(val)
    return out


def run(client, host, port, args, verbose=False, timeout=120):
    """Run the client once against host:port with the given extra arguments."""
    cmd = [client, "--host", host, "--port", str(port)] + list(args)
    if verbose:
        print("    $ " + " ".join(cmd), file=sys.stderr)
    done = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    rep = Reply()
    rep.returncode = done.returncode
    rep.stderr = done.stderr.strip()
    for line in done.stdout.splitlines():
        m = WELCOME_RE.match(line)
        if m:
            rep.server, rep.engine, rep.dataset = m.group(1), m.group(3), m.group(4)
            continue
        m = CORRMASK_RE.match(line)
        if m:
            rep.corrmasks.append((int(m.group(1)), int(m.group(2))))
            continue
        m = REQUEST_RE.match(line)
        if m:
            rep.request_id = int(m.group(1))
            continue
        m = CORRKIND_RE.match(line)
        if m:
            rep.corrkinds.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
            continue
        m = CAPS_RE.match(line)
        if m:
            rep.caps = _caps(m.group(1))
            continue
        m = META_RE.match(line)
        if m:
            rep.objects.append(Meta(int(m.group(1)), m.group(2), int(m.group(3)),
                                    int(m.group(4)), int(m.group(5)), m.group(6)))
            continue
        m = ROW_RE.match(line)
        if m:
            rep.rows[(int(m.group(1)), int(m.group(2)))] = [float(v) for v in m.group(3).split()]
    if verbose:
        # The join to the server's log: prometheiad logs this as req=<id>.
        print(f"      -> {rep.server} req={rep.request_id} exit {rep.returncode}", file=sys.stderr)
    return rep

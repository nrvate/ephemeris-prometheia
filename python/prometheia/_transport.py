# SPDX-License-Identifier: GPL-2.0-or-later
"""How the package reaches a Prometheia server (docs/PYTHON.md).

Two transports carry the same JSON (docs/JSON_API.md):

- StdioTransport starts ``prometheia-json --stdio`` as a child process and
  speaks MCP to it, one JSON-RPC message a line.
- HttpTransport posts to ``/v1/<tool>`` on a running ``prometheia-json
  --http``.

The binary protocol (prometheiad, v4) is the third, and is not written yet.
"""
import http.client
import json
import os
import shutil
import subprocess
import threading
import urllib.parse

from ._errors import Error

USER_AGENT = "prometheia-python"


class ToolError(Error):
    """A whole-call refusal: the arguments, not one object."""


def _answer(tool, body):
    if not isinstance(body, dict):
        raise Error("%s: the server answered something other than a JSON object" % tool)
    return body


class StdioTransport:
    """A local prometheia-json over stdio. Started on first use, stopped by close()."""

    def __init__(self, ephemeris, executable=None, catalogs=(), perturbers=()):
        if not ephemeris:
            raise Error("no ephemeris file: call set_ephe_path() with a JPL DE file or a "
                        "directory holding one, or set PROMETHEIA_EPHEMERIS")
        self.ephemeris = list(ephemeris)
        self.executable = executable or find_executable()
        self.extra_args = []
        for c in catalogs:
            self.extra_args += ["--catalog", c]
        for k in perturbers:
            self.extra_args += ["--perturbers", k]
        self._proc = None
        self._next_id = 0
        self._lock = threading.Lock()

    def _start(self):
        args = [self.executable, "--stdio", "--log-level", "quiet"]
        for path in self.ephemeris:
            args += ["--ephemeris", path]
        args += self.extra_args
        try:
            self._proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                          stderr=subprocess.PIPE, text=True, bufsize=1)
        except OSError as e:
            raise Error("could not start %s: %s" % (self.executable, e)) from None
        self._rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                 "clientInfo": {"name": USER_AGENT, "version": "0"}})
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def _send(self, msg):
        try:
            self._proc.stdin.write(json.dumps(msg) + "\n")
            self._proc.stdin.flush()
        except (BrokenPipeError, OSError):
            raise Error(self._died()) from None

    def _died(self):
        err = ""
        if self._proc is not None:
            try:
                self._proc.wait(timeout=1)
                err = self._proc.stderr.read().strip()
            except Exception:  # noqa: BLE001 -- only for the message
                pass
        if self._proc is not None:
            self._proc.stdout.close()
            self._proc.stderr.close()
        self._proc = None
        return "prometheia-json stopped" + (": " + err if err else "")

    def _rpc(self, method, params):
        self._next_id += 1
        self._send({"jsonrpc": "2.0", "id": self._next_id, "method": method, "params": params})
        line = self._proc.stdout.readline()
        if not line:
            raise Error(self._died())
        reply = json.loads(line)
        if "error" in reply:
            raise Error("%s: %s" % (method, reply["error"].get("message", reply["error"])))
        return reply["result"]

    def call(self, tool, arguments):
        with self._lock:
            if self._proc is None:
                self._start()
            result = self._rpc("tools/call", {"name": tool, "arguments": arguments})
        body = result.get("structuredContent")
        if result.get("isError"):
            message = body.get("message") if isinstance(body, dict) else None
            raise ToolError(message or result.get("content", [{}])[0].get("text", "refused"))
        return _answer(tool, body)

    def close(self):
        with self._lock:
            if self._proc is not None:
                try:
                    self._proc.stdin.close()
                    self._proc.wait(timeout=5)
                except Exception:  # noqa: BLE001
                    self._proc.kill()
                    self._proc.wait()
                self._proc.stdout.close()
                self._proc.stderr.close()
                self._proc = None


class HttpTransport:
    """A running prometheia-json --http, over one kept-alive connection."""

    def __init__(self, url, token=None, timeout=60.0):
        u = urllib.parse.urlsplit(url)
        if u.scheme not in ("http", "https"):
            raise Error("an HTTP server URL starts with http:// or https://")
        self.url = url
        self._conn_args = (u.hostname, u.port, u.scheme == "https", timeout)
        self._prefix = u.path.rstrip("/")
        self._token = token
        self._conn = None
        self._lock = threading.Lock()

    def _connection(self):
        host, port, tls, timeout = self._conn_args
        cls = http.client.HTTPSConnection if tls else http.client.HTTPConnection
        return cls(host, port, timeout=timeout)

    def call(self, tool, arguments):
        body = json.dumps(arguments).encode()
        headers = {"Content-Type": "application/json", "User-Agent": USER_AGENT}
        if self._token:
            headers["Authorization"] = "Bearer " + self._token
        with self._lock:
            for attempt in (0, 1):
                if self._conn is None:
                    self._conn = self._connection()
                try:
                    self._conn.request("POST", "%s/v1/%s" % (self._prefix, tool), body, headers)
                    resp = self._conn.getresponse()
                    data = resp.read()
                    break
                except (http.client.HTTPException, OSError) as e:
                    # A kept-alive connection the server closed: one fresh try.
                    self._conn.close()
                    self._conn = None
                    if attempt:
                        raise Error("%s: %s" % (self.url, e)) from None
        try:
            answer = json.loads(data)
        except ValueError:
            raise Error("%s answered %d with no JSON" % (self.url, resp.status)) from None
        if resp.status != 200:
            message = answer.get("message") if isinstance(answer, dict) else None
            raise ToolError(message or "HTTP %d" % resp.status)
        return _answer(tool, answer)

    def close(self):
        with self._lock:
            if self._conn is not None:
                self._conn.close()
                self._conn = None


def find_executable():
    """prometheia-json: $PROMETHEIA_JSON, then PATH, then this repository's build/."""
    env = os.environ.get("PROMETHEIA_JSON")
    if env:
        return env
    found = shutil.which("prometheia-json")
    if found:
        return found
    here = os.path.dirname(os.path.abspath(__file__))
    candidate = os.path.join(here, "..", "..", "build", "prometheia-json")
    if os.path.exists(candidate):
        return os.path.normpath(candidate)
    raise Error("prometheia-json not found: set PROMETHEIA_JSON or put it on PATH")


# Ephemeris files a directory is searched for, in answering order: DE440
# first, DE441 behind it for the dates DE440 does not reach (docs/DE.md).
EPHEMERIS_NAMES = (
    ("linux_p1550p2650.440", "de440.bsp"),
    ("linux_m13000p17000.441", "de441.bsp"),
)
# The asteroid perturber kernel, the full span first (tools/fetch/de_fetch.py
# --only sb441 provisions both).
PERTURBER_NAMES = ("sb441-n16.bsp", "sb441-n16-de440span.bsp")


def discover(path):
    """What a set_ephe_path() argument holds: (ephemerides, catalogs, perturbers).

    A file, several (os.pathsep), or a directory: in a directory DE440 and
    DE441, every *.epm small-body catalog, and the SB441 perturber kernel."""
    eph, cats, pert = [], [], []
    if not path:
        return eph, cats, pert
    for part in str(path).split(os.pathsep):
        if os.path.isdir(part):
            for names in EPHEMERIS_NAMES:
                for name in names:
                    candidate = os.path.join(part, name)
                    if os.path.exists(candidate):
                        eph.append(candidate)
                        break
            cats += sorted(os.path.join(part, n) for n in os.listdir(part) if n.endswith(".epm"))
            for name in PERTURBER_NAMES:
                candidate = os.path.join(part, name)
                if os.path.exists(candidate):
                    pert.append(candidate)
                    break
        elif part.endswith(".epm"):
            cats.append(part)
        elif part:
            eph.append(part)
    return eph, cats, pert

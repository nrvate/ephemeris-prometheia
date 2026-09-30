# SPDX-License-Identifier: GPL-2.0-or-later
"""Ephemeris Prometheia from Python, with no C extension (docs/PYTHON.md).

Two ways in:

- ``prometheia.swe``: shaped like pyswisseph, so chart code migrates by
  changing its import (``import prometheia.swe as swe``).
- ``prometheia.Client``: the JSON tools as they are (docs/JSON_API.md), a
  whole chart or a series in one call, every answer with its provenance.

Where the answers come from, first match wins:

1. ``set_server(url)`` or ``$PROMETHEIA_SERVER``: a ``ws(s)://`` address of a
   prometheiad (protocol v4, binary), or an ``http(s)://`` URL of a running
   ``prometheia-json --http``.
2. ``set_ephe_path(path)`` or ``$PROMETHEIA_EPHEMERIS``: a JPL DE file, or a
   directory holding DE440 (and DE441, used behind it), any ``*.epm``
   small-body catalogs and the SB441 perturber kernel; a local
   ``prometheia-json`` is started over stdio ($PROMETHEIA_JSON, PATH, or this
   repository's build/). Several paths join with os.pathsep, so a catalog
   kept elsewhere can be named beside the directory.
"""
import os

from ._errors import Error
from ._transport import HttpTransport, StdioTransport, ToolError, discover

__all__ = ["Client", "Error", "ToolError", "default_client", "set_server", "set_ephe_path",
           "close"]
__version__ = "0.7.3"


class Client:
    """One connection to a Prometheia server; the JSON tools as methods."""

    def __init__(self, server=None, ephemeris=None, executable=None, token=None, catalogs=(),
                 perturbers=()):
        if server:
            if server.startswith(("ws://", "wss://")):
                from ._binary import BinaryTransport
                self._transport = BinaryTransport(server, token=token)
            else:
                self._transport = HttpTransport(server, token=token)
        else:
            eph, cats, pert = discover(ephemeris)
            self._transport = StdioTransport(eph, executable, list(cats) + list(catalogs),
                                             list(pert) + list(perturbers))

    def call(self, tool, **arguments):
        """Any tool by name, arguments as keywords; the answer as parsed JSON."""
        return self._transport.call(tool, arguments)

    def positions(self, **arguments):
        return self.call("positions", **arguments)

    def houses(self, **arguments):
        return self.call("houses", **arguments)

    def convert_time(self, **arguments):
        return self.call("convert_time", **arguments)

    def lookup(self, **arguments):
        return self.call("lookup", **arguments)

    def capabilities(self):
        return self.call("capabilities")

    def close(self):
        self._transport.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


_default = None
_server = None
_ephemeris = None


def set_server(url, token=None):
    """Answer from a running prometheia-json --http at ``url``."""
    global _server, _default
    close()
    _server = (url, token)


def set_ephe_path(path):
    """Answer from a local prometheia-json on this ephemeris file or directory."""
    global _ephemeris, _server
    close()
    _server = None
    _ephemeris = path


def default_client():
    """The client prometheia.swe uses, made on first need from the settings above."""
    global _default
    if _default is None:
        if _server:
            _default = Client(server=_server[0], token=_server[1])
        elif os.environ.get("PROMETHEIA_SERVER") and _ephemeris is None:
            _default = Client(server=os.environ["PROMETHEIA_SERVER"],
                              token=os.environ.get("PROMETHEIA_TOKEN"))
        else:
            _default = Client(ephemeris=_ephemeris or os.environ.get("PROMETHEIA_EPHEMERIS"))
    return _default


def close():
    """Stop the default client's connection or child process."""
    global _default
    if _default is not None:
        _default.close()
        _default = None

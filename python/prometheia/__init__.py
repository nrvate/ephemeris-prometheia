# SPDX-License-Identifier: GPL-2.0-or-later
"""Ephemeris Prometheia from Python (docs/PYTHON.md): a native binding and a
pure-Python client.

``prometheia.Dataset`` and its per-thread ``Context`` run the engine in
process through the C interface, when the native extension is built
(``prometheia.native`` is then True): ``Dataset(de_file).local().calc(...)``.

``prometheia.Client`` gives the JSON tools as they are (docs/JSON_API.md), a
whole chart or a series in one call, every answer with its provenance, over
a server or a local prometheia-json; it needs no compiler.

A Client made with no arguments answers from, first match wins:

1. ``$PROMETHEIA_SERVER`` (with ``$PROMETHEIA_TOKEN``): a ``ws(s)://``
   address of a prometheiad (protocol v4, binary), or an ``http(s)://`` URL
   of a running ``prometheia-json --http``.
2. ``$PROMETHEIA_EPHEMERIS``: a JPL DE file, or a directory holding DE440
   (and DE441, used behind it), any ``*.epm`` small-body catalogs and the
   SB441 perturber kernel; a local ``prometheia-json`` is started over stdio
   ($PROMETHEIA_JSON, PATH, or this repository's build/). Several paths join
   with os.pathsep, so a catalog kept elsewhere can be named beside the
   directory.

There is no module-level state: every client is made, and closed, by its
caller.
"""
import os

from ._errors import Error
from ._transport import HttpTransport, StdioTransport, ToolError, discover

__all__ = ["Client", "Error", "ToolError", "native"]

# The native binding's public names, at the top of the package (the
# extension module itself is an implementation detail); _native.pyi types them.
try:
    from ._native import (BODIES, COORDINATES, CORRECTIONS, EVENTS, FRAMES, HOUSE_SYSTEMS, KINDS,
                          LIMBS, METHODS, OBSERVERS, OUTCOMES, POINTS, PRECESSION,
                          SIDEREAL_PLANES, SOLAR_KINDS, ZODIACS, Besselian, Context, Dataset,
                          Event, Horizontal, Houses, LunarEclipse, LunarEclipseLocal, Match,
                          ObjectInfo, Options, Phenomena, Position, SolarEclipse, SolarLocal,
                          SolarWhere, abi_version,
                          civil_from_jd, constellation_at, delta_t, ecliptic_to_equatorial,
                          equatorial_to_ecliptic, houses_from_armc, jd_from_ymdhms, refraction,
                          star_count, star_find, star_info, star_lookup, tdb_minus_tt, tt_to_utc,
                          utc_to_tt, version)
except ImportError:  # the extension is not built: the client alone
    native = False
else:
    native = True
    __all__ += ["BODIES", "COORDINATES", "CORRECTIONS", "EVENTS", "FRAMES", "HOUSE_SYSTEMS",
                "KINDS", "LIMBS", "METHODS", "OBSERVERS", "OUTCOMES", "POINTS", "PRECESSION",
                "SIDEREAL_PLANES", "SOLAR_KINDS", "ZODIACS", "Besselian", "Context", "Dataset",
                "Event", "Horizontal", "Houses", "LunarEclipse", "LunarEclipseLocal", "Match",
                "ObjectInfo", "Options", "Phenomena", "Position", "SolarEclipse", "SolarLocal",
                "SolarWhere", "abi_version",
                "civil_from_jd", "constellation_at", "delta_t", "ecliptic_to_equatorial",
                "equatorial_to_ecliptic", "houses_from_armc", "jd_from_ymdhms", "refraction",
                "star_count", "star_find", "star_info", "star_lookup", "tdb_minus_tt",
                "tt_to_utc", "utc_to_tt", "version"]
__version__ = "0.7.3"


class Client:
    """One connection to a Prometheia server; the JSON tools as methods."""

    def __init__(self, server=None, ephemeris=None, executable=None, token=None, catalogs=(),
                 perturbers=()):
        if server is None and ephemeris is None:
            server = os.environ.get("PROMETHEIA_SERVER") or None
            token = token or os.environ.get("PROMETHEIA_TOKEN")
            ephemeris = None if server else os.environ.get("PROMETHEIA_EPHEMERIS")
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

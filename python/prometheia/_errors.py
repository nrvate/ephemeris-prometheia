# SPDX-License-Identifier: GPL-2.0-or-later
"""The package's one exception type (``prometheia.swe.Error`` is this class)."""


class Error(Exception):
    """A refusal from the server, or a request this package cannot make.

    The message is the server's sentence where there is one: a name that is
    not served, an instant outside the ephemeris, a house system undefined at
    a latitude. Never a zero-filled answer in its place.
    """

    def __init__(self, message, code=None):
        super().__init__(message)
        self.code = code

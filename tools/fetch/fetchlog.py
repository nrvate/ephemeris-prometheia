# SPDX-License-Identifier: GPL-2.0-or-later
"""One timestamped stderr line per HTTP request of the fetch tools.

Every GET a fetch tool makes is logged with its UTC time and URL, then its
outcome (bytes and seconds, or the error and the retry wait). A pull that
runs for hours can then be read back against the manifest it wrote, and any
question from the service's operators about our traffic ("what did you ask
at 03:12?") has an answer.
"""

import sys
import time
from datetime import datetime, timezone


def utc():
    now = datetime.now(timezone.utc)
    return now.strftime("%Y-%m-%dT%H:%M:%S.") + f"{now.microsecond // 1000:03d}Z"


def log(msg):
    print(f"{utc()} {msg}", file=sys.stderr, flush=True)


class Timer:
    """`with Timer() as t: ...` then t.seconds."""

    def __enter__(self):
        self.start = time.monotonic()
        self.seconds = 0.0
        return self

    def __exit__(self, *exc):
        self.seconds = time.monotonic() - self.start
        return False

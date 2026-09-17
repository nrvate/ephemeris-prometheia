// SPDX-License-Identifier: GPL-2.0-or-later
//
// The doctest runner shared by every test executable. Each binary accepts
// doctest's command line, e.g. `-tc=<test case>` to run one case, `-s` to
// print passing assertions, `-d` for per-case durations.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

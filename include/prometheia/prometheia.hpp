// SPDX-License-Identifier: GPL-2.0-or-later
//
// Ephemeris Prometheia — cleanroom solar-system ephemeris.
//
// "Prometheia" (προμήθεια) is the Greek word for forethought, the quality
// Prometheus personifies. This library stores forethought (initial
// conditions) and computes positions by integrating, rather than storing
// answers baked years in advance.
#ifndef PROMETHEIA_PROMETHEIA_HPP
#define PROMETHEIA_PROMETHEIA_HPP

#define PROMETHEIA_VERSION_MAJOR 0
#define PROMETHEIA_VERSION_MINOR 1
#define PROMETHEIA_VERSION_PATCH 0
#define PROMETHEIA_VERSION "0.4.0"

namespace prometheia {

/// Library version as individual numbers and a string; mirrors the CMake
/// project version. Single source of truth is CMakeLists.txt — keep in sync.
inline constexpr int version_major = PROMETHEIA_VERSION_MAJOR;
inline constexpr int version_minor = PROMETHEIA_VERSION_MINOR;
inline constexpr int version_patch = PROMETHEIA_VERSION_PATCH;
inline constexpr const char* version_string = PROMETHEIA_VERSION;

} // namespace prometheia

#include <prometheia/catalog.hpp>
#include <prometheia/cbor.hpp>
#include <prometheia/error.hpp>

#endif // PROMETHEIA_PROMETHEIA_HPP

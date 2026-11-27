// SPDX-License-Identifier: GPL-2.0-or-later
//
// registry::detail::widened, built twice (CMakeLists.txt): as the library
// builds it, and as test_widened_fallbacks with PROMETHEIA_PORTABLE_FALLBACKS,
// the path Apple's libc++ below macOS 26 takes (no floating-point
// <charconv>). Both must give the shortest decimal of the float, read as a
// double.
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>

#include <prometheia/registry.hpp>

#include <doctest/doctest.h>

using prometheia::registry::detail::widened;

TEST_CASE("widened_gives_the_decimal_written") {
    CHECK(widened(-1.26f) == -1.26);
    CHECK(widened(0.15f) == 0.15);
    CHECK(widened(939.4f) == 939.4);
    CHECK(widened(3.34f) == 3.34);
    CHECK(widened(0.0f) == 0.0);
    CHECK(widened(1e-3f) == 1e-3);
    CHECK(widened(16777216.0f) == 16777216.0);
    CHECK(widened(90282311680.0f) == 90282311680.0); // whole: in full, as to_chars
    CHECK(std::isnan(widened(std::nanf(""))));
    CHECK(std::isinf(widened(-INFINITY)));
}

#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
TEST_CASE("widened_matches_to_chars") {
    // The reference: to_chars's shortest decimal, read back by from_chars.
    const auto reference = [](float f) {
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof buf, f);
        double d = double(f);
        std::from_chars(buf, r.ptr, d);
        return d;
    };
    std::mt19937 rng(7);
    int differ = 0, n = 0;
    const auto check = [&](float f) {
        if (!std::isfinite(f))
            return;
        ++n;
        const double a = widened(f), b = reference(f);
        differ += std::memcmp(&a, &b, sizeof a) != 0;
    };
    for (int i = -3000; i <= 3000; ++i)
        check(float(i) / 100.0f); // H and G as catalogs give them
    for (int i = 0; i < 200000; ++i) {
        const uint32_t u = rng();
        float f;
        std::memcpy(&f, &u, sizeof f);
        check(f); // every magnitude
    }
    MESSAGE(n << " floats");
    CHECK(differ == 0);
}
#endif

// SPDX-License-Identifier: GPL-2.0-or-later
//
// Minimal zero-dependency test harness. Each test binary is a single
// translation unit that defines TEST cases and returns run_all().
#ifndef PROMETHEIA_TESTS_TEST_MAIN_HPP
#define PROMETHEIA_TESTS_TEST_MAIN_HPP

#include <cstdio>
#include <vector>

namespace ptest {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}

inline int& failures() {
    static int f = 0;
    return f;
}

inline int run_all() {
    for (const Case& c : cases()) {
        const int before = failures();
        c.fn();
        std::printf("[%s] %s\n", failures() == before ? "PASS" : "FAIL", c.name);
    }
    std::printf("%s: %d failure(s), %zu case(s)\n", failures() == 0 ? "OK" : "FAILED", failures(),
                cases().size());
    return failures() == 0 ? 0 : 1;
}

} // namespace ptest

#define TEST(name)                                                                                 \
    static void test_fn_##name();                                                                  \
    static const bool reg_##name =                                                                 \
        (ptest::cases().push_back(ptest::Case{#name, &test_fn_##name}), true);                     \
    static void test_fn_##name()

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            ++ptest::failures();                                                                   \
            std::printf("  CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr);               \
        }                                                                                          \
    } while (0)

#endif // PROMETHEIA_TESTS_TEST_MAIN_HPP

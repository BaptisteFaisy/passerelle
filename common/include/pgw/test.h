/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pgw/test.h - the smallest test harness that still reports what we need.
 *
 * Why our own: the tests must build with nothing but a C11 compiler, both on
 * the PC (CI) and inside a QNX guest image (the conformance bench of host/fsproof
 * runs the very same assertions there). No allocation, no globals beyond a
 * static table, one line of output per test.
 *
 * Usage:
 *
 *   #include "pgw/test.h"
 *
 *   static void test_decodes_rversion(void) { PGW_ASSERT_EQ(1, 1); }
 *   PGW_TEST(test_decodes_rversion);
 *
 *   // nothing else: PGW_TEST_MAIN() is provided by the library
 *
 * PGW_TEST registers the case at load time; the generated main runs them all,
 * prints "ok N name" / "not ok N name (file:line: message)", and exits non-zero
 * on the first failing assertion of any case. CTest parses that output.
 */
#ifndef PGW_TEST_H
#define PGW_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pgw_test_fn)(void);

/* Failure reporting. The runner installs a setjmp/longjmp frame around each
 * case, so pgw_fail records the first failure and abandons that case only:
 * the remaining cases still run and the exit status still reflects the
 * failure. From the caller's point of view it does not return. */
void pgw_fail(const char *file, int line, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4), noreturn))
#endif
    ;

bool pgw_case_failed(void); /* true while the running case has failed */
void pgw_case_reset(void);

/* Registration. `PGW_TEST(f)` puts f in a dedicated section; the harness walks
 * the section bounds. On toolchains without section support, fall back to an
 * explicit table the test file defines with PGW_SUITE(...). */
#if defined(__GNUC__) || defined(__clang__)
#define PGW_TEST(fn)                                                        \
    static pgw_test_fn const pgw_test_entry_##fn                            \
        __attribute__((used, section("pgw_tests"))) = fn
#else
#error "PGW_TEST needs section support; add this toolchain to the harness"
#endif

/* Provided by common: runs every registered case, returns the exit status. */
int pgw_test_main(int argc, char **argv);

/* --- assertions ----------------------------------------------------------- */

#define PGW_ASSERT(cond)                                                     \
    do {                                                                     \
        if (!(cond))                                                         \
            pgw_fail(__FILE__, __LINE__, "expected true: %s", #cond);        \
    } while (0)

#define PGW_ASSERT_FALSE(cond)                                               \
    do {                                                                     \
        if (cond)                                                            \
            pgw_fail(__FILE__, __LINE__, "expected false: %s", #cond);       \
    } while (0)

#define PGW_ASSERT_EQ_I(got, want)                                           \
    do {                                                                     \
        long long g_ = (long long)(got), w_ = (long long)(want);             \
        if (g_ != w_)                                                        \
            pgw_fail(__FILE__, __LINE__, "%s: got %lld, want %lld", #got,    \
                     g_, w_);                                                \
    } while (0)

#define PGW_ASSERT_EQ_U(got, want)                                           \
    do {                                                                     \
        unsigned long long g_ = (unsigned long long)(got);                   \
        unsigned long long w_ = (unsigned long long)(want);                  \
        if (g_ != w_)                                                        \
            pgw_fail(__FILE__, __LINE__, "%s: got %llu, want %llu", #got,    \
                     g_, w_);                                                \
    } while (0)

#define PGW_ASSERT_EQ_PTR(got, want)                                         \
    do {                                                                     \
        const void *g_ = (const void *)(got);                                \
        const void *w_ = (const void *)(want);                               \
        if (g_ != w_)                                                        \
            pgw_fail(__FILE__, __LINE__, "%s: got %p, want %p", #got, g_,    \
                     w_);                                                    \
    } while (0)

#define PGW_ASSERT_STR_EQ(got, want)                                         \
    do {                                                                     \
        const char *g_ = (got), *w_ = (want);                                \
        if (g_ == NULL || w_ == NULL || strcmp(g_, w_) != 0)                  \
            pgw_fail(__FILE__, __LINE__, "%s: got \"%s\", want \"%s\"",      \
                     #got, g_ ? g_ : "(null)", w_ ? w_ : "(null)");          \
    } while (0)

#define PGW_ASSERT_MEM_EQ(got, want, n)                                      \
    do {                                                                     \
        if (memcmp((got), (want), (n)) != 0)                                 \
            pgw_fail(__FILE__, __LINE__, "%s: %zu bytes differ", #got,       \
                     (size_t)(n));                                           \
    } while (0)

/* --- helpers shared by many test files ------------------------------------ */

/* Fill `n` bytes with a deterministic pattern seeded by `seed`, so a test can
 * prove that content survived a round trip without storing a copy. */
void pgw_test_pattern(void *buf, size_t n, uint32_t seed, uint64_t offset);
bool pgw_test_pattern_ok(const void *buf, size_t n, uint32_t seed,
                         uint64_t offset);

/* A temporary directory that removes itself when the process exits. Used by
 * the differential and reference-server tests. Returns NULL on failure. */
char *pgw_test_tmpdir(const char *prefix);

#ifdef __cplusplus
}
#endif

#endif /* PGW_TEST_H */

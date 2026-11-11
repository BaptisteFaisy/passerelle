/* SPDX-License-Identifier: Apache-2.0 */
/*
 * clab_check.h - the three-line assertion macro the codelab's tests use.
 *
 * The codelab's exercises are meant to be read and modified, so they must not
 * depend on the Passerelle build (no CMake helper, no pgw_test harness): this
 * header and a main() are all a reader needs. Chapter 1 explains why the
 * exercises are separate programs rather than part of the project's build.
 */
#ifndef CLAB_CHECK_H
#define CLAB_CHECK_H

#include <stdio.h>
#include <string.h>

static int clab_failures;

#define CLAB_CHECK(cond)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            clab_failures++;                                                   \
        }                                                                      \
    } while (0)

#define CLAB_CHECK_EQ(got, want)                                               \
    do {                                                                       \
        long long g_ = (long long)(got), w_ = (long long)(want);               \
        if (g_ != w_) {                                                        \
            fprintf(stderr, "%s:%d: %s == %s (%lld != %lld)\n", __FILE__,      \
                    __LINE__, #got, #want, g_, w_);                            \
            clab_failures++;                                                   \
        }                                                                      \
    } while (0)

#define CLAB_CHECK_STR(got, want)                                              \
    do {                                                                       \
        const char *g_ = (got), *w_ = (want);                                  \
        if (g_ == NULL || w_ == NULL || strcmp(g_, w_) != 0) {                 \
            fprintf(stderr, "%s:%d: %s: got \"%s\", want \"%s\"\n", __FILE__,     \
                    __LINE__, #got, g_ ? g_ : "(null)", w_ ? w_ : "(null)");   \
            clab_failures++;                                                   \
        }                                                                      \
    } while (0)

#define CLAB_CHECK_MEM(got, want, n)                                           \
    do {                                                                       \
        if (memcmp((got), (want), (n)) != 0) {                                 \
            fprintf(stderr, "%s:%d: %s: %zu bytes differ\n", __FILE__,         \
                    __LINE__, #got, (size_t)(n));                              \
            clab_failures++;                                                   \
        }                                                                      \
    } while (0)

/* Return from main(): 0 when every check passed. */
static int clab_result(void)
{
    if (clab_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", clab_failures);
        return 1;
    }
    return 0;
}

#endif /* CLAB_CHECK_H */

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * common/src/test.c - minimal C11 test runner (section-based + TAP-like output).
 */
#define _XOPEN_SOURCE 700
#include "pgw/test.h"

#include <ftw.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern pgw_test_fn const __start_pgw_tests[];
extern pgw_test_fn const __stop_pgw_tests[];

static jmp_buf g_case_jmp;
static bool g_failed = false;
static char g_fail_msg[256];

#define MAX_TMPDIRS 16
static char g_tmpdirs[MAX_TMPDIRS][256];
static int g_ntmpdirs = 0;
static bool g_atexit_reg = false;

static int rm_cb(const char *fpath, const struct stat *sb, int typeflag,
                 struct FTW *ftwbuf)
{
    (void)sb;
    (void)typeflag;
    (void)ftwbuf;
    return remove(fpath);
}

static void cleanup_tmpdirs(void)
{
    for (int i = 0; i < g_ntmpdirs; i++) {
        if (g_tmpdirs[i][0] != '\0')
            nftw(g_tmpdirs[i], rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    }
}

void pgw_fail(const char *file, int line, const char *fmt, ...)
{
    g_failed = true;
    int off = snprintf(g_fail_msg, sizeof(g_fail_msg), "%s:%d: ", file, line);
    if (off < 0 || (size_t)off >= sizeof(g_fail_msg))
        off = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_fail_msg + off, sizeof(g_fail_msg) - (size_t)off, fmt, ap);
    va_end(ap);
    g_fail_msg[sizeof(g_fail_msg) - 1] = '\0';
    longjmp(g_case_jmp, 1);
}

bool pgw_case_failed(void)
{
    return g_failed;
}

void pgw_case_reset(void)
{
    g_failed = false;
    g_fail_msg[0] = '\0';
}

static uint32_t splitmix_step(uint32_t seed, uint64_t off)
{
    uint64_t x = ((uint64_t)seed << 32) ^ off ^ 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return (uint32_t)((x ^ (x >> 31)) & 0xFFu);
}

void pgw_test_pattern(void *buf, size_t n, uint32_t seed, uint64_t offset)
{
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)splitmix_step(seed, offset + i);
}

bool pgw_test_pattern_ok(const void *buf, size_t n, uint32_t seed,
                         uint64_t offset)
{
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != (uint8_t)splitmix_step(seed, offset + i))
            return false;
    }
    return true;
}

char *pgw_test_tmpdir(const char *prefix)
{
    if (g_ntmpdirs >= MAX_TMPDIRS)
        return NULL;
    char tmpl[256];
    const char *pfx = prefix ? prefix : "pgw";
    snprintf(tmpl, sizeof(tmpl), "/tmp/%s-XXXXXX", pfx);
    char *res = mkdtemp(tmpl);
    if (res == NULL)
        return NULL;
    if (!g_atexit_reg) {
        atexit(cleanup_tmpdirs);
        g_atexit_reg = true;
    }
    snprintf(g_tmpdirs[g_ntmpdirs], sizeof(g_tmpdirs[0]), "%s", res);
    return g_tmpdirs[g_ntmpdirs++];
}

int pgw_test_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    size_t total = (size_t)(__stop_pgw_tests - __start_pgw_tests);
    size_t failures = 0;
    printf("1..%zu\n", total);
    for (size_t i = 0; i < total; i++) {
        pgw_case_reset();
        if (setjmp(g_case_jmp) == 0) {
            __start_pgw_tests[i]();
        }
        if (g_failed) {
            printf("not ok %zu (%s)\n", i + 1, g_fail_msg);
            failures++;
        } else {
            printf("ok %zu\n", i + 1);
        }
    }
    return failures == 0 ? 0 : 1;
}

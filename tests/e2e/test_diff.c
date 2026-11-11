/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_diff.c - differential conformance suite on the PC (spec 7.2).
 *
 * Seeded random operation suites are replayed through pgw_rmfs and compared
 * with the real directory after every operation and once more for the whole
 * tree at the end. On divergence the failing suite is reduced to its shortest
 * reproducible prefix, which is exactly the artefact a regression test needs.
 *
 * The number of operations is tunable so the same file serves the CI (fast) and
 * the long campaign (spec 7.6: 1 000 000 operations):
 *   PGW_DIFF_OPS=1000000 ./test_diff
 */
#define _POSIX_C_SOURCE 200809L
#include "diff.h"
#include "testfs.h"

#include "pgw/limits.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_OPS 1000
#define SEED_COUNT 3

static size_t ops_from_env(void)
{
    const char *v = getenv("PGW_DIFF_OPS");
    if (v == NULL || *v == '\0')
        return DEFAULT_OPS;
    long n = strtol(v, NULL, 10);
    if (n < 1 || n > PGW_DIFF_MAX_OPS)
        return DEFAULT_OPS;
    return (size_t)n;
}

static void run_suite(uint32_t seed, size_t n_ops)
{
    /* The suite is heap-allocated: the 1 000 000-operation campaign of spec 7.6
     * is 24 MB of operations, too large for a static array, and a static cap
     * would silently truncate the campaign (it did: an env value above the cap
     * fell back to 1 000 operations while looking like a million). */
    diff_op *ops = calloc(n_ops, sizeof(*ops));
    if (ops == NULL)
        pgw_fail(__FILE__, __LINE__, "cannot allocate %zu operations", n_ops);
    char err[PGW_DIFF_MSG];
    diff_gen(ops, n_ops, seed);
    long bad = diff_run(ops, n_ops, seed, err, sizeof(err));
    if (bad < 0) {
        free(ops);
        return;
    }
    long minimal = diff_reduce(ops, n_ops, seed, err, sizeof(err));
    const char *kind = (size_t)minimal <= n_ops && minimal > 0
                           ? diff_kind_name(ops[minimal - 1].kind)
                           : diff_kind_name(ops[bad < (long)n_ops ? bad : 0].kind);
    long at = minimal > 0 ? minimal : bad;
    free(ops);
    pgw_fail(__FILE__, __LINE__, "seed %u: divergence at op %ld (%s); %s", seed,
             at, kind, err);
}

static void test_diff_random_suites(void)
{
    size_t n = ops_from_env();
    for (uint32_t i = 0; i < SEED_COUNT; i++)
        run_suite(0x9E3779B9u * (i + 1u), n);
}
PGW_TEST(test_diff_random_suites);

/* FS-11: a file deleted while open stays readable until it is closed. */
static void test_deleted_but_open_stays_readable(void)
{
    testfs t;
    PGW_ASSERT(testfs_open(&t, "pgwfs11", NULL));
    pgw_rmfs *fs = t.fs;
    pgw_posix_backend *be = t.be;
    pgw_fs_cred cred = t.cred;

    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "gone.txt",
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644,
                                  &fd),
                    PGW_OK);
    const char *payload = "still here after unlink";
    size_t written = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_write(fs, fd, 0, payload, strlen(payload),
                                   &written),
                    PGW_OK);
    PGW_ASSERT_EQ_U(written, strlen(payload));

    PGW_ASSERT_EQ_I(pgw_rmfs_unlink(fs, &cred, "gone.txt", false), PGW_OK);

    char buf[64] = { 0 };
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_read(fs, fd, 0, buf, sizeof(buf) - 1, &got),
                    PGW_OK);
    PGW_ASSERT_EQ_U(got, strlen(payload));
    PGW_ASSERT_STR_EQ(buf, payload);

    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_posix_backend_open_handles(be), 0);
    testfs_close(&t);
}
PGW_TEST(test_deleted_but_open_stays_readable);

/* FS-12: a change made on the PC is visible on the next read, no remount. */
static void test_host_change_visible_without_remount(void)
{
    testfs t;
    PGW_ASSERT(testfs_open(&t, "pgwfs12", NULL)); /* no cache by default */
    pgw_rmfs *fs = t.fs;
    pgw_fs_cred cred = t.cred;

    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "shared.txt",
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644,
                                  &fd),
                    PGW_OK);
    size_t done = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_write(fs, fd, 0, "first", 5, &done), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);

    /* The PC side rewrites the file behind the share's back. */
    char abs[512];
    testfs_abs(&t, "shared.txt", abs, sizeof(abs));
    FILE *f = fopen(abs, "wb");
    PGW_ASSERT(f != NULL);
    fputs("second-and-longer", f);
    fclose(f);

    pgw_fs_attr at;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "shared.txt", &at), PGW_OK);
    PGW_ASSERT_EQ_U(at.size, strlen("second-and-longer"));

    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "shared.txt", PGW_O_RDONLY, 0, &fd),
                    PGW_OK);
    char buf[64] = { 0 };
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_read(fs, fd, 0, buf, sizeof(buf) - 1, &got),
                    PGW_OK);
    PGW_ASSERT_STR_EQ(buf, "second-and-longer");
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);
    testfs_close(&t);
}
PGW_TEST(test_host_change_visible_without_remount);

/* ROB-02 / P9-07: no descriptor leaks after many open-close cycles. */
static void test_no_handle_leak_after_many_cycles(void)
{
    testfs t;
    PGW_ASSERT(testfs_open(&t, "pgwleak", NULL));
    pgw_rmfs *fs = t.fs;
    pgw_posix_backend *be = t.be;
    pgw_fs_cred cred = t.cred;

    const long cycles = 20000;
    for (long i = 0; i < cycles; i++) {
        pgw_rmfs_fd *fd = NULL;
        char name[32];
        snprintf(name, sizeof(name), "cycle-%ld.txt", i % 8);
        PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, name,
                                      PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                        PGW_OK);
        PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);
    }
    PGW_ASSERT_EQ_U(pgw_rmfs_open_count(fs), 0);
    PGW_ASSERT_EQ_U(pgw_posix_backend_open_handles(be), 0);
    testfs_close(&t);
}
PGW_TEST(test_no_handle_leak_after_many_cycles);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_fuse_diff.c - the differential conformance bench through the FUSE
 * adapter (VFS-05, gap 25).
 *
 * The bench is the one of tests/e2e: the same seeded suites, the same
 * generation, the same verification against the real directory. Only the
 * mount changes: rmfs is built over the FUSE adapter (pgw_rmfs_from_fuse)
 * instead of over the POSIX truth backend, and the truth is the directory
 * the reference FUSE server serves. Nothing is copied: the bench installs
 * its mount provider (diff_set_mount_provider) and calls diff_run().
 *
 * A divergence is reported with the seed and reduced to the shortest prefix
 * that still reproduces it, exactly as the POSIX edition does.
 *
 * Covers VFS-05 (gap 25 of docs/gap-analysis.md), FS-01..FS-15, PERF-07.
 */
#define _POSIX_C_SOURCE 200809L
#include "diff.h"
#include "fuse_mount.h"

#include "pgw/err.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"

#include <stdlib.h>
#include <string.h>

#define DEFAULT_OPS 400
#define SEED_COUNT 2

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

/* One suite through the FUSE adapter. */
static void run_suite(uint32_t seed, size_t n_ops)
{
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
    const char *kind = (size_t)minimal <= n_ops && minimal > 0 ?
                               diff_kind_name(ops[minimal - 1].kind) :
                               diff_kind_name(ops[bad < (long)n_ops ? bad : 0].kind);
    long at = minimal > 0 ? minimal : bad;
    free(ops);
    pgw_fail(__FILE__, __LINE__, "seed %u (FUSE): divergence at op %ld (%s); %s", seed,
             at, kind, err);
}

static void test_diff_random_suites_over_fuse(void)
{
    diff_set_mount_provider(fuse_diff_mount, NULL);
    size_t n = ops_from_env();
    for (uint32_t i = 0; i < SEED_COUNT; i++)
        run_suite(0x9E3779B9u * (i + 1u), n);
    diff_set_mount_provider(NULL, NULL);
}
PGW_TEST(test_diff_random_suites_over_fuse);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}

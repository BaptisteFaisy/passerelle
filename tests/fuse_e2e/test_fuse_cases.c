/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_fuse_cases.c - the directed cases through the FUSE adapter.
 *
 * The case bodies are the ones of tests/cas (cas_cases.h): they take a
 * cas_view and nothing else, so the same assertions that run over the POSIX
 * backend and over the 9P adapter run here over the FUSE adapter (the
 * reference server of tests/fuse_ref serving a real directory, the truth).
 *
 * Covers ROB-04 (gap 19) and gap 68 through the FUSE adapter, VFS-05.
 */
#define _POSIX_C_SOURCE 200809L
#include "cas.h"

#include "cas_cases.h"
#include "fuse_mount.h"

#include "test_cred.h"

#include "pgw/rmfs.h"
#include "pgw/test.h"

/* The validity of the attribute cache of the cache cases. */
#define CACHE_TTL_NS 1000000000ULL

static void with_fuse(void (*body)(const cas_view *))
{
    fuse_mount m;
    PGW_ASSERT(fuse_mount_open(&m, NULL));
    cas_view v;
    v.fs = m.fs;
    v.root = m.root;
    v.cred = tc_self(10);
    body(&v);
    fuse_mount_close(&m);
}

/* The cache cases need their own mount: the cache is a mount option. */
static void with_fuse_cache(void (*body)(const cas_view *))
{
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    opts.attr_ttl_ns = CACHE_TTL_NS;
    fuse_mount m;
    PGW_ASSERT(fuse_mount_open(&m, &opts));
    cas_view v;
    v.fs = m.fs;
    v.root = m.root;
    v.cred = tc_self(10);
    body(&v);
    fuse_mount_close(&m);
}

static void test_replace_over_fuse(void)
{
    with_fuse(cas_case_replace);
}
PGW_TEST(test_replace_over_fuse);

static void test_truncate_over_fuse(void)
{
    with_fuse(cas_case_truncate);
}
PGW_TEST(test_truncate_over_fuse);

static void test_unlink_over_fuse(void)
{
    with_fuse(cas_case_unlink);
}
PGW_TEST(test_unlink_over_fuse);

static void test_cache_over_fuse(void)
{
    with_fuse_cache(cas_case_cache_stale);
}
PGW_TEST(test_cache_over_fuse);

static void test_link_identity_over_fuse(void)
{
    with_fuse(cas_case_link_identity);
}
PGW_TEST(test_link_identity_over_fuse);

static void test_link_unlink_over_fuse(void)
{
    with_fuse(cas_case_link_unlink);
}
PGW_TEST(test_link_unlink_over_fuse);

static void test_link_rename_over_fuse(void)
{
    with_fuse(cas_case_link_rename);
}
PGW_TEST(test_link_rename_over_fuse);

static void test_link_target_over_fuse(void)
{
    with_fuse(cas_case_link_target);
}
PGW_TEST(test_link_target_over_fuse);

static void test_link_cache_over_fuse(void)
{
    with_fuse_cache(cas_case_link_cache);
}
PGW_TEST(test_link_cache_over_fuse);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}

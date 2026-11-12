/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_cas_hardlink.c - gap 68 over the two adapters of the fixture.
 *
 * The case bodies live in cas_cases_hardlink.c: they are written once and
 * driven here over the POSIX backend (the layer alone) and over the 9P
 * adapter (the view a real client has). The FUSE adapter drives the very
 * same bodies from tests/fuse_e2e.
 *
 * Covers gap 68 of docs/gap-analysis.md, FS-05, FS-13.
 */
#define _POSIX_C_SOURCE 200809L
#include "cas.h"
#include "cas_9p.h"

#include "cas_cases.h"

#include "pgw/rmfs.h"
#include "pgw/test.h"

/* The validity of the attribute cache of the cache case. */
#define CACHE_TTL_NS 1000000000ULL

static void with_posix(void (*body)(const cas_view *))
{
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgw-hardlink", NULL, NULL));
    cas_view v;
    cas_view_of_mount(&v, &m);
    body(&v);
    cas_mount_close(&m);
}

static void with_9p(void (*body)(const cas_view *))
{
    cas_9p m;
    PGW_ASSERT(cas_9p_open(&m, "pgw-hardlink-9p", NULL));
    cas_view v;
    cas_view_of_9p(&v, &m);
    body(&v);
    cas_9p_close(&m);
}

/* The cache case needs its own mount: the cache is a mount option. */
static void with_posix_cache(void (*body)(const cas_view *))
{
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    opts.attr_ttl_ns = CACHE_TTL_NS;
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgw-hardlink-cache", &opts, NULL));
    cas_view v;
    cas_view_of_mount(&v, &m);
    body(&v);
    cas_mount_close(&m);
}

static void with_9p_cache(void (*body)(const cas_view *))
{
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    opts.attr_ttl_ns = CACHE_TTL_NS;
    cas_9p m;
    PGW_ASSERT(cas_9p_open(&m, "pgw-hardlink-cache-9p", &opts));
    cas_view v;
    cas_view_of_9p(&v, &m);
    body(&v);
    cas_9p_close(&m);
}

static void test_link_identity_posix(void)
{
    with_posix(cas_case_link_identity);
}
PGW_TEST(test_link_identity_posix);

static void test_link_identity_9p(void)
{
    with_9p(cas_case_link_identity);
}
PGW_TEST(test_link_identity_9p);

static void test_link_unlink_posix(void)
{
    with_posix(cas_case_link_unlink);
}
PGW_TEST(test_link_unlink_posix);

static void test_link_unlink_9p(void)
{
    with_9p(cas_case_link_unlink);
}
PGW_TEST(test_link_unlink_9p);

static void test_link_rename_posix(void)
{
    with_posix(cas_case_link_rename);
}
PGW_TEST(test_link_rename_posix);

static void test_link_rename_9p(void)
{
    with_9p(cas_case_link_rename);
}
PGW_TEST(test_link_rename_9p);

static void test_link_target_posix(void)
{
    with_posix(cas_case_link_target);
}
PGW_TEST(test_link_target_posix);

static void test_link_target_9p(void)
{
    with_9p(cas_case_link_target);
}
PGW_TEST(test_link_target_9p);

static void test_link_cache_posix(void)
{
    with_posix_cache(cas_case_link_cache);
}
PGW_TEST(test_link_cache_posix);

static void test_link_cache_9p(void)
{
    with_9p_cache(cas_case_link_cache);
}
PGW_TEST(test_link_cache_9p);

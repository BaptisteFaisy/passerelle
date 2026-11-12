/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_cas_replace.c - ROB-04 over the two adapters of the fixture.
 *
 * The case bodies live in cas_cases_replace.c: they are written once and
 * driven here over the POSIX backend (the layer alone) and over the 9P
 * adapter (the view a real client has: share -> 9P -> client -> rmfs ->
 * truth). The FUSE adapter drives the very same bodies from tests/fuse_e2e.
 *
 * Covers ROB-04 (gap 19 of docs/gap-analysis.md), FS-11, FS-12, FS-13.
 */
#define _POSIX_C_SOURCE 200809L
#include "cas.h"
#include "cas_9p.h"

#include "cas_cases.h"

#include "pgw/rmfs.h"
#include "pgw/test.h"

/* The validity of the attribute cache of the cache cases: one second, so the
 * case can let the entry expire with the injected clock instead of sleeping. */
#define CACHE_TTL_NS 1000000000ULL

static void with_posix(void (*body)(const cas_view *))
{
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgw-replace", NULL, NULL));
    cas_view v;
    cas_view_of_mount(&v, &m);
    body(&v);
    cas_mount_close(&m);
}

static void with_9p(void (*body)(const cas_view *))
{
    cas_9p m;
    PGW_ASSERT(cas_9p_open(&m, "pgw-replace-9p", NULL));
    cas_view v;
    cas_view_of_9p(&v, &m);
    body(&v);
    cas_9p_close(&m);
}

/* The cache cases need their own mount: the cache is a mount option. */
static void with_posix_cache(void (*body)(const cas_view *))
{
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    opts.attr_ttl_ns = CACHE_TTL_NS;
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgw-replace-cache", &opts, NULL));
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
    PGW_ASSERT(cas_9p_open(&m, "pgw-replace-cache-9p", &opts));
    cas_view v;
    cas_view_of_9p(&v, &m);
    body(&v);
    cas_9p_close(&m);
}

static void test_replace_posix(void)
{
    with_posix(cas_case_replace);
}
PGW_TEST(test_replace_posix);

static void test_replace_9p(void)
{
    with_9p(cas_case_replace);
}
PGW_TEST(test_replace_9p);

static void test_truncate_posix(void)
{
    with_posix(cas_case_truncate);
}
PGW_TEST(test_truncate_posix);

static void test_truncate_9p(void)
{
    with_9p(cas_case_truncate);
}
PGW_TEST(test_truncate_9p);

static void test_unlink_posix(void)
{
    with_posix(cas_case_unlink);
}
PGW_TEST(test_unlink_posix);

static void test_unlink_9p(void)
{
    with_9p(cas_case_unlink);
}
PGW_TEST(test_unlink_9p);

static void test_cache_posix(void)
{
    with_posix_cache(cas_case_cache_stale);
}
PGW_TEST(test_cache_posix);

static void test_cache_9p(void)
{
    with_9p_cache(cas_case_cache_stale);
}
PGW_TEST(test_cache_9p);

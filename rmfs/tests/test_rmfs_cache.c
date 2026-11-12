/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/tests/test_rmfs_cache.c - The optional attribute cache in isolation.
 *
 * Covers requirement FS-13 (an optional attribute cache with a bounded,
 * adjustable validity) and decision D4 (no cache at all unless asked for,
 * never a data cache). The clock is injected through pgw_time_set_clock /
 * pgw_time_advance, so expiry is exercised without ever sleeping.
 */
#include "fake_backend.h"

#include "pgw/limits.h"
#include "pgw/log.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"
#include "test_cred.h"
#include "pgw/time.h"

#include <string.h>

static pgw_rmfs *make_fs(fake_backend *b, uint64_t attr_ttl_ns)
{
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    opts.attr_ttl_ns = attr_ttl_ns;
    return pgw_rmfs_create(b->ops, b, &opts);
}

static uint64_t g_now_ns = 0;

static uint64_t fake_clock(void *user)
{
    (void)user;
    return g_now_ns;
}

static void test_attr_cache_disabled_by_default(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b, 0);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "d.txt", "abc", 3), 0);

    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "d.txt", &a), PGW_OK);
    size_t first = b->attr_calls;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "d.txt", &a), PGW_OK);
    /* D4: off by default, so every stat reaches the server. */
    PGW_ASSERT_EQ_U(b->attr_calls, first + 1);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_attr_ttl_clamped_to_max(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    opts.attr_ttl_ns = PGW_ATTR_CACHE_NS_MAX * 4ull;
    pgw_rmfs *fs = pgw_rmfs_create(b->ops, b, &opts);
    PGW_ASSERT(fs != NULL);
    /* FS-13: validity is bounded, whatever the mount asked for. */
    PGW_ASSERT_EQ_U(pgw_rmfs_opts(fs)->attr_ttl_ns, PGW_ATTR_CACHE_NS_MAX);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_expiry_follows_the_injected_clock(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    g_now_ns = 5000;
    pgw_time_set_clock(fake_clock, &g_now_ns);
    pgw_rmfs *fs = make_fs(b, 100000000ull); /* 100 ms */
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "t.txt", "abc", 3), 0);

    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "t.txt", &a), PGW_OK);
    size_t base = b->attr_calls;

    /* Just under the TTL: still cached. */
    PGW_ASSERT(pgw_time_advance(99999999ull));
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "t.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_U(b->attr_calls, base);

    /* Exactly at the TTL the entry is stale (FS-13). */
    PGW_ASSERT(pgw_time_advance(1));
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "t.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_U(b->attr_calls, base + 1);

    pgw_time_set_clock(NULL, NULL);
    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_writes_invalidate_the_entry(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    g_now_ns = 0;
    pgw_time_set_clock(fake_clock, &g_now_ns);
    pgw_rmfs *fs = make_fs(b, 1000000000ull); /* 1 s */
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);

    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "w.txt",
                                  PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                    PGW_OK);
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_write(fs, fd, 0, "abc", 3, &n), PGW_OK);

    /* The size seen right after the write is the new one, not a stale
     * cached attribute: a write invalidates the entry (FS-12, FS-13). */
    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_fstat(fs, fd, &a), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, 3);

    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);
    pgw_time_set_clock(NULL, NULL);
    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* PERF-02: resolving a path must not ask the server for the same element
 * twice. A stat on a plain file costs exactly one lookup. */
static void test_stat_costs_one_round_trip(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b, 0); /* cache off: every answer is a real call */
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "one.txt", "abc", 3), 0);

    size_t lookups0 = b->lookup_calls;
    size_t getattrs0 = b->getattr_calls;
    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "one.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_U(b->lookup_calls - lookups0, 1);
    PGW_ASSERT_EQ_U(b->getattr_calls - getattrs0, 0);
    PGW_ASSERT_EQ_I(a.type, PGW_FT_REG);

    /* lstat of the same file is its own request, and still a single one. */
    lookups0 = b->lookup_calls;
    getattrs0 = b->getattr_calls;
    PGW_ASSERT_EQ_I(pgw_rmfs_lstat(fs, &cred, "one.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_U(b->lookup_calls - lookups0, 1);
    PGW_ASSERT_EQ_U(b->getattr_calls - getattrs0, 0);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

PGW_TEST(test_stat_costs_one_round_trip);
PGW_TEST(test_attr_cache_disabled_by_default);
PGW_TEST(test_attr_ttl_clamped_to_max);
PGW_TEST(test_expiry_follows_the_injected_clock);
PGW_TEST(test_writes_invalidate_the_entry);

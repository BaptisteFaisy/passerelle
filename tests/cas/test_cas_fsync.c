/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_cas_fsync.c - what fsync can and cannot prove (gap 69).
 *
 * Nobody proves platter durability from a test: that needs a power cut, and a
 * test that claims otherwise is lying. What *can* be proven here, and is:
 *
 *   - fsync is propagated to the backend, once per call, and is not swallowed
 *     by the attribute cache (a counting decorator around the truth backend
 *     shows every call that leaves the layer);
 *   - the bytes written before the fsync are readable afterwards by a fresh
 *     reader that bypasses every cache of the layer, with the page cache of
 *     the file dropped first (posix_fadvise DONTNEED) - the closest thing to a
 *     crash simulation a test may do without privileges;
 *   - the ordering holds: A written, fsync, B written, everything closed and
 *     re-opened on a fresh mount -> A is there and byte exact, and B is either
 *     absent or byte exact, never torn;
 *   - the answer of the layer is the answer of the kernel for the same
 *     descriptor (a read-only fd, an invalid argument).
 *
 * What is NOT proven is stated in the pull request: no F_FULLFSYNC on a real
 * disk, no write-cache flush, no power cut. Covers FS-10, FS-12, ROB-03.
 */
#define _POSIX_C_SOURCE 200809L
#include "cas.h"
#include "cas_9p.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK 4096u

/* Write one pattern block at `off` through the layer. */
static void write_block(pgw_rmfs *fs, pgw_rmfs_fd *fd, uint64_t off, uint32_t seed)
{
    uint8_t buf[BLOCK];
    pgw_test_pattern(buf, sizeof(buf), seed, off);
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_write(fs, fd, off, buf, sizeof(buf), &n), PGW_OK);
    PGW_ASSERT_EQ_U(n, sizeof(buf));
}

/* Read one block back through a *fresh* handle of a *fresh* mount, so nothing
 * the layer holds can answer: only the file system can. */
static void reread_fresh(const char *root, const char *name, uint64_t off, uint32_t seed,
                         bool expect_present)
{
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    opts.attr_ttl_ns = 10000000000ULL; /* the longest cache allowed */
    cas_mount fresh;
    PGW_ASSERT(cas_mount_open_at(&fresh, root, &opts, NULL));

    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fresh.fs, &fresh.cred, name, PGW_O_RDONLY, 0, &fd),
                    PGW_OK);
    uint8_t buf[BLOCK];
    memset(buf, 0xA5, sizeof(buf));
    size_t n = 0;
    pgw_err err = pgw_rmfs_read(fresh.fs, fd, off, buf, sizeof(buf), &n);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fresh.fs, fd), PGW_OK);
    if (!expect_present) {
        /* Not required to be there; if it is, it must not be torn. */
        if (err == PGW_OK && n == sizeof(buf))
            PGW_ASSERT(pgw_test_pattern_ok(buf, sizeof(buf), seed, off));
        cas_mount_close(&fresh);
        return;
    }
    PGW_ASSERT_EQ_I(err, PGW_OK);
    CAS_ASSERT_MSG(n == sizeof(buf), "a fresh reader got %zu bytes, not %u", n, BLOCK);
    CAS_ASSERT_MSG(pgw_test_pattern_ok(buf, sizeof(buf), seed, off),
                   "the block fsynced at offset %llu did not survive a "
                   "cache drop",
                   (unsigned long long)off);
    cas_mount_close(&fresh);
}

/* FS-10 (gap 69): every fsync of the layer reaches the backend, one for one. */
static void test_fsync_reaches_the_backend(void)
{
    cas_probe probe;
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-fsync", NULL, &probe));
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(
            pgw_rmfs_open(m.fs, &m.cred, "f.bin", PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
            PGW_OK);
    write_block(m.fs, fd, 0, 1);
    PGW_ASSERT_EQ_U(probe.fsync_calls, 0);
    PGW_ASSERT_EQ_I(pgw_rmfs_fsync(m.fs, fd), PGW_OK);
    CAS_ASSERT_MSG(probe.fsync_calls == 1, "one fsync reached the backend %zu times",
                   probe.fsync_calls);
    PGW_ASSERT_EQ_I(pgw_rmfs_fsync(m.fs, fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_fsync(m.fs, fd), PGW_OK);
    CAS_ASSERT_MSG(probe.fsync_calls == 3, "three fsyncs reached the backend %zu times",
                   probe.fsync_calls);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, fd), PGW_OK);
    cas_mount_close(&m);
}
PGW_TEST(test_fsync_reaches_the_backend);

/* FS-10, FS-13 (gap 69): the attribute cache never absorbs an fsync, and a
 * stat served from the cache does not stand in for one. */
static void test_fsync_not_swallowed_by_attr_cache(void)
{
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    opts.attr_ttl_ns = 10000000000ULL; /* the longest cache allowed */

    cas_probe probe;
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-fsynccache", &opts, &probe));
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(
            pgw_rmfs_open(m.fs, &m.cred, "c.bin", PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
            PGW_OK);
    write_block(m.fs, fd, 0, 2);

    /* Warm the cache, then fsync: the cached attributes must not shortcut it. */
    pgw_fs_attr at;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(m.fs, &m.cred, "c.bin", &at), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(m.fs, &m.cred, "c.bin", &at), PGW_OK);
    const size_t attrs_before = probe.getattr_calls + probe.lookup_calls;
    PGW_ASSERT_EQ_I(pgw_rmfs_fsync(m.fs, fd), PGW_OK);
    CAS_ASSERT_MSG(probe.fsync_calls == 1,
                   "the attribute cache swallowed the fsync (%zu reached the "
                   "backend)",
                   probe.fsync_calls);
    /* The second stat is served from the cache: that is FS-13, and it must not
     * have been counted as a sync. */
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(m.fs, &m.cred, "c.bin", &at), PGW_OK);
    PGW_ASSERT_EQ_U(probe.getattr_calls + probe.lookup_calls, attrs_before);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, fd), PGW_OK);
    cas_mount_close(&m);
}
PGW_TEST(test_fsync_not_swallowed_by_attr_cache);

/* FS-10 (gap 69): what was written before the fsync is readable afterwards by
 * a fresh reader, with the page cache of the file dropped first. */
static void test_fsync_survives_a_cache_drop(void)
{
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-fsyncdrop", NULL, NULL));
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(
            pgw_rmfs_open(m.fs, &m.cred, "d.bin", PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
            PGW_OK);
    write_block(m.fs, fd, 0, 3);
    write_block(m.fs, fd, BLOCK, 4);
    PGW_ASSERT_EQ_I(pgw_rmfs_fsync(m.fs, fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, fd), PGW_OK);

    char abs[CAS_PATH_MAX];
    cas_mount_abs(&m, "d.bin", abs, sizeof(abs));
    PGW_ASSERT(cas_drop_cache(abs)); /* the crash simulation a test may do */

    reread_fresh(m.root, "d.bin", 0, 3, true);
    reread_fresh(m.root, "d.bin", BLOCK, 4, true);
    cas_mount_close(&m);
}
PGW_TEST(test_fsync_survives_a_cache_drop);

/* FS-10 (gap 69): the order is kept - A, fsync, B, everything closed and the
 * caches dropped: A is on the disk and byte exact, B is not required to be
 * there but must never be torn. */
static void test_fsync_keeps_the_order_of_writes(void)
{
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-fsyncorder", NULL, NULL));
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(
            pgw_rmfs_open(m.fs, &m.cred, "o.bin", PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
            PGW_OK);
    write_block(m.fs, fd, 0, 5); /* A */
    PGW_ASSERT_EQ_I(pgw_rmfs_fsync(m.fs, fd), PGW_OK);
    write_block(m.fs, fd, BLOCK, 6); /* B, never fsynced */
    PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, fd), PGW_OK);
    cas_mount_close(&m); /* the mount dies here, as a driver would */

    char abs[CAS_PATH_MAX];
    cas_path_join(abs, sizeof(abs), m.root, "o.bin");
    PGW_ASSERT(cas_drop_cache(abs));

    reread_fresh(m.root, "o.bin", 0, 5, true);      /* A is required */
    reread_fresh(m.root, "o.bin", BLOCK, 6, false); /* B is not */
}
PGW_TEST(test_fsync_keeps_the_order_of_writes);

/* FS-10 (gap 69): the layer answers what the kernel answers for the same
 * descriptor: a read-only fd may be fsynced, an invalid one may not. */
static void test_fsync_answers_like_the_kernel(void)
{
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-fsyncro", NULL, NULL));
    pgw_rmfs_fd *wr = NULL;
    PGW_ASSERT_EQ_I(
            pgw_rmfs_open(m.fs, &m.cred, "r.bin", PGW_O_RDWR | PGW_O_CREAT, 0644, &wr),
            PGW_OK);
    write_block(m.fs, wr, 0, 7);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, wr), PGW_OK);

    /* The truth: fsync on a read-only descriptor. */
    char abs[CAS_PATH_MAX];
    cas_mount_abs(&m, "r.bin", abs, sizeof(abs));
    int raw = open(abs, O_RDONLY);
    PGW_ASSERT(raw >= 0);
    errno = 0;
    const int truth = (fsync(raw) == 0) ? 0 : errno;
    close(raw);

    pgw_rmfs_fd *ro = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(m.fs, &m.cred, "r.bin", PGW_O_RDONLY, 0, &ro), PGW_OK);
    const pgw_err got = pgw_rmfs_fsync(m.fs, ro);
    CAS_ASSERT_MSG(got == pgw_code_from_host(truth),
                   "fsync of a read-only descriptor: the kernel answered errno "
                   "%d, the layer answered %s",
                   truth, pgw_err_name(got));
    /* An invalid argument is refused before anything is sent. */
    PGW_ASSERT_EQ_I(pgw_rmfs_fsync(m.fs, NULL), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_rmfs_fsync(NULL, ro), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, ro), PGW_OK);
    cas_mount_close(&m);
}
PGW_TEST(test_fsync_answers_like_the_kernel);

/* FS-10, P9-02 (gap 69): the fsync of a client crosses the wire: the data is
 * readable after the caches are dropped, and the server really synced (it
 * answers with the errno of its own os.fsync). */
static void test_fsync_through_9p(void)
{
    cas_9p c;
    PGW_ASSERT(cas_9p_open(&c, "pgwcas-fsync9p", NULL));
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(
            pgw_rmfs_open(c.fs, &c.cred, "w.bin", PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
            PGW_OK);
    write_block(c.fs, fd, 0, 8);
    PGW_ASSERT_EQ_I(pgw_rmfs_fsync(c.fs, fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(c.fs, fd), PGW_OK);
    cas_9p_close(&c); /* the session and the server both stop here */

    char abs[CAS_PATH_MAX];
    cas_path_join(abs, sizeof(abs), c.root, "w.bin");
    PGW_ASSERT(cas_drop_cache(abs));

    /* Read the truth directly: the server is gone, only the disk answers. */
    uint8_t buf[BLOCK];
    size_t len = 0;
    PGW_ASSERT(cas_truth_read(c.root, "w.bin", buf, sizeof(buf), &len));
    CAS_ASSERT_MSG(len == sizeof(buf), "the file holds %zu bytes, not %u", len, BLOCK);
    PGW_ASSERT(pgw_test_pattern_ok(buf, sizeof(buf), 8, 0));
}
PGW_TEST(test_fsync_through_9p);

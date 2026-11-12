/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_cas_big_file.c - the 5 GiB directed case of spec 7.4 (gap 64).
 *
 * The specification asks for a 5 GiB file. No test may materialise 5 GiB of
 * data: the case builds a *sparse* file instead (truncate to the size, then
 * write three 4 KiB blocks at known offsets) and verifies the behaviour that
 * actually matters at that scale:
 *
 *   - a hole reads back as zeros, including across a block boundary (FS-04);
 *   - a write past the end grows the file and leaves a zero gap (FS-04);
 *   - the size reported through the attribute cache is the real one, and the
 *     cache never serves data (FS-12, FS-13, D4);
 *   - the size the *client* sees through the 9P adapter is the real one;
 *   - the file stays sparse: the blocks on the disk are reported and bounded
 *     (PERF-04).
 *
 * The scale comes from PGW_CAS_BIG_BYTES: 64 MiB in a pull request, 5 GiB in
 * the DISABLED long campaign (CI budget, 5 minutes per job).
 */
#define _POSIX_C_SOURCE 200809L
#include "cas.h"
#include "cas_9p.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"
#include "pgw/time.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CAS_BIG_BYTES_ENV "PGW_CAS_BIG_BYTES"
#define CAS_BIG_DEFAULT (64ull * 1024ull * 1024ull) /* 64 MiB in a pull request */
#define CAS_BIG_SPEC (5ull * 1024ull * 1024ull * 1024ull) /* the 5 GiB of 7.4 */
#define BLK 4096u

static uint64_t big_bytes(void)
{
    return cas_env_u64(CAS_BIG_BYTES_ENV, CAS_BIG_DEFAULT);
}

/* Write (resp. read back) one block of the deterministic pattern of the
 * harness at `off`, so a case proves the content survived without a copy. */
static void big_write_block(pgw_rmfs *fs, pgw_rmfs_fd *fd, uint64_t off,
                            uint32_t seed)
{
    uint8_t buf[BLK];
    pgw_test_pattern(buf, sizeof(buf), seed, off);
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_write(fs, fd, off, buf, sizeof(buf), &n), PGW_OK);
    PGW_ASSERT_EQ_U(n, sizeof(buf));
}

static void big_expect_block(pgw_rmfs *fs, pgw_rmfs_fd *fd, uint64_t off,
                             uint32_t seed)
{
    uint8_t buf[BLK];
    memset(buf, 0xAA, sizeof(buf));
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_read(fs, fd, off, buf, sizeof(buf), &n), PGW_OK);
    PGW_ASSERT_EQ_U(n, sizeof(buf));
    CAS_ASSERT_MSG(pgw_test_pattern_ok(buf, sizeof(buf), seed, off),
                   "block at offset %llu does not match the pattern it was "
                   "written with",
                   (unsigned long long)off);
}

/* A hole must read back as zeros: this is the whole point of a sparse file. */
static void big_expect_hole(pgw_rmfs *fs, pgw_rmfs_fd *fd, uint64_t off,
                            size_t len)
{
    uint8_t buf[BLK];
    if (len > sizeof(buf))
        len = sizeof(buf);
    memset(buf, 0xAA, sizeof(buf));
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_read(fs, fd, off, buf, len, &n), PGW_OK);
    PGW_ASSERT_EQ_U(n, len);
    for (size_t i = 0; i < len; i++)
        CAS_ASSERT_MSG(buf[i] == 0, "hole at offset %llu reads %u, not zero "
                       "(byte %zu of the read)",
                       (unsigned long long)off, (unsigned)buf[i], i);
}

/* Open (creating it) a file of `n` logical bytes with three written blocks. */
static pgw_rmfs_fd *big_make(cas_mount *m, const char *name, uint64_t n)
{
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(m->fs, &m->cred, name,
                                  PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                    PGW_OK);
    /* truncate() extends without allocating: the file is sparse from here. */
    PGW_ASSERT_EQ_I(pgw_rmfs_truncate(m->fs, &m->cred, name, n), PGW_OK);
    big_write_block(m->fs, fd, 0, 1);
    big_write_block(m->fs, fd, n / 2, 2);
    big_write_block(m->fs, fd, n - BLK, 3);
    return fd;
}

/* FS-04, PERF-04 (gap 64): the hole, the boundary and the end of the file. */
static void test_big_file_hole_and_boundary(void)
{
    const uint64_t n = big_bytes();
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-bighole", NULL, NULL));
    pgw_rmfs_fd *fd = big_make(&m, "big.bin", n);

    big_expect_block(m.fs, fd, 0, 1);
    big_expect_hole(m.fs, fd, BLK, BLK); /* just after the first block */
    big_expect_block(m.fs, fd, n / 2, 2);
    big_expect_hole(m.fs, fd, n / 2 + BLK, BLK);
    big_expect_block(m.fs, fd, n - BLK, 3);

    /* The boundary: the last bytes of a block and the first bytes of the hole
     * come back in one read, unaligned. */
    uint8_t got[32];
    uint8_t want[32];
    pgw_test_pattern(want, 16, 1, BLK - 16);
    memset(want + 16, 0, 16);
    size_t nr = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_read(m.fs, fd, BLK - 16, got, sizeof(got), &nr),
                    PGW_OK);
    PGW_ASSERT_EQ_U(nr, sizeof(got));
    PGW_ASSERT_MEM_EQ(got, want, sizeof(got));

    /* A read that starts inside the file and runs past the end is short, not
     * an error, and a read at the end yields nothing. */
    uint8_t tail[64];
    PGW_ASSERT_EQ_I(pgw_rmfs_read(m.fs, fd, n - 8, tail, sizeof(tail), &nr),
                    PGW_OK);
    PGW_ASSERT_EQ_U(nr, 8);
    PGW_ASSERT_EQ_I(pgw_rmfs_read(m.fs, fd, n, tail, sizeof(tail), &nr),
                    PGW_OK);
    PGW_ASSERT_EQ_U(nr, 0);

    PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, fd), PGW_OK);
    cas_mount_close(&m);
}
PGW_TEST(test_big_file_hole_and_boundary);

/* FS-04 (gap 64): a write past the end grows the file and leaves a zero gap. */
static void test_big_file_write_past_end_grows(void)
{
    const uint64_t n = big_bytes();
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-biggrow", NULL, NULL));
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(m.fs, &m.cred, "grow.bin",
                                  PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                    PGW_OK);
    size_t w = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_write(m.fs, fd, 0, "hello", 5, &w), PGW_OK);
    PGW_ASSERT_EQ_U(w, 5);

    /* Jump far past the end: the size becomes off + len, the gap is a hole. */
    const uint64_t far = n + BLK;
    big_write_block(m.fs, fd, far, 7);

    pgw_fs_attr at;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(m.fs, &m.cred, "grow.bin", &at), PGW_OK);
    PGW_ASSERT_EQ_U(at.size, far + BLK);
    PGW_ASSERT_EQ_U(cas_truth_size(m.root, "grow.bin"), far + BLK);

    big_expect_block(m.fs, fd, far, 7);
    big_expect_hole(m.fs, fd, n, BLK); /* the gap the jump left behind */
    uint8_t head[5];
    PGW_ASSERT_EQ_I(pgw_rmfs_read(m.fs, fd, 0, head, sizeof(head), &w),
                    PGW_OK);
    PGW_ASSERT_EQ_U(w, 5);
    PGW_ASSERT_MEM_EQ(head, "hello", 5);

    PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, fd), PGW_OK);
    cas_mount_close(&m);
}
PGW_TEST(test_big_file_write_past_end_grows);

/* FS-12, FS-13, D4 (gap 64): with the attribute cache on, the size reported
 * after a growth is the real one, and the cache never serves *data*: a file
 * modified behind the layer is read back modified. */
static void test_big_file_attr_cache_size_and_no_data_cache(void)
{
    const uint64_t n = big_bytes();
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    opts.attr_ttl_ns = 1000000000ULL; /* 1 s of attribute cache */

    uint64_t clock_ns = 1000000000ULL;
    pgw_time_set_clock(cas_fake_clock, &clock_ns);
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-bigcache", &opts, NULL));
    pgw_rmfs_fd *fd = big_make(&m, "cached.bin", n);

    pgw_fs_attr at;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(m.fs, &m.cred, "cached.bin", &at), PGW_OK);
    PGW_ASSERT_EQ_U(at.size, n);

    /* Shrink the file behind the layer, as the PC side would. Inside the TTL
     * the cached size still answers (that is FS-13); the data does not. */
    char abs[512];
    cas_mount_abs(&m, "cached.bin", abs, sizeof(abs));
    PGW_ASSERT_EQ_I(truncate(abs, (off_t)(n / 2)), 0);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(m.fs, &m.cred, "cached.bin", &at), PGW_OK);
    PGW_ASSERT_EQ_U(at.size, n); /* bounded staleness, not a bug: FS-13 */

    uint8_t buf[BLK];
    size_t nr = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_read(m.fs, fd, 0, buf, sizeof(buf), &nr),
                    PGW_OK);
    PGW_ASSERT(pgw_test_pattern_ok(buf, sizeof(buf), 1, 0));

    /* Past the TTL the size is refreshed from the truth. */
    PGW_ASSERT(pgw_time_advance(opts.attr_ttl_ns + 1));
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(m.fs, &m.cred, "cached.bin", &at), PGW_OK);
    PGW_ASSERT_EQ_U(at.size, n / 2);
    pgw_time_set_clock(NULL, NULL);

    PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, fd), PGW_OK);
    cas_mount_close(&m);
}
PGW_TEST(test_big_file_attr_cache_size_and_no_data_cache);

/* PERF-04 (gap 64): the file never touches the disk for the hole, and the
 * case reports the real block usage so a regression cannot hide. */
static void test_big_file_stays_sparse(void)
{
    const uint64_t n = big_bytes();
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-bigsparse", NULL, NULL));
    pgw_rmfs_fd *fd = big_make(&m, "sparse.bin", n);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, fd), PGW_OK);

    const uint64_t real = cas_truth_blocks(m.root, "sparse.bin");
    printf("# big file: %llu logical bytes, %llu bytes really on the disk\n",
           (unsigned long long)n, (unsigned long long)real);
    PGW_ASSERT_EQ_U(cas_truth_size(m.root, "sparse.bin"), n);
    /* Three 4 KiB blocks plus the directory metadata of the file system: far
     * below the logical size, and never more than a few megabytes. */
    CAS_ASSERT_MSG(real <= 4ull * 1024ull * 1024ull,
                   "the case is no longer sparse: %llu bytes really allocated "
                   "for a %llu byte file",
                   (unsigned long long)real, (unsigned long long)n);
    CAS_ASSERT_MSG(real * 16 <= n,
                   "the hole is not a hole any more: %llu bytes allocated",
                   (unsigned long long)real);
    cas_mount_close(&m);
}
PGW_TEST(test_big_file_stays_sparse);

/* PERF-04 (gap 64): the size the *client* sees, through the 9P adapter, is
 * the size the served directory really holds. */
static void test_big_file_client_sees_size(void)
{
    const uint64_t n = big_bytes();
    cas_9p c;
    PGW_ASSERT(cas_9p_open(&c, "pgwcas-big9p", NULL));

    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(c.fs, &c.cred, "big.bin",
                                  PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_truncate(c.fs, &c.cred, "big.bin", n), PGW_OK);
    big_write_block(c.fs, fd, 0, 11);
    big_write_block(c.fs, fd, n - BLK, 12);

    pgw_fs_attr at;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(c.fs, &c.cred, "big.bin", &at), PGW_OK);
    PGW_ASSERT_EQ_U(at.size, n);
    PGW_ASSERT_EQ_I(pgw_rmfs_fstat(c.fs, fd, &at), PGW_OK);
    PGW_ASSERT_EQ_U(at.size, n);
    PGW_ASSERT_EQ_U(cas_truth_size(c.root, "big.bin"), n);

    big_expect_block(c.fs, fd, 0, 11);
    big_expect_block(c.fs, fd, n - BLK, 12);
    big_expect_hole(c.fs, fd, BLK, BLK);

    PGW_ASSERT_EQ_I(pgw_rmfs_close(c.fs, fd), PGW_OK);
    cas_9p_close(&c);
}
PGW_TEST(test_big_file_client_sees_size);

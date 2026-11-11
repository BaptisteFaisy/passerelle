/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/tests/test_rmfs_locks.c - Guest-local byte-range locks.
 *
 * Covers requirement FS-14: locks work between processes of the guest, they
 * are *not* relayed to the PC (the backend sees no lock call at all), shared
 * read locks coexist, write locks conflict, ranges can be split and released,
 * and every lock of a dead process is dropped (ROB-02).
 */
#include "../src/rmfs_internal.h"

#include "fake_backend.h"

#include "pgw/rmfs.h"
#include "pgw/test.h"

#include <string.h>

static pgw_rmfs *make_fs(fake_backend *b)
{
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    return pgw_rmfs_create(b->ops, b, &opts);
}

static void test_read_locks_are_shared(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "l.txt", "0123456789", 10), 0);

    /* Two processes take a read lock on the same range: no conflict. */
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "l.txt", 100, PGW_LOCK_RD, 0, 4),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "l.txt", 200, PGW_LOCK_RD, 0, 4),
                    PGW_OK);

    /* FS-14: none of this reached the PC. */
    PGW_ASSERT_EQ_U(b->open_calls, 0);
    PGW_ASSERT_EQ_U(b->write_calls, 0);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_write_lock_conflicts(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "w.txt", "0123456789", 10), 0);

    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "w.txt", 100, PGW_LOCK_WR, 10, 5),
                    PGW_OK);
    /* A second writer overlapping the range is refused. */
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "w.txt", 200, PGW_LOCK_WR, 12, 5),
                    PGW_EACCES);
    /* A reader overlapping a writer is refused too. */
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "w.txt", 200, PGW_LOCK_RD, 0, 100),
                    PGW_EACCES);
    /* Outside the range, everything is free. */
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "w.txt", 200, PGW_LOCK_WR, 0, 10),
                    PGW_OK);
    /* Another file is independent. */
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "other.txt", "x", 1), 0);
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "other.txt", 200, PGW_LOCK_WR, 10, 5),
                    PGW_OK);

    /* The same owner may re-lock its own range. */
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "w.txt", 100, PGW_LOCK_WR, 10, 5),
                    PGW_OK);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_unlock_frees_the_range(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "u.txt", "0123456789", 10), 0);

    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "u.txt", 100, PGW_LOCK_WR, 0, 10),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "u.txt", 200, PGW_LOCK_WR, 0, 10),
                    PGW_EACCES);

    /* Unlock the whole range: the other process can now take it. */
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "u.txt", 100, PGW_LOCK_UN, 0, 10),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "u.txt", 200, PGW_LOCK_WR, 0, 10),
                    PGW_OK);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_partial_unlock_splits_range(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "s.txt", "0123456789", 10), 0);

    /* Hold [0, 100), then release the middle [40, 60). */
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "s.txt", 100, PGW_LOCK_WR, 0, 100),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "s.txt", 100, PGW_LOCK_UN, 40, 20),
                    PGW_OK);

    /* The hole is free ... */
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "s.txt", 200, PGW_LOCK_WR, 40, 20),
                    PGW_OK);
    /* ... but both tails are still held. */
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "s.txt", 300, PGW_LOCK_WR, 0, 10),
                    PGW_EACCES);
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "s.txt", 300, PGW_LOCK_WR, 90, 10),
                    PGW_EACCES);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_getlock_reports_holder(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "g.txt", "0123456789", 10), 0);

    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "g.txt", 111, PGW_LOCK_WR, 10, 20),
                    PGW_OK);

    pgw_lock_type type = PGW_LOCK_RD;
    uint64_t start = 0;
    uint64_t len = 0;
    uint32_t holder = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_getlock(fs, "g.txt", 222, &type, &start, &len,
                                     &holder),
                    PGW_OK);
    PGW_ASSERT_EQ_I(type, PGW_LOCK_WR);
    PGW_ASSERT_EQ_U(holder, 111);
    PGW_ASSERT_EQ_U(start, 10);
    PGW_ASSERT_EQ_U(len, 20);

    /* No conflict: F_UNLCK and no holder. */
    type = PGW_LOCK_WR;
    start = 500;
    len = 10;
    holder = 1234;
    PGW_ASSERT_EQ_I(pgw_rmfs_getlock(fs, "g.txt", 222, &type, &start, &len,
                                     &holder),
                    PGW_OK);
    PGW_ASSERT_EQ_I(type, PGW_LOCK_UN);
    PGW_ASSERT_EQ_U(holder, 0);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_dead_process_locks_are_released(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "d.txt", "0123456789", 10), 0);

    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "d.txt", 777, PGW_LOCK_WR, 0, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "d.txt", 888, PGW_LOCK_WR, 0, 0),
                    PGW_EACCES);

    /* The holder dies: all its locks vanish (ROB-02). */
    pgw_rmfs_unlock_pid(fs, 777);
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "d.txt", 888, PGW_LOCK_WR, 0, 0),
                    PGW_OK);

    /* Releasing an unknown pid is harmless. */
    pgw_rmfs_unlock_pid(fs, 123456);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_lock_to_eof(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "e.txt", "x", 1), 0);

    /* len == 0 means "to the end of file", so a huge offset still conflicts. */
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "e.txt", 100, PGW_LOCK_WR, 0, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "e.txt", 200, PGW_LOCK_RD,
                                  (uint64_t)1 << 40, 10),
                    PGW_EACCES);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_invalid_arguments(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);

    PGW_ASSERT_EQ_I(pgw_rmfs_lock(NULL, "x", 1, PGW_LOCK_RD, 0, 1),
                    PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, NULL, 1, PGW_LOCK_RD, 0, 1),
                    PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "x", 1, (pgw_lock_type)42, 0, 1),
                    PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_rmfs_getlock(fs, "x", 1, NULL, NULL, NULL, NULL),
                    PGW_EINVAL);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

PGW_TEST(test_read_locks_are_shared);
PGW_TEST(test_write_lock_conflicts);
PGW_TEST(test_unlock_frees_the_range);
PGW_TEST(test_partial_unlock_splits_range);
PGW_TEST(test_getlock_reports_holder);
PGW_TEST(test_dead_process_locks_are_released);
PGW_TEST(test_lock_to_eof);
PGW_TEST(test_invalid_arguments);

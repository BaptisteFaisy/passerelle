/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/tests/test_rmfs_fd.c - Lifetime of the open descriptors.
 *
 * Covers requirement FS-11 (a deleted file stays readable through its open
 * descriptor), the reference counting of the open table, and ROB-02 (the death
 * of a client closes every descriptor it held, so no fid leaks on the
 * server).
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

/* Refcounting: the backend handle is released only by the last close. */
static void test_refcount_releases_on_last_close(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = {1000, 1000, 20};
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "rc.txt", "data", 4), 0);

    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "rc.txt", PGW_O_RDONLY, 0, &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_U(pgw_rmfs_fd_refcount(fd), 1);
    PGW_ASSERT_EQ_I(pgw_rmfs_fd_ref(fd), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_rmfs_fd_refcount(fd), 2);

    size_t closes0 = b->close_calls;
    /* First close drops one reference: the handle stays open. */
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);
    PGW_ASSERT_EQ_U(b->close_calls, closes0);
    PGW_ASSERT_EQ_U(pgw_rmfs_open_count(fs), 1);
    PGW_ASSERT_EQ_U(pgw_rmfs_fd_refcount(fd), 1);

    /* The file is still readable through the surviving reference. */
    char buf[8] = {0};
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_read(fs, fd, 0, buf, sizeof(buf), &got), PGW_OK);
    PGW_ASSERT_EQ_U(got, 4);

    /* Last close releases the backend handle. */
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);
    PGW_ASSERT_EQ_U(b->close_calls, closes0 + 1);
    PGW_ASSERT_EQ_U(pgw_rmfs_open_count(fs), 0);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* --- ROB-02: a dead client leaves nothing behind ------------------------- */

static void test_release_pid_closes_everything(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = {1000, 1000, 20};
    const uint32_t dead_pid = 4242;

    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "p1.txt", "aa", 2), 0);
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "p2.txt", "bb", 2), 0);
    pgw_rmfs_fd *f1 = NULL;
    pgw_rmfs_fd *f2 = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "p1.txt", PGW_O_RDONLY, 0, &f1),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "p2.txt", PGW_O_RDONLY, 0, &f2),
                    PGW_OK);
    pgw_rmfs_fd_set_owner(f1, dead_pid);
    pgw_rmfs_fd_set_owner(f2, dead_pid);
    PGW_ASSERT_EQ_U(pgw_rmfs_open_count(fs), 2);

    /* A lock held by that process goes away with it too (FS-14, ROB-02). */
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "p1.txt", dead_pid, PGW_LOCK_WR, 0, 0),
                    PGW_OK);

    PGW_ASSERT_EQ_I(pgw_rmfs_release_pid(fs, dead_pid), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_rmfs_open_count(fs), 0);
    /* The backend saw the matching closes: no fid leaked on the server. */
    PGW_ASSERT_EQ_U(b->close_calls, 2);

    /* The lock is gone: another process can take it. */
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(fs, "p1.txt", 999, PGW_LOCK_WR, 0, 0),
                    PGW_OK);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}


PGW_TEST(test_refcount_releases_on_last_close);
PGW_TEST(test_release_pid_closes_everything);

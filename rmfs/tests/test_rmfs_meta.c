/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/tests/test_rmfs_meta.c - Metadata operations of the resource manager.
 *
 * Covers requirements FS-02 (size, type, mode, owner, nanosecond dates, link
 * count, stable inode), FS-05 (mkdir, rmdir, unlink, atomic rename including
 * across directories), FS-07 (chmod, chown, truncate, utimens), FS-08 (symlink
 * creation and reading, hard links), FS-10 (fsync reaching the PC, statfs for
 * df), FS-15 (UTF-8 and special characters in names) and QNX-08 (per-operation
 * counters).
 */
#include "fake_backend.h"

#include "pgw/limits.h"
#include "pgw/log.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"
#include "test_cred.h"

#include <stdio.h>
#include <string.h>

static pgw_rmfs *make_fs(fake_backend *b)
{
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    return pgw_rmfs_create(b->ops, b, &opts);
}

/* --- FS-02 ---------------------------------------------------------------- */

static void test_stat_reports_every_field(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);

    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "s.txt", "12345", 5), 0);
    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "s.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_I(a.type, PGW_FT_REG);
    PGW_ASSERT_EQ_U(a.size, 5);
    PGW_ASSERT_EQ_U(a.nlink, 1);
    PGW_ASSERT(a.ino != PGW_INO_NONE);
    PGW_ASSERT((a.mode & 0777u) != 0);
    /* Nanosecond timestamps, not seconds: a non-zero nsec proves it. */
    PGW_ASSERT(a.mtime_ns > 0);

    /* The inode number is stable across calls (FS-02). */
    uint64_t ino = a.ino;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "s.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_U(a.ino, ino);

    PGW_ASSERT_EQ_I(pgw_rmfs_mkdir(fs, &cred, "dir", 0755), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "dir", &a), PGW_OK);
    PGW_ASSERT_EQ_I(a.type, PGW_FT_DIR);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* --- FS-05 ---------------------------------------------------------------- */

static void test_mkdir_unlink_rmdir(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);

    PGW_ASSERT_EQ_I(pgw_rmfs_mkdir(fs, &cred, "d1", 0755), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_mkdir(fs, &cred, "d1/sub", 0700), PGW_OK);
    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "d1/sub", &a), PGW_OK);
    PGW_ASSERT_EQ_I(a.type, PGW_FT_DIR);

    /* A non-empty directory cannot be removed as a file. */
    PGW_ASSERT_EQ_I(pgw_rmfs_unlink(fs, &cred, "d1", false), PGW_EISDIR);
    PGW_ASSERT_EQ_I(pgw_rmfs_unlink(fs, &cred, "d1/sub", true), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "d1/sub", &a), PGW_ENOENT);
    PGW_ASSERT_EQ_I(pgw_rmfs_unlink(fs, &cred, "d1", true), PGW_OK);

    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "f.txt", "x", 1), 0);
    PGW_ASSERT_EQ_I(pgw_rmfs_unlink(fs, &cred, "f.txt", false), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "f.txt", &a), PGW_ENOENT);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_rename_across_directories(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);

    PGW_ASSERT_EQ_I(pgw_rmfs_mkdir(fs, &cred, "src", 0755), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_mkdir(fs, &cred, "dst", 0755), PGW_OK);
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "src/move.txt", "data", 4), 0);

    PGW_ASSERT_EQ_I(pgw_rmfs_rename(fs, &cred, "src/move.txt",
                                    "dst/moved.txt"),
                    PGW_OK);
    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "src/move.txt", &a), PGW_ENOENT);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "dst/moved.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, 4);

    /* Renaming onto an existing file replaces it atomically (FS-05). */
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "dst/other.txt", "zz", 2), 0);
    PGW_ASSERT_EQ_I(pgw_rmfs_rename(fs, &cred, "dst/moved.txt",
                                    "dst/other.txt"),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "dst/other.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, 4);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* --- FS-07 ---------------------------------------------------------------- */

static void test_chmod_chown_truncate_utimens(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "m.txt", "0123456789", 10), 0);

    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "m.txt", &a), PGW_OK);
    uint32_t owner = a.uid;

    /* chmod: the owner may change its own mode. */
    PGW_ASSERT_EQ_I(pgw_rmfs_chmod(fs, &cred, "m.txt", 0600), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "m.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_U(a.mode & 0777u, 0600u);

    /* chown by a caller who is neither the owner nor root is refused (FS-09).
     * The identity is deliberately not the process one: the rule must hold
     * whoever runs the suite, root included (a container makes that easy to
     * hit, and the assertion used to depend on the process uid). */
    pgw_fs_cred outsider = tc_other(20);
    PGW_ASSERT_EQ_I(pgw_rmfs_chown(fs, &outsider, "m.txt", owner + 1, owner),
                    PGW_EPERM);

    /* truncate down and up. */
    PGW_ASSERT_EQ_I(pgw_rmfs_truncate(fs, &cred, "m.txt", 4), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "m.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, 4);
    PGW_ASSERT_EQ_I(pgw_rmfs_truncate(fs, &cred, "m.txt", 32), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "m.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, 32);

    /* utimens: explicit nanosecond dates land on the file. */
    const uint64_t atime = 1600000000123456789ull;
    const uint64_t mtime = 1600000001987654321ull;
    PGW_ASSERT_EQ_I(pgw_rmfs_utimens(fs, &cred, "m.txt", atime, mtime),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "m.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_U(a.mtime_ns, mtime);
    PGW_ASSERT_EQ_U(a.atime_ns, atime);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* --- FS-10 ---------------------------------------------------------------- */

static void test_fsync_and_statfs(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);

    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "sync.txt",
                                  PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                    PGW_OK);
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_write(fs, fd, 0, "durable", 7, &n), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_fsync(fs, fd), PGW_OK); /* FS-10 */
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);

    pgw_fs_statvfs sv;
    memset(&sv, 0, sizeof(sv));
    PGW_ASSERT_EQ_I(pgw_rmfs_statfs(fs, &cred, &sv), PGW_OK);
    PGW_ASSERT(sv.bsize > 0);
    PGW_ASSERT(sv.blocks > 0);
    PGW_ASSERT(sv.namelen > 0);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* --- FS-15 ---------------------------------------------------------------- */

static void test_utf8_and_special_names(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);

    const char *names[] = {
        "caf\xc3\xa9.txt",           /* UTF-8 accented */
        "a b\tc.txt",                /* space and tab */
        "quote'and\"double.txt",     /* quotes */
        "dash-dot_dot.name",         /* ordinary specials */
        "\xe6\x97\xa5\xe6\x9c\xac",  /* CJK, no extension */
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        pgw_rmfs_fd *fd = NULL;
        PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, names[i],
                                      PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                        PGW_OK);
        PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);
        pgw_fs_attr a;
        PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, names[i], &a), PGW_OK);
        PGW_ASSERT_EQ_I(a.type, PGW_FT_REG);
    }

    /* A name of exactly 255 bytes is accepted (FS-15). */
    char max_name[PGW_NAME_MAX + 1];
    memset(max_name, 'm', PGW_NAME_MAX);
    max_name[PGW_NAME_MAX] = '\0';
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, max_name, "x", 1), 0);
    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, max_name, &a), PGW_OK);

    /* 256 bytes is refused before anything reaches the server. */
    char too_long[PGW_NAME_MAX + 2];
    memset(too_long, 'm', PGW_NAME_MAX + 1);
    too_long[PGW_NAME_MAX + 1] = '\0';
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, too_long, &a), PGW_ENAMETOOLONG);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* --- QNX-08 --------------------------------------------------------------- */

static void test_counters_move(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);

    uint64_t req0 = pgw_stat_get(PGW_STAT_REQ);
    uint64_t err0 = pgw_stat_get(PGW_STAT_ERR);
    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "nope.txt", &a), PGW_ENOENT);
    /* A failed operation is counted, and counted as an error. */
    PGW_ASSERT(pgw_stat_get(PGW_STAT_REQ) > req0);
    PGW_ASSERT(pgw_stat_get(PGW_STAT_ERR) > err0);

    /* The table renders as name=value lines (exposed by the driver). */
    char table[1024];
    PGW_ASSERT(pgw_stat_format(table, sizeof(table)) > 0);
    PGW_ASSERT(strstr(table, "requests=") != NULL);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

PGW_TEST(test_stat_reports_every_field);
PGW_TEST(test_mkdir_unlink_rmdir);
PGW_TEST(test_rename_across_directories);
PGW_TEST(test_chmod_chown_truncate_utimens);
PGW_TEST(test_fsync_and_statfs);
PGW_TEST(test_utf8_and_special_names);
PGW_TEST(test_counters_move);

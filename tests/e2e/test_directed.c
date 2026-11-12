/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_directed.c - the directed cases of spec 7.4, PC edition.
 *
 * Random suites reach some situations rarely or never; these cases aim at them
 * on purpose: rename onto an existing file, exclusive create, permission
 * matrix, symlink loops, a 255-byte name, a large directory listing resumed
 * from an offset, hard links, read-only mount, guest-local locks.
 *
 * Each case is written against pgw_rmfs over the POSIX truth backend, so the
 * expected result is whatever POSIX does - no expected value is invented here.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "testfs.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* FS-05: rename replaces an existing target atomically. */
static void test_rename_over_existing_file(void)
{
    testfs t;
    PGW_ASSERT(testfs_open(&t, "pgwdir-rename", NULL));
    pgw_rmfs_fd *fd = NULL;
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, "src",
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644,
                                  &fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_write(t.fs, fd, 0, "new", 3, &n), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, "dst",
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0600,
                                  &fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_write(t.fs, fd, 0, "old-and-longer", 14, &n), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, fd), PGW_OK);

    PGW_ASSERT_EQ_I(pgw_rmfs_rename(t.fs, &t.cred, "src", "dst"), PGW_OK);

    pgw_fs_attr at;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(t.fs, &t.cred, "dst", &at), PGW_OK);
    PGW_ASSERT_EQ_U(at.size, 3);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(t.fs, &t.cred, "src", &at), PGW_ENOENT);
    char abs[512];
    testfs_abs(&t, "dst", abs, sizeof(abs));
    struct stat st;
    PGW_ASSERT_EQ_I(stat(abs, &st), 0);
    PGW_ASSERT_EQ_U((uint64_t)st.st_size, 3);
    testfs_close(&t);
}
PGW_TEST(test_rename_over_existing_file);

/* FS-04: exclusive create, truncate on open, append at end. */
static void test_create_excl_trunc_append(void)
{
    testfs t;
    PGW_ASSERT(testfs_open(&t, "pgwdir-create", NULL));
    pgw_rmfs_fd *fd = NULL;
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, "f",
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_EXCL, 0644,
                                  &fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_write(t.fs, fd, 0, "0123456789", 10, &n), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, fd), PGW_OK);

    /* Second exclusive create must fail with EEXIST. */
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, "f",
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_EXCL, 0644,
                                  &fd), PGW_EEXIST);

    /* Append lands after the existing bytes. */
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, "f",
                                  PGW_O_WRONLY | PGW_O_APPEND, 0, &fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_write(t.fs, fd, 0, "AB", 2, &n), PGW_OK);
    PGW_ASSERT_EQ_U(n, 2);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, fd), PGW_OK);
    pgw_fs_attr at;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(t.fs, &t.cred, "f", &at), PGW_OK);
    PGW_ASSERT_EQ_U(at.size, 12);

    /* Truncate on open empties the file. */
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, "f",
                                  PGW_O_RDWR | PGW_O_TRUNC, 0, &fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(t.fs, &t.cred, "f", &at), PGW_OK);
    PGW_ASSERT_EQ_U(at.size, 0);
    testfs_close(&t);
}
PGW_TEST(test_create_excl_trunc_append);

/* FS-09 + SEC-01: the permission matrix is enforced inside the guest. */
static void test_permission_matrix(void)
{
    testfs t;
    PGW_ASSERT(testfs_open(&t, "pgwdir-perm", NULL));
    pgw_rmfs_fd *fd = NULL;
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, "locked",
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644,
                                  &fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_write(t.fs, fd, 0, "x", 1, &n), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_chmod(t.fs, &t.cred, "locked", 0600), PGW_OK);

    /* The truth backend creates files owned by the *process* uid, so read the
     * real owner and pick a caller that is neither the owner nor root: it falls
     * in the "other" class, where mode 0600 grants nothing. */
    char abs[512];
    testfs_abs(&t, "locked", abs, sizeof(abs));
    struct stat truth;
    PGW_ASSERT_EQ_I(stat(abs, &truth), 0);
    pgw_fs_cred other = { (uint32_t)truth.st_uid + 1u,
                          (uint32_t)truth.st_gid + 1u, 10 };
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &other, "locked", PGW_O_RDONLY, 0, &fd),
                    PGW_EACCES);
    /* Root bypasses the mode bits, as POSIX requires. */
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, "locked", PGW_O_RDONLY, 0, &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, fd), PGW_OK);

    /* A read-only mount rejects writes even from root (EROFS). */
    pgw_fs_opts ro;
    pgw_fs_opts_default(&ro);
    ro.readonly = true;
    testfs r;
    PGW_ASSERT(testfs_open(&r, "pgwdir-ro", &ro));
    PGW_ASSERT_EQ_I(pgw_rmfs_open(r.fs, &r.cred, "w",
                                  PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                    PGW_EROFS);
    testfs_close(&r);
    testfs_close(&t);
}
PGW_TEST(test_permission_matrix);

/* FS-08: symlink loops are detected, links are readable, hard links share. */
static void test_symlinks_and_hard_links(void)
{
    testfs t;
    PGW_ASSERT(testfs_open(&t, "pgwdir-link", NULL));
    PGW_ASSERT_EQ_I(pgw_rmfs_symlink(t.fs, &t.cred, "b", "a"), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_symlink(t.fs, &t.cred, "a", "b"), PGW_OK);
    char target[256];
    PGW_ASSERT_EQ_I(pgw_rmfs_readlink(t.fs, &t.cred, "a", target,
                                      sizeof(target)), PGW_OK);
    PGW_ASSERT_STR_EQ(target, "b");
    /* Resolving the loop must fail with ELOOP, not spin. */
    pgw_fs_attr at;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(t.fs, &t.cred, "a", &at), PGW_ELOOP);

    pgw_rmfs_fd *fd = NULL;
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, "real",
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644,
                                  &fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_write(t.fs, fd, 0, "shared", 6, &n), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_link(t.fs, &t.cred, "real", "alias"), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_lstat(t.fs, &t.cred, "alias", &at), PGW_OK);
    PGW_ASSERT_EQ_U(at.nlink, 2);
    PGW_ASSERT_EQ_U(at.size, 6);
    testfs_close(&t);
}
PGW_TEST(test_symlinks_and_hard_links);

/* FS-15: a 255-byte name works, a longer one is refused. */
static void test_long_names(void)
{
    testfs t;
    PGW_ASSERT(testfs_open(&t, "pgwdir-name", NULL));
    char name[PGW_NAME_MAX + 8];
    memset(name, 'n', PGW_NAME_MAX);
    name[PGW_NAME_MAX] = '\0';
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, name,
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644,
                                  &fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, fd), PGW_OK);
    pgw_fs_attr at;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(t.fs, &t.cred, name, &at), PGW_OK);
    PGW_ASSERT_EQ_I(at.type, PGW_FT_REG);

    name[PGW_NAME_MAX] = 'x'; /* 256 bytes */
    name[PGW_NAME_MAX + 1] = '\0';
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, name,
                                  PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                    PGW_ENAMETOOLONG);
    testfs_close(&t);
}
PGW_TEST(test_long_names);

/* FS-03: a large listing loses nothing, duplicates nothing, resumes. */
static void test_readdir_large_and_resumed(void)
{
    testfs t;
    PGW_ASSERT(testfs_open(&t, "pgwdir-readdir", NULL));
    const size_t total = 600;
    for (size_t i = 0; i < total; i++) {
        char name[32];
        snprintf(name, sizeof(name), "e%04zu", i);
        pgw_rmfs_fd *fd = NULL;
        PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, name,
                                      PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC,
                                      0644, &fd), PGW_OK);
        PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, fd), PGW_OK);
    }
    pgw_rmfs_fd *dir = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, ".",
                                  PGW_O_RDONLY | PGW_O_DIRECTORY, 0, &dir),
                    PGW_OK);
    /* Read in small pages to force several round trips and offsets. */
    /* A name buffer must hold a whole protocol name: a shorter one silently
     * truncates (gcc reports it as -Wformat-truncation in a Debug build). */
    static char seen[800][PGW_NAME_MAX + 1];
    size_t n_seen = 0;
    pgw_fs_dirent ents[7];
    uint64_t off = 0;
    for (;;) {
        size_t got = 0;
        uint64_t next = 0;
        PGW_ASSERT_EQ_I(pgw_rmfs_readdir(t.fs, dir, off, ents, 7, &got, &next),
                        PGW_OK);
        if (got == 0)
            break;
        for (size_t i = 0; i < got && n_seen < 800; i++)
            snprintf(seen[n_seen++], sizeof(seen[0]), "%s", ents[i].name);
        PGW_ASSERT(next > off);
        off = next;
    }
    PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, dir), PGW_OK);
    PGW_ASSERT_EQ_U(n_seen, total);
    for (size_t i = 0; i < total; i++) {
        char want[32];
        snprintf(want, sizeof(want), "e%04zu", i);
        bool found = false;
        for (size_t j = 0; j < n_seen; j++)
            found = found || strcmp(seen[j], want) == 0;
        PGW_ASSERT(found);
    }
    testfs_close(&t);
}
PGW_TEST(test_readdir_large_and_resumed);

/* FS-05: rmdir refuses a non-empty directory, then succeeds. */
static void test_rmdir_semantics(void)
{
    testfs t;
    PGW_ASSERT(testfs_open(&t, "pgwdir-rmdir", NULL));
    PGW_ASSERT_EQ_I(pgw_rmfs_mkdir(t.fs, &t.cred, "d", 0755), PGW_OK);
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, "d/f",
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644,
                                  &fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_unlink(t.fs, &t.cred, "d", true), PGW_ENOTEMPTY);
    PGW_ASSERT_EQ_I(pgw_rmfs_unlink(t.fs, &t.cred, "d/f", false), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_unlink(t.fs, &t.cred, "d", true), PGW_OK);
    pgw_fs_attr at;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(t.fs, &t.cred, "d", &at), PGW_ENOENT);
    testfs_close(&t);
}
PGW_TEST(test_rmdir_semantics);

/* FS-10: fsync reaches the backend, statfs reports the shared folder. */
static void test_fsync_and_statfs(void)
{
    testfs t;
    PGW_ASSERT(testfs_open(&t, "pgwdir-sync", NULL));
    pgw_rmfs_fd *fd = NULL;
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, "s",
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644,
                                  &fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_write(t.fs, fd, 0, "data", 4, &n), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_fsync(t.fs, fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, fd), PGW_OK);

    pgw_fs_statvfs vfs;
    PGW_ASSERT_EQ_I(pgw_rmfs_statfs(t.fs, &t.cred, &vfs), PGW_OK);
    PGW_ASSERT(vfs.bsize > 0);
    PGW_ASSERT(vfs.blocks > 0);
    testfs_close(&t);
}
PGW_TEST(test_fsync_and_statfs);

/* FS-14: byte-range locks are local to the guest and conflict correctly. */
static void test_local_locks(void)
{
    testfs t;
    PGW_ASSERT(testfs_open(&t, "pgwdir-lock", NULL));
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, "lk",
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644,
                                  &fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, fd), PGW_OK);

    PGW_ASSERT_EQ_I(pgw_rmfs_lock(t.fs, "lk", 11, PGW_LOCK_WR, 0, 100), PGW_OK);
    /* Another process cannot take a write lock on the same range. POSIX allows
     * either EACCES or EAGAIN for a refused fcntl lock; both are correct. */
    pgw_err conflict = pgw_rmfs_lock(t.fs, "lk", 22, PGW_LOCK_WR, 50, 100);
    PGW_ASSERT(conflict == PGW_EAGAIN || conflict == PGW_EACCES);
    /* F_GETLK semantics: ask as *another* process for a conflicting range and
     * the answer names the holder. Asking as the owner finds no conflict, which
     * is also POSIX-correct and worth pinning down. */
    pgw_lock_type ty = PGW_LOCK_WR;
    uint64_t start = 50, len = 100;
    uint32_t holder = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_getlock(t.fs, "lk", 22, &ty, &start, &len, &holder),
                    PGW_OK);
    PGW_ASSERT_EQ_I(ty, PGW_LOCK_WR);
    PGW_ASSERT_EQ_U(holder, 11);
    PGW_ASSERT_EQ_U(start, 0);
    PGW_ASSERT_EQ_U(len, 100);

    ty = PGW_LOCK_WR;
    holder = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_getlock(t.fs, "lk", 11, &ty, &start, &len, &holder),
                    PGW_OK);
    PGW_ASSERT_EQ_I(ty, PGW_LOCK_UN); /* own lock never conflicts with itself */
    /* A dying process releases everything it held. */
    pgw_rmfs_unlock_pid(t.fs, 11);
    PGW_ASSERT_EQ_I(pgw_rmfs_lock(t.fs, "lk", 22, PGW_LOCK_WR, 50, 100), PGW_OK);
    testfs_close(&t);
}
PGW_TEST(test_local_locks);

/* The priority scheduler itself (QNX-04, QNX-05, PERF-05) is covered by
 * rmfs/tests/test_rmfs_sched.c, including the ordering guarantee and the
 * non-blocking enqueue; it is deliberately not tested twice here. */

/* FS-08: a relative symlink target is relative to the directory holding the
 * link, never to the share root. The differential bench found this while
 * running as root: for `d/link -> target.txt` the share answered with the
 * content of `<root>/target.txt` while POSIX looks for `<root>/d/target.txt`.
 * The bug was in the redirect target, which was joined from the root. */
static void test_relative_symlink_target_is_relative_to_its_dir(void)
{
    testfs t;
    PGW_ASSERT(testfs_open(&t, "pgwdir-rellink", NULL));
    PGW_ASSERT_EQ_I(pgw_rmfs_mkdir(t.fs, &t.cred, "d", 0755), PGW_OK);
    pgw_rmfs_fd *fd = NULL;
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, "target.txt",
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644,
                                  &fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_write(t.fs, fd, 0, "at-root", 7, &n), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, "d/target.txt",
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644,
                                  &fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_write(t.fs, fd, 0, "inside-d", 8, &n), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, fd), PGW_OK);

    PGW_ASSERT_EQ_I(pgw_rmfs_symlink(t.fs, &t.cred, "target.txt", "d/link"),
                    PGW_OK);

    /* Reading d/link must yield the file inside d/, not the one at the root. */
    PGW_ASSERT_EQ_I(pgw_rmfs_open(t.fs, &t.cred, "d/link", PGW_O_RDONLY, 0, &fd),
                    PGW_OK);
    char buf[32] = { 0 };
    PGW_ASSERT_EQ_I(pgw_rmfs_read(t.fs, fd, 0, buf, sizeof(buf) - 1, &n), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(t.fs, fd), PGW_OK);
    PGW_ASSERT_STR_EQ(buf, "inside-d");

    /* The truth agrees: the host resolves the very same link the same way. */
    char abs[512];
    testfs_abs(&t, "d/link", abs, sizeof(abs));
    FILE *f = fopen(abs, "rb");
    PGW_ASSERT(f != NULL);
    char want[32] = { 0 };
    size_t m = fread(want, 1, sizeof(want) - 1, f);
    fclose(f);
    PGW_ASSERT_EQ_U(m, 8);
    PGW_ASSERT_STR_EQ(want, "inside-d");
    testfs_close(&t);
}
PGW_TEST(test_relative_symlink_target_is_relative_to_its_dir);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}

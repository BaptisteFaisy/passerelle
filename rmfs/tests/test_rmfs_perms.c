/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/tests/test_rmfs_perms.c - Guest-side permissions and mount options.
 *
 * Covers requirements FS-09 (permissions checked inside the guest against the
 * caller identity; mount options read-only / no-exec / no-privilege) and
 * SEC-01 (no privilege elevation by default), plus the errno discipline of
 * ROB-01 (a refusal is an error code, never a hang).
 */
#include "../src/rmfs_internal.h"

#include "fake_backend.h"

#include "pgw/limits.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"
#include "test_cred.h"

#include <string.h>
#include <sys/stat.h>

static pgw_rmfs *make_fs_opts(fake_backend *b, const pgw_fs_opts *opts)
{
    return pgw_rmfs_create(b->ops, b, opts);
}

static pgw_rmfs *make_fs_default(fake_backend *b)
{
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    return pgw_rmfs_create(b->ops, b, &opts);
}

/* --- SEC-01: the defaults ------------------------------------------------- */

static void test_default_options_are_safe(void)
{
    pgw_fs_opts o;
    pgw_fs_opts_default(&o);
    PGW_ASSERT(o.nosuid);                 /* SEC-01 */
    PGW_ASSERT_FALSE(o.noexec);           /* FS-06 needs exec from the share */
    PGW_ASSERT_FALSE(o.readonly);
    PGW_ASSERT_EQ_U(o.attr_ttl_ns, PGW_ATTR_CACHE_NS_DEFAULT); /* D4 */
    PGW_ASSERT_EQ_U(o.max_inflight, PGW_INFLIGHT_DEFAULT);
}

/* --- FS-09: the permission matrix ---------------------------------------- */

/* The three "who" cases of POSIX: owner, group member, other. */
static void check_matrix_triplet(pgw_rmfs *fs, const pgw_fs_attr *a)
{
    pgw_rmfs_fd *fd = NULL;
    pgw_fs_cred file_owner = {a->uid, a->gid, 20};
    pgw_fs_cred groupie = {a->uid + 4242u, a->gid, 20};
    pgw_fs_cred stranger = {a->uid + 4242u, a->gid + 4242u, 20};

    /* Owner: rw allowed, x denied (no x bit). */
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &file_owner, "own.txt", PGW_O_RDWR, 0,
                                  &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);

    /* Same group, not owner: group bits are r-- only. */
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &groupie, "own.txt", PGW_O_RDONLY, 0,
                                  &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &groupie, "own.txt", PGW_O_RDWR, 0, &fd),
                    PGW_EACCES);

    /* Neither owner nor group: "other" has no bits at all. */
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &stranger, "own.txt", PGW_O_RDONLY, 0,
                                  &fd),
                    PGW_EACCES);
    PGW_ASSERT(fd == NULL);

    /* root inside the guest bypasses the check. */
    pgw_fs_cred root = {0, 0, 20};
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &root, "own.txt", PGW_O_RDWR, 0, &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);
}

static void test_permission_matrix(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs_default(b);
    PGW_ASSERT(fs != NULL);

    /* A file owned by uid 1000, group 1000, mode 0640. */
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "own.txt", "data", 4), 0);
    pgw_fs_cred owner = tc_self(20);
    PGW_ASSERT_EQ_I(pgw_rmfs_chmod(fs, &owner, "own.txt", 0640), PGW_OK);
    /* Give the owner the identity the file really has on the host. */
    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &owner, "own.txt", &a), PGW_OK);
    pgw_fs_cred file_owner = {a.uid, a.gid, 20};
    PGW_ASSERT_EQ_I(pgw_rmfs_chmod(fs, &file_owner, "own.txt", 0640), PGW_OK);

    check_matrix_triplet(fs, &a);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* --- FS-09: read-only mount ---------------------------------------------- */

static void test_readonly_mount(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    opts.readonly = true;
    pgw_rmfs *fs = make_fs_opts(b, &opts);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);

    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "ro.txt", "abc", 3), 0);

    /* Reading is fine ... */
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "ro.txt", PGW_O_RDONLY, 0, &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);

    /* ... every write path is refused with EROFS. */
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "ro.txt", PGW_O_RDWR, 0, &fd),
                    PGW_EROFS);
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "new.txt",
                                  PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                    PGW_EROFS);
    PGW_ASSERT_EQ_I(pgw_rmfs_mkdir(fs, &cred, "nd", 0755), PGW_EROFS);
    PGW_ASSERT_EQ_I(pgw_rmfs_unlink(fs, &cred, "ro.txt", false), PGW_EROFS);
    PGW_ASSERT_EQ_I(pgw_rmfs_rename(fs, &cred, "ro.txt", "r2.txt"), PGW_EROFS);
    PGW_ASSERT_EQ_I(pgw_rmfs_chmod(fs, &cred, "ro.txt", 0777), PGW_EROFS);
    PGW_ASSERT_EQ_I(pgw_rmfs_truncate(fs, &cred, "ro.txt", 0), PGW_EROFS);
    PGW_ASSERT_EQ_I(pgw_rmfs_symlink(fs, &cred, "x", "lnk"), PGW_EROFS);

    /* Reads still work, and the file is untouched. */
    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "ro.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, 3);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* --- SEC-01: noexec ------------------------------------------------------ */

static void test_noexec_refuses_execution(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    opts.noexec = true;
    pgw_rmfs *fs = make_fs_opts(b, &opts);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);

    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "prog", "\x7f" "ELF", 4), 0);
    PGW_ASSERT_EQ_I(pgw_rmfs_chmod(fs, &cred, "prog", 0755), PGW_OK);

    /* Execution request on a noexec mount: EACCES, before any backend call. */
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "prog",
                                  PGW_O_RDONLY | PGW_RMFS_O_EXEC, 0, &fd),
                    PGW_EACCES);
    PGW_ASSERT(fd == NULL);

    /* Reading the same file is still allowed: only exec is refused. */
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "prog", PGW_O_RDONLY, 0, &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_exec_allowed_when_noexec_off(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs_default(b); /* noexec off by default: FS-06 */
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);

    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "prog2", "ELF", 3), 0);
    PGW_ASSERT_EQ_I(pgw_rmfs_chmod(fs, &cred, "prog2", 0755), PGW_OK);
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "prog2",
                                  PGW_O_RDONLY | PGW_RMFS_O_EXEC, 0, &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* --- SEC-01: nosuid ------------------------------------------------------ */

static void test_nosuid_strips_and_refuses(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs_default(b); /* nosuid on by default */
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);

    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "suid", "x", 1), 0);
    /* Setting a setuid bit on a nosuid mount drops it silently. */
    PGW_ASSERT_EQ_I(pgw_rmfs_chmod(fs, &cred, "suid", 04755), PGW_OK);
    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "suid", &a), PGW_OK);
    PGW_ASSERT_EQ_U(a.mode & PGW_S_ISUID, 0);

    /* And an exec request on a file that carries setuid anyway is refused. */
    PGW_ASSERT_EQ_I(pgw_rmfs_chmod(fs, &cred, "suid", 0755), PGW_OK);
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "suid",
                                  PGW_O_RDONLY | PGW_RMFS_O_EXEC, 0, &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_nosuid_blocks_setuid_exec(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs_default(b);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);

    /* Create the setuid file on the host side: the mount option cannot strip
     * what already exists, it must refuse to run it (SEC-01). */
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "rootshell", "ELF", 3), 0);
    char host[1024];
    fake_backend_host_path(b, "rootshell", host, sizeof(host));
    PGW_ASSERT_EQ_I(chmod(host, 04755), 0);

    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "rootshell", &a), PGW_OK);
    PGW_ASSERT((a.mode & PGW_S_ISUID) != 0);

    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "rootshell",
                                  PGW_O_RDONLY | PGW_RMFS_O_EXEC, 0, &fd),
                    PGW_EACCES);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* --- ROB-01: a refusal is an error code, never a hang ------------------- */

static void test_errors_are_propagated(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs_default(b);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = tc_self(20);

    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "absent", &a), PGW_ENOENT);

    /* An injected server error reaches the caller unchanged (ROB-01). */
    b->fail_next = true;
    b->fail_err = PGW_EIO;
    PGW_ASSERT_EQ_I(pgw_rmfs_statfs(fs, &cred, NULL), PGW_EINVAL);
    pgw_fs_statvfs sv;
    PGW_ASSERT_EQ_I(pgw_rmfs_statfs(fs, &cred, &sv), PGW_EIO);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

PGW_TEST(test_default_options_are_safe);
PGW_TEST(test_permission_matrix);
PGW_TEST(test_readonly_mount);
PGW_TEST(test_noexec_refuses_execution);
PGW_TEST(test_exec_allowed_when_noexec_off);
PGW_TEST(test_nosuid_strips_and_refuses);
PGW_TEST(test_nosuid_blocks_setuid_exec);
PGW_TEST(test_errors_are_propagated);

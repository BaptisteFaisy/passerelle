/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/tests/test_rmfs_path.c - Path normalization and symlink resolution.
 *
 * Covers requirements FS-08 (symlinks: creation, reading, resolution, loop
 * detection; hard links) and FS-15 (UTF-8 names up to 255 bytes, special
 * characters), plus the redirect response of specification section 5.3: the
 * driver must not follow a symlink itself, it hands the target and the
 * remaining path back to the client.
 */
#include "../src/path.h"

#include "fake_backend.h"
#include "pgw/limits.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"

#include <string.h>

/* --- FS-15: normalization ------------------------------------------------ */

static void test_normalize_collapses(void)
{
    char out[PGW_PATH_MAX];
    PGW_ASSERT_EQ_I(rmfs_path_normalize("a//b/./c", out, sizeof(out)), PGW_OK);
    PGW_ASSERT_STR_EQ(out, "a/b/c");
    PGW_ASSERT_EQ_I(rmfs_path_normalize("./a", out, sizeof(out)), PGW_OK);
    PGW_ASSERT_STR_EQ(out, "a");
    PGW_ASSERT_EQ_I(rmfs_path_normalize("a/b/../c", out, sizeof(out)), PGW_OK);
    PGW_ASSERT_STR_EQ(out, "a/c");
    PGW_ASSERT_EQ_I(rmfs_path_normalize("///", out, sizeof(out)), PGW_OK);
    PGW_ASSERT_STR_EQ(out, ".");
    PGW_ASSERT_EQ_I(rmfs_path_normalize("..", out, sizeof(out)), PGW_OK);
    PGW_ASSERT_STR_EQ(out, ".");
}

static void test_normalize_clamps_at_root(void)
{
    char out[PGW_PATH_MAX];
    PGW_ASSERT_EQ_I(rmfs_path_normalize("a/../../etc/passwd", out,
                                        sizeof(out)),
                    PGW_OK);
    PGW_ASSERT_STR_EQ(out, "etc/passwd");
    PGW_ASSERT_EQ_I(rmfs_path_normalize("/../..", out, sizeof(out)), PGW_OK);
    PGW_ASSERT_STR_EQ(out, ".");
}

static void test_normalize_rejects(void)
{
    char out[PGW_PATH_MAX];
    PGW_ASSERT_EQ_I(rmfs_path_normalize("", out, sizeof(out)), PGW_ENOENT);

    /* FS-15: a component of exactly 255 bytes is legal ... */
    char name[PGW_NAME_MAX + 1];
    memset(name, 'n', PGW_NAME_MAX);
    name[PGW_NAME_MAX] = '\0';
    PGW_ASSERT_EQ_I(rmfs_path_normalize(name, out, sizeof(out)), PGW_OK);
    PGW_ASSERT_EQ_U(strlen(out), PGW_NAME_MAX);

    /* ... one more byte is not. */
    char long_name[PGW_NAME_MAX + 2];
    memset(long_name, 'n', PGW_NAME_MAX + 1);
    long_name[PGW_NAME_MAX + 1] = '\0';
    PGW_ASSERT_EQ_I(rmfs_path_normalize(long_name, out, sizeof(out)),
                    PGW_ENAMETOOLONG);
}

static void test_normalize_keeps_utf8(void)
{
    char out[PGW_PATH_MAX];
    const char *name = "r\xc3\xa9pertoire/\xc3\xa9t\xc3\xa9.txt";
    PGW_ASSERT_EQ_I(rmfs_path_normalize(name, out, sizeof(out)), PGW_OK);
    PGW_ASSERT_STR_EQ(out, name);
    /* 4 bytes of UTF-8 count as 4 of the 255 allowed */
    PGW_ASSERT_EQ_U(strlen(out), strlen(name));
}

/* --- FS-08 + spec 5.3: symlink redirect -------------------------------- */

static pgw_rmfs *make_fs(fake_backend *b)
{
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    return pgw_rmfs_create(b->ops, b, &opts);
}

static void test_symlink_yields_redirect(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);

    pgw_fs_cred cred = {1000, 1000, 10};
    PGW_ASSERT_EQ_I(pgw_rmfs_mkdir(fs, &cred, "dir", 0755), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_symlink(fs, &cred, "dir", "link"), PGW_OK);

    /* A symlink met mid-path produces a redirect, not a followed path. */
    char resolved[PGW_PATH_MAX];
    rmfs_redirect redir;
    pgw_err e = rmfs_path_resolve(b->ops, b, "link/file.txt", 10, true,
                                  resolved, sizeof(resolved), &redir);
    PGW_ASSERT_EQ_I(e, PGW_OK);
    PGW_ASSERT(redir.is_redirect);
    PGW_ASSERT_STR_EQ(redir.target, "dir");
    PGW_ASSERT_STR_EQ(redir.remainder, "file.txt");

    /* A symlink in the middle of the path is never silently followed by the
     * resolver itself: the caller decides (spec 5.3). */
    PGW_ASSERT_STR_EQ(resolved, "link/file.txt");

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_symlink_follow_when_asked(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = {1000, 1000, 10};

    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "target.txt", "hello", 5),
                    0);
    PGW_ASSERT_EQ_I(pgw_rmfs_symlink(fs, &cred, "target.txt", "alias"),
                    PGW_OK);

    /* Following the redirect chain lands on the real file. */
    char resolved[PGW_PATH_MAX];
    PGW_ASSERT_EQ_I(rmfs_path_follow(b->ops, b, "alias", 10, true, resolved,
                                     sizeof(resolved)),
                    PGW_OK);
    PGW_ASSERT_STR_EQ(resolved, "target.txt");

    /* stat follows, lstat does not. */
    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "alias", &a), PGW_OK);
    PGW_ASSERT_EQ_I(a.type, PGW_FT_REG);
    PGW_ASSERT_EQ_U(a.size, 5);
    PGW_ASSERT_EQ_I(pgw_rmfs_lstat(fs, &cred, "alias", &a), PGW_OK);
    PGW_ASSERT_EQ_I(a.type, PGW_FT_LNK);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_symlink_loop_detected(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = {1000, 1000, 10};

    /* loop -> loop : resolution must stop with ELOOP, not spin (FS-08). */
    PGW_ASSERT_EQ_I(pgw_rmfs_symlink(fs, &cred, "loop", "loop"), PGW_OK);
    char resolved[PGW_PATH_MAX];
    PGW_ASSERT_EQ_I(rmfs_path_follow(b->ops, b, "loop", 10, true, resolved,
                                     sizeof(resolved)),
                    PGW_ELOOP);
    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "loop", &a), PGW_ELOOP);

    /* A two-link cycle is caught as well. */
    PGW_ASSERT_EQ_I(pgw_rmfs_symlink(fs, &cred, "b", "a"), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_symlink(fs, &cred, "a", "b"), PGW_OK);
    PGW_ASSERT_EQ_I(rmfs_path_follow(b->ops, b, "a", 10, true, resolved,
                                     sizeof(resolved)),
                    PGW_ELOOP);

    /* A chain shorter than PGW_SYMLINK_MAX still resolves. */
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "end.txt", "x", 1), 0);
    PGW_ASSERT_EQ_I(pgw_rmfs_symlink(fs, &cred, "end.txt", "c1"), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_symlink(fs, &cred, "c1", "c2"), PGW_OK);
    PGW_ASSERT_EQ_I(rmfs_path_follow(b->ops, b, "c2", 10, true, resolved,
                                     sizeof(resolved)),
                    PGW_OK);
    PGW_ASSERT_STR_EQ(resolved, "end.txt");

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_readlink_returns_body(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = {1000, 1000, 10};

    PGW_ASSERT_EQ_I(pgw_rmfs_symlink(fs, &cred, "some/where", "lnk"), PGW_OK);
    char target[PGW_PATH_MAX];
    PGW_ASSERT_EQ_I(pgw_rmfs_readlink(fs, &cred, "lnk", target,
                                      sizeof(target)),
                    PGW_OK);
    PGW_ASSERT_STR_EQ(target, "some/where");

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_hard_link_shares_inode(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = {1000, 1000, 10};

    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "orig.txt", "abc", 3), 0);
    PGW_ASSERT_EQ_I(pgw_rmfs_link(fs, &cred, "orig.txt", "copy.txt"), PGW_OK);
    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "copy.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, 3);
    PGW_ASSERT_EQ_U(a.nlink, 2);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

PGW_TEST(test_normalize_collapses);
PGW_TEST(test_normalize_clamps_at_root);
PGW_TEST(test_normalize_rejects);
PGW_TEST(test_normalize_keeps_utf8);
PGW_TEST(test_symlink_yields_redirect);
PGW_TEST(test_symlink_follow_when_asked);
PGW_TEST(test_symlink_loop_detected);
PGW_TEST(test_readlink_returns_body);
static void test_open_nofollow_refuses_symlink(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = {1000, 1000, 10};

    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "real.txt", "abc", 3), 0);
    PGW_ASSERT_EQ_I(pgw_rmfs_symlink(fs, &cred, "real.txt", "ln"), PGW_OK);

    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "ln",
                                  PGW_O_RDONLY | PGW_O_NOFOLLOW, 0, &fd),
                    PGW_ELOOP);
    PGW_ASSERT(fd == NULL);

    /* Without O_NOFOLLOW the link is followed and the file opens. */
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "ln", PGW_O_RDONLY, 0, &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

PGW_TEST(test_hard_link_shares_inode);
PGW_TEST(test_open_nofollow_refuses_symlink);

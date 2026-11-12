/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * cas_cases_hardlink.c - gap 68: the hard-link semantics.
 *
 * The link count was checked, the rest was not: two names of one inode, the
 * identity behind them, a write through one name seen through the other, the
 * unlink of one name, the rename and the link onto an existing target, and
 * the attribute cache that must not keep a stale link count.
 *
 * Every expected value comes from the truth (the real directory), never from
 * the layer under test, and every body takes a cas_view so that the same
 * assertions run over the POSIX backend, over the 9P adapter and over the
 * FUSE adapter.
 *
 * Covers gap 68 of docs/gap-analysis.md, FS-05, FS-13.
 */
#define _POSIX_C_SOURCE 200809L
#include "cas_cases.h"

#include "ref9p_truth.h"
#include "test_cred.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"
#include "pgw/time.h"

#include <string.h>

/* The validity of the attribute cache of the cache case. */
#define CACHE_TTL_NS 1000000000ULL

/* Create a file through the share. */
static void make_file(const cas_view *v, const char *rel, const char *text)
{
    pgw_rmfs_fd *fd = NULL;
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(v->fs, &v->cred, rel,
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644, &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_write(v->fs, fd, 0, text, strlen(text), &n), PGW_OK);
    PGW_ASSERT_EQ_U(n, strlen(text));
    PGW_ASSERT_EQ_I(pgw_rmfs_close(v->fs, fd), PGW_OK);
}

/* The content of a file, read through the share. */
static void read_is(const cas_view *v, const char *rel, const char *want)
{
    pgw_rmfs_fd *fd = NULL;
    char buf[128] = {0};
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(v->fs, &v->cred, rel, PGW_O_RDONLY, 0, &fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_read(v->fs, fd, 0, buf, sizeof(buf) - 1, &got), PGW_OK);
    PGW_ASSERT_EQ_U(got, strlen(want));
    PGW_ASSERT_STR_EQ(buf, want);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(v->fs, fd), PGW_OK);
}

/* The link count and the inode number the layer reports for a name. */
static void layer_identity(const cas_view *v, const char *rel, uint64_t *out_ino,
                           uint64_t *out_nlink)
{
    pgw_fs_attr at;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(v->fs, &v->cred, rel, &at), PGW_OK);
    *out_ino = at.ino;
    *out_nlink = at.nlink;
}

/* Two names, one inode: the link count of both, the identity, and a write
 * through one name seen through the other. */
void cas_case_link_identity(const cas_view *v)
{
    make_file(v, "a.txt", "data");
    PGW_ASSERT_EQ_I(pgw_rmfs_link(v->fs, &v->cred, "a.txt", "b.txt"), PGW_OK);

    uint64_t ino_a = 0;
    uint64_t nlink_a = 0;
    uint64_t ino_b = 0;
    uint64_t nlink_b = 0;
    layer_identity(v, "a.txt", &ino_a, &nlink_a);
    layer_identity(v, "b.txt", &ino_b, &nlink_b);
    PGW_ASSERT_EQ_U(nlink_a, 2);
    PGW_ASSERT_EQ_U(nlink_b, 2);
    PGW_ASSERT_EQ_U(ino_a, ino_b);

    /* The truth agrees: one inode, two names. */
    struct stat sa;
    struct stat sb;
    PGW_ASSERT(ref9p_truth_stat(v->root, "a.txt", &sa));
    PGW_ASSERT(ref9p_truth_stat(v->root, "b.txt", &sb));
    PGW_ASSERT_EQ_U((uint64_t)sa.st_nlink, 2);
    PGW_ASSERT_EQ_U((uint64_t)sb.st_nlink, 2);
    PGW_ASSERT_EQ_U((uint64_t)sa.st_ino, (uint64_t)sb.st_ino);
    PGW_ASSERT_EQ_U((uint64_t)sa.st_dev, (uint64_t)sb.st_dev);

    /* A write through one name is seen through the other. */
    pgw_rmfs_fd *fd = NULL;
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(v->fs, &v->cred, "a.txt", PGW_O_RDWR, 0, &fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_write(v->fs, fd, 0, "DATA!", 5, &n), PGW_OK);
    PGW_ASSERT_EQ_U(n, 5);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(v->fs, fd), PGW_OK);
    read_is(v, "b.txt", "DATA!");

    char buf[16] = {0};
    size_t len = 0;
    PGW_ASSERT(cas_truth_read(v->root, "b.txt", buf, sizeof(buf) - 1, &len));
    PGW_ASSERT_EQ_U(len, 5);
    PGW_ASSERT_STR_EQ(buf, "DATA!");
}

/* Unlinking one name leaves the other intact, with a decremented count. */
void cas_case_link_unlink(const cas_view *v)
{
    make_file(v, "u1.txt", "data");
    PGW_ASSERT_EQ_I(pgw_rmfs_link(v->fs, &v->cred, "u1.txt", "u2.txt"), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_unlink(v->fs, &v->cred, "u1.txt", false), PGW_OK);

    uint64_t ino = 0;
    uint64_t nlink = 0;
    layer_identity(v, "u2.txt", &ino, &nlink);
    PGW_ASSERT_EQ_U(nlink, 1);
    read_is(v, "u2.txt", "data");

    struct stat st;
    PGW_ASSERT(ref9p_truth_stat(v->root, "u2.txt", &st));
    PGW_ASSERT_EQ_U((uint64_t)st.st_nlink, 1);
    PGW_ASSERT_FALSE(ref9p_truth_stat(v->root, "u1.txt", &st));

    pgw_fs_attr at;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(v->fs, &v->cred, "u1.txt", &at), PGW_ENOENT);
}

/* Renaming a name onto an existing one, and renaming a name onto another
 * name of the same inode (POSIX: nothing happens, both names stay). */
void cas_case_link_rename(const cas_view *v)
{
    make_file(v, "x.txt", "new");
    make_file(v, "y.txt", "old-and-longer");
    PGW_ASSERT_EQ_I(pgw_rmfs_rename(v->fs, &v->cred, "x.txt", "y.txt"), PGW_OK);

    pgw_fs_attr at;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(v->fs, &v->cred, "y.txt", &at), PGW_OK);
    PGW_ASSERT_EQ_U(at.size, 3);
    PGW_ASSERT_EQ_U(at.nlink, 1);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(v->fs, &v->cred, "x.txt", &at), PGW_ENOENT);
    read_is(v, "y.txt", "new");

    struct stat st;
    PGW_ASSERT(ref9p_truth_stat(v->root, "y.txt", &st));
    PGW_ASSERT_EQ_U((uint64_t)st.st_size, 3);
    PGW_ASSERT_FALSE(ref9p_truth_stat(v->root, "x.txt", &st));

    /* A name renamed onto another name of the same inode changes nothing. */
    make_file(v, "h1.txt", "same");
    PGW_ASSERT_EQ_I(pgw_rmfs_link(v->fs, &v->cred, "h1.txt", "h2.txt"), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_rename(v->fs, &v->cred, "h2.txt", "h1.txt"), PGW_OK);

    uint64_t ino1 = 0;
    uint64_t nlink1 = 0;
    uint64_t ino2 = 0;
    uint64_t nlink2 = 0;
    layer_identity(v, "h1.txt", &ino1, &nlink1);
    layer_identity(v, "h2.txt", &ino2, &nlink2);
    PGW_ASSERT_EQ_U(nlink1, 2);
    PGW_ASSERT_EQ_U(nlink2, 2);
    PGW_ASSERT_EQ_U(ino1, ino2);
    PGW_ASSERT(ref9p_truth_stat(v->root, "h1.txt", &st));
    PGW_ASSERT(ref9p_truth_stat(v->root, "h2.txt", &st));
    PGW_ASSERT_EQ_U((uint64_t)st.st_nlink, 2);
}

/* Linking onto a name that already exists is refused, and the link count of
 * the file does not change. */
void cas_case_link_target(const cas_view *v)
{
    make_file(v, "t1.txt", "data");
    PGW_ASSERT_EQ_I(pgw_rmfs_link(v->fs, &v->cred, "t1.txt", "t2.txt"), PGW_OK);

    /* The target exists: POSIX refuses the link. */
    PGW_ASSERT_EQ_I(pgw_rmfs_link(v->fs, &v->cred, "t1.txt", "t2.txt"), PGW_EEXIST);
    /* A file cannot be linked onto itself either. */
    PGW_ASSERT_EQ_I(pgw_rmfs_link(v->fs, &v->cred, "t1.txt", "t1.txt"), PGW_EEXIST);

    uint64_t ino = 0;
    uint64_t nlink = 0;
    layer_identity(v, "t1.txt", &ino, &nlink);
    PGW_ASSERT_EQ_U(nlink, 2);
    layer_identity(v, "t2.txt", &ino, &nlink);
    PGW_ASSERT_EQ_U(nlink, 2);

    struct stat st;
    PGW_ASSERT(ref9p_truth_stat(v->root, "t1.txt", &st));
    PGW_ASSERT_EQ_U((uint64_t)st.st_nlink, 2);
    PGW_ASSERT_EQ_U(cas_truth_count(v->root, "."), 2);
}

/* The attribute cache must not keep a stale link count, neither after a link
 * made through the share nor after an unlink made on the PC. */
void cas_case_link_cache(const cas_view *v)
{
    uint64_t now = 1000;
    pgw_time_set_clock(cas_fake_clock, &now);
    make_file(v, "k1.txt", "data");

    uint64_t ino = 0;
    uint64_t nlink = 0;
    layer_identity(v, "k1.txt", &ino, &nlink);
    PGW_ASSERT_EQ_U(nlink, 1);

    /* The link changes the count of the existing name: the cache must not
     * serve the count it had before. */
    PGW_ASSERT_EQ_I(pgw_rmfs_link(v->fs, &v->cred, "k1.txt", "k2.txt"), PGW_OK);
    layer_identity(v, "k1.txt", &ino, &nlink);
    PGW_ASSERT_EQ_U(nlink, 2);
    layer_identity(v, "k2.txt", &ino, &nlink);
    PGW_ASSERT_EQ_U(nlink, 2);

    /* The PC removes one name: the layer cannot see it, the entry of the
     * other name must expire instead of staying at two. */
    PGW_ASSERT(cas_host_unlink(v->root, "k1.txt"));
    PGW_ASSERT(pgw_time_advance(CACHE_TTL_NS + 1));
    layer_identity(v, "k2.txt", &ino, &nlink);
    PGW_ASSERT_EQ_U(nlink, 1);
    pgw_time_set_clock(NULL, NULL);
}

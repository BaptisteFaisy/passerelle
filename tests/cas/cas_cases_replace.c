/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * cas_cases_replace.c - ROB-04: a file replaced on the PC while it is open.
 *
 * The specification asks for clean error codes when a file is unlinked or
 * replaced on the host during guest use (ROB-04). These bodies pin the whole
 * story, and they are written against the cas_view so that the same
 * assertions run over every adapter:
 *
 *   - a descriptor opened before the replacement keeps reading the *old*
 *     inode until it is closed (POSIX);
 *   - a descriptor opened after it sees the *new* file;
 *   - the attributes of the name describe the new file: no stale size, no
 *     stale mtime, no stale inode number - the attribute cache included;
 *   - the contrast case: the file truncated in place keeps its inode, so the
 *     open descriptor sees the new content;
 *   - the contrast case: the file unlinked while open stays readable and the
 *     name disappears;
 *   - nothing crashes and no descriptor is left behind.
 *
 * The truth side is always the real directory, read with plain POSIX calls.
 *
 * Covers ROB-04 (gap 19 of docs/gap-analysis.md), FS-11, FS-12, FS-13.
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

/* Two modification times, far enough apart that "the client still serves the
 * old mtime" is a statement about the cache and not about the clock. */
#define OLD_MTIME_SEC 1000000000LL /* 2001-09-09 */
#define NEW_MTIME_SEC 1600000000LL /* 2020-09-13 */
#define NS_PER_SEC 1000000000ULL

/* The validity of the attribute cache of the cache cases: one second, so the
 * case can let the entry expire with the injected clock instead of sleeping. */
#define CACHE_TTL_NS 1000000000ULL

/* Create a file through the share and give it a known modification time. */
static void make_file(const cas_view *v, const char *rel, const char *text,
                      long long mtime_sec)
{
    pgw_rmfs_fd *fd = NULL;
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(v->fs, &v->cred, rel,
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644, &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_write(v->fs, fd, 0, text, strlen(text), &n), PGW_OK);
    PGW_ASSERT_EQ_U(n, strlen(text));
    PGW_ASSERT_EQ_I(pgw_rmfs_close(v->fs, fd), PGW_OK);
    PGW_ASSERT(cas_host_set_mtime(v->root, rel, mtime_sec));
}

/* Read the whole file through the share and compare with the expected text. */
static void read_is(const cas_view *v, pgw_rmfs_fd *fd, const char *want)
{
    char buf[128] = {0};
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_read(v->fs, fd, 0, buf, sizeof(buf) - 1, &got), PGW_OK);
    PGW_ASSERT_EQ_U(got, strlen(want));
    PGW_ASSERT_STR_EQ(buf, want);
}

/* The editor pattern: a new file is renamed over the old one while a
 * descriptor is open on the old one. */
void cas_case_replace(const cas_view *v)
{
    const char *old_text = "old-content";
    const char *new_text = "new-content-longer";
    make_file(v, "doc.txt", old_text, OLD_MTIME_SEC);

    pgw_rmfs_fd *old_fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(v->fs, &v->cred, "doc.txt", PGW_O_RDONLY, 0, &old_fd),
                    PGW_OK);
    pgw_fs_attr before;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(v->fs, &v->cred, "doc.txt", &before), PGW_OK);
    PGW_ASSERT_EQ_U(before.size, strlen(old_text));

    /* The PC replaces the file behind the share's back. */
    PGW_ASSERT(cas_host_write(v->root, "doc.new", new_text, strlen(new_text)));
    PGW_ASSERT(cas_host_set_mtime(v->root, "doc.new", NEW_MTIME_SEC));
    PGW_ASSERT(cas_host_rename(v->root, "doc.new", "doc.txt"));

    /* POSIX: the descriptor opened before keeps the inode it opened. */
    read_is(v, old_fd, old_text);

    /* A descriptor opened after sees the new file. */
    pgw_rmfs_fd *new_fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(v->fs, &v->cred, "doc.txt", PGW_O_RDONLY, 0, &new_fd),
                    PGW_OK);
    read_is(v, new_fd, new_text);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(v->fs, new_fd), PGW_OK);

    /* The name now describes the new file: new size, new inode, new mtime. */
    pgw_fs_attr after;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(v->fs, &v->cred, "doc.txt", &after), PGW_OK);
    PGW_ASSERT_EQ_U(after.size, strlen(new_text));
    PGW_ASSERT(after.ino != before.ino);
    PGW_ASSERT_EQ_U(after.mtime_ns / NS_PER_SEC, (uint64_t)NEW_MTIME_SEC);

    /* The truth agrees with the client. */
    struct stat st;
    PGW_ASSERT(ref9p_truth_stat(v->root, "doc.txt", &st));
    PGW_ASSERT_EQ_U((uint64_t)st.st_size, strlen(new_text));
    PGW_ASSERT_EQ_U((uint64_t)st.st_mtim.tv_sec, (uint64_t)NEW_MTIME_SEC);
    PGW_ASSERT((uint64_t)st.st_ino != before.ino);

    PGW_ASSERT_EQ_I(pgw_rmfs_close(v->fs, old_fd), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_rmfs_open_count(v->fs), 0);
}

/* The opposite of the rename: the inode stays, so the open descriptor sees
 * the new content and the new size. */
void cas_case_truncate(const cas_view *v)
{
    make_file(v, "t.txt", "0123456789", OLD_MTIME_SEC);
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(v->fs, &v->cred, "t.txt", PGW_O_RDWR, 0, &fd), PGW_OK);
    pgw_fs_attr before;
    PGW_ASSERT_EQ_I(pgw_rmfs_fstat(v->fs, fd, &before), PGW_OK);
    PGW_ASSERT_EQ_U(before.size, 10);

    /* The PC empties the file and writes two bytes into it. */
    PGW_ASSERT(cas_host_write(v->root, "t.txt", "ab", 2));

    pgw_fs_attr after;
    PGW_ASSERT_EQ_I(pgw_rmfs_fstat(v->fs, fd, &after), PGW_OK);
    PGW_ASSERT_EQ_U(after.size, 2);
    PGW_ASSERT_EQ_U(after.ino, before.ino);
    read_is(v, fd, "ab");

    struct stat st;
    PGW_ASSERT(ref9p_truth_stat(v->root, "t.txt", &st));
    PGW_ASSERT_EQ_U((uint64_t)st.st_size, 2);
    PGW_ASSERT_EQ_U((uint64_t)st.st_ino, before.ino);

    PGW_ASSERT_EQ_I(pgw_rmfs_close(v->fs, fd), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_rmfs_open_count(v->fs), 0);
}

/* The name disappears, the inode survives until the descriptor is closed. */
void cas_case_unlink(const cas_view *v)
{
    make_file(v, "u.txt", "keep-me", OLD_MTIME_SEC);
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(v->fs, &v->cred, "u.txt", PGW_O_RDONLY, 0, &fd),
                    PGW_OK);
    PGW_ASSERT(cas_host_unlink(v->root, "u.txt"));

    read_is(v, fd, "keep-me");

    pgw_fs_attr at;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(v->fs, &v->cred, "u.txt", &at), PGW_ENOENT);
    struct stat st;
    PGW_ASSERT_FALSE(ref9p_truth_stat(v->root, "u.txt", &st));

    PGW_ASSERT_EQ_I(pgw_rmfs_close(v->fs, fd), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_rmfs_open_count(v->fs), 0);
}

/* With the cache on, the first stat fills it. The replacement happens on the
 * PC, which the layer cannot see: the entry must expire with its bounded
 * validity instead of serving the attributes of a file that is gone. */
void cas_case_cache_stale(const cas_view *v)
{
    uint64_t now = 1000;
    pgw_time_set_clock(cas_fake_clock, &now);
    make_file(v, "c.txt", "first", OLD_MTIME_SEC);

    pgw_fs_attr first;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(v->fs, &v->cred, "c.txt", &first), PGW_OK);
    PGW_ASSERT_EQ_U(first.size, 5);
    PGW_ASSERT_EQ_U(first.mtime_ns / NS_PER_SEC, (uint64_t)OLD_MTIME_SEC);

    PGW_ASSERT(cas_host_write(v->root, "c.new", "second-and-longer", 17));
    PGW_ASSERT(cas_host_set_mtime(v->root, "c.new", NEW_MTIME_SEC));
    PGW_ASSERT(cas_host_rename(v->root, "c.new", "c.txt"));

    /* The data path is never cached: the new content is there at once. */
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(v->fs, &v->cred, "c.txt", PGW_O_RDONLY, 0, &fd),
                    PGW_OK);
    read_is(v, fd, "second-and-longer");
    PGW_ASSERT_EQ_I(pgw_rmfs_close(v->fs, fd), PGW_OK);

    /* Once the entry has expired, the attributes are the new ones. */
    PGW_ASSERT(pgw_time_advance(CACHE_TTL_NS + 1));
    pgw_fs_attr after;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(v->fs, &v->cred, "c.txt", &after), PGW_OK);
    PGW_ASSERT_EQ_U(after.size, 17);
    PGW_ASSERT_EQ_U(after.mtime_ns / NS_PER_SEC, (uint64_t)NEW_MTIME_SEC);
    PGW_ASSERT(after.ino != first.ino);
    pgw_time_set_clock(NULL, NULL);
}

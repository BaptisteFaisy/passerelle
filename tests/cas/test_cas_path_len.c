/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_cas_path_len.c - the maximum path length of spec 7.4 (gap 66).
 *
 * Path length is where a client and the truth diverge most often: one side
 * truncates a component, the other answers ENOENT for a path it shortened, and
 * the caller believes the entry does not exist. Every case here therefore asks
 * *both* sides and requires the same answer:
 *
 *   - a path whose absolute length is exactly PATH_MAX - 1 is accepted;
 *   - the same path with one more byte is refused with ENAMETOOLONG, and
 *     nothing is created half way;
 *   - a component of exactly NAME_MAX bytes is accepted, one byte more is
 *     refused with ENAMETOOLONG;
 *   - a deeply nested path is created, listed and read back byte for byte, and
 *     a path that differs only in its last byte is ENOENT (the detector of a
 *     silent truncation);
 *   - the same deep path survives the 9P adapter, whose walk is split into
 *     several Twalk messages (PGW_MAXWELEM, P9-08).
 *
 * Covers FS-15, PERF-04, P9-08 (gap 66 of docs/gap-analysis.md).
 */
#define _POSIX_C_SOURCE 200809L
#include "cas.h"
#include "cas_9p.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define CHAIN_COMP 100u /* bytes in one component of the chain */

/* Depth of the chain driven through the 9P client: 20 levels by default, i.e.
 * a walk longer than the PGW_MAXWELEM (16) names of one Twalk, which the
 * session must split (P9-08). Every operation re-walks its whole path from the
 * session root, so the cost grows with the square of the depth; measured here,
 * 0.34 s for 20 levels and 0.82 s for 40. The DISABLED long variant goes to 40
 * levels, which also brings the absolute path close to PATH_MAX. */
static size_t deep_levels(void)
{
    return (size_t)cas_env_u64("PGW_CAS_DEEP_LEVELS", 20);
}

/* A component of exactly `len` bytes, distinct for every index so two levels
 * of a chain never collide. */
static void cas_component(char *out, size_t len, size_t i)
{
    snprintf(out, len + 1, "%02zu", i % 100u);
    size_t used = strlen(out);
    memset(out + used, 'a', len - used);
    out[len] = '\0';
}

/* "<a><b>" with an explicit bound check: the limit cases build paths that a
 * format string would refuse to concatenate. */
static void cas_append(char *out, size_t cap, const char *a, const char *b)
{
    const size_t na = strlen(a);
    const size_t nb = strlen(b);
    PGW_ASSERT(na + nb + 1 <= cap);
    memcpy(out, a, na);
    memcpy(out + na, b, nb + 1);
}

/* Build `n` components of `comp` bytes on the truth side; `rel` receives the
 * relative path of the deepest directory, trailing slash included. */
static size_t chain_on_truth(const char *root, char *rel, size_t cap, size_t comp,
                             size_t n)
{
    char one[PGW_NAME_MAX + 1];
    char dir[CAS_PATH_MAX];
    size_t used = 0;
    rel[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        cas_component(one, comp, i);
        int k = snprintf(rel + used, cap - used, "%s/", one);
        PGW_ASSERT(k > 0 && (size_t)k < cap - used);
        used += (size_t)k;
        snprintf(dir, sizeof(dir), "%.*s", (int)(used - 1), rel);
        PGW_ASSERT_EQ_I(cas_truth_mkdir_errno(root, dir, 0700), 0);
    }
    return used;
}

/* The longest relative path the kernel still accepts under this root: the
 * absolute path is root + '/' + rel and must stay under PATH_MAX bytes, the
 * terminator included. */
static size_t rel_budget(const cas_mount *m)
{
    const size_t root_len = strlen(m->root);
    PGW_ASSERT(root_len + 2 < (size_t)PATH_MAX);
    return (size_t)PATH_MAX - 1u - root_len - 1u;
}

/* mkdir on both sides and require the same verdict. The layer answers first
 * (a mkdir is not idempotent), then the kernel is asked the same question:
 * whatever it says is the only expectation, nothing is invented here. */
static void expect_mkdir_agrees(const cas_mount *m, const char *rel)
{
    const pgw_err got = pgw_rmfs_mkdir(m->fs, &m->cred, rel, 0700);
    if (got == PGW_OK) {
        CAS_ASSERT_MSG(cas_truth_exists(m->root, rel),
                       "the layer created a %zu byte path the truth does not "
                       "hold",
                       strlen(rel));
        return;
    }
    const int truth = cas_truth_mkdir_errno(m->root, rel, 0700);
    if (got == PGW_EEXIST) {
        CAS_ASSERT_MSG(truth == EEXIST,
                       "the layer answered EEXIST for a %zu byte path, the "
                       "truth answered errno %d",
                       strlen(rel), truth);
        return;
    }
    CAS_ASSERT_MSG(truth == ENAMETOOLONG,
                   "the truth refused a %zu byte path with errno %d, expected "
                   "ENAMETOOLONG",
                   strlen(rel), truth);
    CAS_ASSERT_MSG(got == PGW_ENAMETOOLONG,
                   "the truth refused a %zu byte path with ENAMETOOLONG, the "
                   "layer answered %s",
                   strlen(rel), pgw_err_name(got));
    PGW_ASSERT_FALSE(cas_truth_exists(m->root, rel));
}

/* FS-15 (gap 66): a path of exactly PATH_MAX - 1 absolute bytes is accepted by
 * the kernel and by the layer; one byte more is refused by both. */
static void test_path_at_the_limit_and_one_over(void)
{
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-pathmax", NULL, NULL));
    const size_t budget = rel_budget(&m);
    const size_t levels = (budget - 1u) / (CHAIN_COMP + 1u);

    char rel[CAS_PATH_MAX];
    size_t used = chain_on_truth(m.root, rel, sizeof(rel), CHAIN_COMP, levels);
    const size_t tail = budget - used; /* 1..CHAIN_COMP bytes: the last name */
    CAS_ASSERT_MSG(tail >= 1 && tail <= PGW_NAME_MAX,
                   "the last component would be %zu bytes", tail);
    cas_component(rel + used, tail, levels);
    rel[used + tail] = '\0';

    CAS_ASSERT_MSG(strlen(m.root) + 1 + strlen(rel) == (size_t)PATH_MAX - 1,
                   "the case is not at the limit: %zu absolute bytes",
                   strlen(m.root) + 1 + strlen(rel));
    expect_mkdir_agrees(&m, rel);

    /* One byte more: the same path with a longer last component. */
    rel[used + tail] = 'b';
    rel[used + tail + 1] = '\0';
    expect_mkdir_agrees(&m, rel);
    cas_mount_close(&m);
}
PGW_TEST(test_path_at_the_limit_and_one_over);

/* FS-15 (gap 66): a single component of NAME_MAX bytes is a valid name, one of
 * NAME_MAX + 1 bytes is refused with ENAMETOOLONG by the kernel and by the
 * layer, at every depth. */
static void test_component_at_name_max(void)
{
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-namemax", NULL, NULL));
    char rel[CAS_PATH_MAX];
    chain_on_truth(m.root, rel, sizeof(rel), CHAIN_COMP, 2);

    char one[PGW_NAME_MAX + 2];
    cas_component(one, PGW_NAME_MAX, 7); /* 255 bytes */
    snprintf(rel + strlen(rel), sizeof(rel) - strlen(rel), "%s", one);
    expect_mkdir_agrees(&m, rel);

    one[PGW_NAME_MAX] = 'z'; /* 256 bytes */
    one[PGW_NAME_MAX + 1] = '\0';
    char long_rel[CAS_PATH_MAX];
    snprintf(long_rel, sizeof(long_rel), "d/%s", one);
    PGW_ASSERT_EQ_I(cas_truth_mkdir_errno(m.root, "d", 0700), 0);
    expect_mkdir_agrees(&m, long_rel);

    /* The same over-long name is refused by the create path too, and by the
     * read path: no layer may answer ENOENT for a name it shortened. */
    pgw_rmfs_fd *fd = NULL;
    pgw_err got =
            pgw_rmfs_open(m.fs, &m.cred, long_rel, PGW_O_RDWR | PGW_O_CREAT, 0644, &fd);
    CAS_ASSERT_MSG(got == PGW_ENAMETOOLONG, "create of a 256 byte name answered %s",
                   pgw_err_name(got));
    pgw_fs_attr at;
    got = pgw_rmfs_stat(m.fs, &m.cred, long_rel, &at);
    CAS_ASSERT_MSG(got == PGW_ENAMETOOLONG, "stat of a 256 byte name answered %s",
                   pgw_err_name(got));
    cas_mount_close(&m);
}
PGW_TEST(test_component_at_name_max);

/* FS-15, FS-03 (gap 66): a deeply nested path is created through the layer,
 * listed back byte for byte, and a path that differs only in its last byte is
 * ENOENT - the detector of a silent truncation. */
static void test_deep_chain_is_not_truncated(void)
{
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-deep", NULL, NULL));
    const size_t levels = 20;
    char rel[CAS_PATH_MAX];
    size_t used = chain_on_truth(m.root, rel, sizeof(rel), CHAIN_COMP, levels);
    printf("# deep path: %zu levels, %zu relative bytes\n", levels, used);

    /* Create the leaf through the layer, then read it back through the layer
     * and compare with the truth. */
    char leaf[CAS_PATH_MAX];
    cas_append(leaf, sizeof(leaf), rel, "leaf.txt");
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(
            pgw_rmfs_open(m.fs, &m.cred, leaf, PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
            PGW_OK);
    size_t w = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_write(m.fs, fd, 0, "deep", 4, &w), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, fd), PGW_OK);
    PGW_ASSERT(cas_truth_exists(m.root, leaf));
    PGW_ASSERT_EQ_U(cas_truth_size(m.root, leaf), 4);

    /* The listing of the parent reports the exact name, not a shortened one. */
    char dir[CAS_PATH_MAX];
    snprintf(dir, sizeof(dir), "%.*s", (int)(used - 1), rel);
    pgw_rmfs_fd *dh = NULL;
    PGW_ASSERT_EQ_I(
            pgw_rmfs_open(m.fs, &m.cred, dir, PGW_O_RDONLY | PGW_O_DIRECTORY, 0, &dh),
            PGW_OK);
    pgw_fs_dirent ents[8];
    size_t got = 0;
    uint64_t next = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_readdir(m.fs, dh, 0, ents, 8, &got, &next), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, dh), PGW_OK);
    PGW_ASSERT_EQ_U(got, 1);
    PGW_ASSERT_STR_EQ(ents[0].name, "leaf.txt");

    /* One byte changed in the last component: ENOENT, never the neighbour. */
    char other[CAS_PATH_MAX];
    cas_append(other, sizeof(other), rel, "leaf.txu");
    pgw_fs_attr at;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(m.fs, &m.cred, other, &at), PGW_ENOENT);
    PGW_ASSERT_EQ_I(cas_truth_stat_errno(m.root, other), ENOENT);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(m.fs, &m.cred, leaf, &at), PGW_OK);
    PGW_ASSERT_EQ_U(at.size, 4);
    cas_mount_close(&m);
}
PGW_TEST(test_deep_chain_is_not_truncated);

/* FS-15, P9-08 (gap 66): the same deep path through the 9P adapter, where a
 * walk longer than PGW_MAXWELEM names is split into several messages. */

/* Build `levels` components of CHAIN_COMP bytes through the layer itself, so
 * every mkdir is a split walk; `rel` receives the deepest directory. */
static void chain_through_client(pgw_rmfs *fs, const pgw_fs_cred *cred, size_t levels,
                                 char *rel, size_t cap)
{
    char one[PGW_NAME_MAX + 1];
    size_t used = 0;
    rel[0] = '\0';
    for (size_t i = 0; i < levels; i++) {
        cas_component(one, CHAIN_COMP, i);
        snprintf(rel + used, cap - used, "%s", one);
        used += CHAIN_COMP;
        PGW_ASSERT_EQ_I(pgw_rmfs_mkdir(fs, cred, rel, 0755), PGW_OK);
        snprintf(rel + used, cap - used, "/");
        used += 1;
    }
}

/* Create a leaf at the end of a deep path through the client and require that
 * the served directory holds exactly those bytes with exactly that content. */
static void leaf_through_client(const cas_9p *c, const char *rel)
{
    char leaf[CAS_PATH_MAX];
    cas_append(leaf, sizeof(leaf), rel, "leaf.txt");
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(
            pgw_rmfs_open(c->fs, &c->cred, leaf, PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
            PGW_OK);
    size_t w = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_write(c->fs, fd, 0, "deep over 9P", 12, &w), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(c->fs, fd), PGW_OK);

    PGW_ASSERT(cas_truth_exists(c->root, leaf));
    PGW_ASSERT_EQ_U(cas_truth_size(c->root, leaf), 12);
    char buf[16];
    size_t len = 0;
    PGW_ASSERT(cas_truth_read(c->root, leaf, buf, sizeof(buf), &len));
    PGW_ASSERT_EQ_U(len, 12);
    PGW_ASSERT_MEM_EQ(buf, "deep over 9P", 12);
}

static void test_deep_chain_through_9p(void)
{
    cas_9p c;
    PGW_ASSERT(cas_9p_open(&c, "pgwcas-deep9p", NULL));
    const size_t levels = deep_levels();
    char rel[CAS_PATH_MAX];
    chain_through_client(c.fs, &c.cred, levels, rel, sizeof(rel));
    printf("# deep path over 9P: %zu levels, %zu relative bytes\n", levels, strlen(rel));
    leaf_through_client(&c, rel);

    /* An over-long name is refused on this side of the wire as well. */
    char too_long[PGW_NAME_MAX + 2];
    cas_component(too_long, PGW_NAME_MAX + 1, 3);
    /* Wide enough for the deepest chain of the long variant plus the over-long
     * name appended to it. */
    char bad[CAS_PATH_MAX + PGW_NAME_MAX + 2];
    cas_append(bad, sizeof(bad), rel, too_long);
    pgw_fs_attr at;
    CAS_ASSERT_MSG(pgw_rmfs_stat(c.fs, &c.cred, bad, &at) == PGW_ENAMETOOLONG,
                   "a 256 byte name over 9P was not refused with "
                   "ENAMETOOLONG");
    cas_9p_close(&c);
}

PGW_TEST(test_deep_chain_through_9p);

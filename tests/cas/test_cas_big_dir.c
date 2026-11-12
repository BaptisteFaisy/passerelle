/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_cas_big_dir.c - the 100 000-entry directory of spec 7.4 (gap 65).
 *
 * A big directory is where a listing silently loses entries: the page size,
 * the buffer boundaries, the cookie the client must pass back and the two
 * pseudo entries "." and ".." are all places where a client and the truth
 * drift apart. These cases therefore never trust the layer under test: the
 * entries are created on the truth side (the PC writes, the guest reads), the
 * client lists them through the layer, and both name sets are compared entry
 * by entry.
 *
 * The readdir path is exercised through *both* backends: the POSIX truth
 * backend and the 9P adapter in front of the reference server, because the
 * adapter is what a real client sees (PERF-04).
 *
 * Scale from PGW_CAS_DIR_ENTRIES: 5 000 in a pull request, 100 000 in the
 * DISABLED long campaign (CI budget, 5 minutes per job).
 */
#define _POSIX_C_SOURCE 200809L
#include "cas.h"
#include "cas_9p.h"
#include "ref9p_truth.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAS_DIR_ENV "PGW_CAS_DIR_ENTRIES"
#define CAS_DIR_DEFAULT 5000u /* pull-request variant of the 100 000 of 7.4 */
#define DIR_NAME "big"
#define NAME_CAP 256

static size_t dir_entries(void)
{
    return (size_t)cas_env_u64(CAS_DIR_ENV, CAS_DIR_DEFAULT);
}

/* Build the tree on the truth side: one file in ten is a directory, so the
 * listing carries both entry types. */
static void make_entries(const char *root, size_t n)
{
    PGW_ASSERT(ref9p_truth_mkdir(root, DIR_NAME, 0755));
    char rel[64];
    for (size_t i = 0; i < n; i++) {
        snprintf(rel, sizeof(rel), DIR_NAME "/e%06zu", i);
        if (i % 10u == 9u)
            PGW_ASSERT(ref9p_truth_mkdir(root, rel, 0755));
        else
            PGW_ASSERT(ref9p_truth_write(root, rel, "x", 1));
    }
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* A listing buffer of `n` names; NULL aborts the case. */
static char (*names_alloc(size_t n))[NAME_CAP]
{
    char (*p)[NAME_CAP] = calloc(n ? n : 1, NAME_CAP);
    PGW_ASSERT(p != NULL);
    return p;
}

/* List the whole directory through the layer, `page` entries at a time.
 * Returns the number of entries collected and reports the number of round
 * trips and the largest page the layer actually handed back. */
static size_t list_all(pgw_rmfs *fs, const pgw_fs_cred *cred, size_t page,
                       char (*names)[NAME_CAP], size_t cap, size_t *pages,
                       size_t *max_page)
{
    pgw_rmfs_fd *dir = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, cred, DIR_NAME,
                                  PGW_O_RDONLY | PGW_O_DIRECTORY, 0, &dir),
                    PGW_OK);
    pgw_fs_dirent *ents = calloc(page, sizeof(*ents));
    PGW_ASSERT(ents != NULL);

    size_t seen = 0;
    uint64_t off = 0;
    *pages = 0;
    *max_page = 0;
    for (;;) {
        size_t got = 0;
        uint64_t next = 0;
        PGW_ASSERT_EQ_I(pgw_rmfs_readdir(fs, dir, off, ents, page, &got, &next),
                        PGW_OK);
        if (got == 0)
            break;
        (*pages)++;
        if (got > *max_page)
            *max_page = got;
        for (size_t i = 0; i < got; i++) {
            CAS_ASSERT_MSG(seen < cap,
                           "the client listed more than the %zu entries of the "
                           "truth (extra: \"%s\")",
                           cap, ents[i].name);
            snprintf(names[seen++], NAME_CAP, "%s", ents[i].name);
        }
        if (next == 0)
            break; /* the 9P adapter answers "exhausted" with a zero cookie */
        CAS_ASSERT_MSG(next > off,
                       "the listing does not advance: off=%llu next=%llu",
                       (unsigned long long)off, (unsigned long long)next);
        off = next;
    }
    free(ents);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, dir), PGW_OK);
    return seen;
}

/* Compare a listing with the truth, name by name. */
static void expect_names_match(const char *root, char (*got)[NAME_CAP],
                               size_t seen, size_t n)
{
    char (*want)[NAME_CAP] = names_alloc(n);
    size_t truth = ref9p_truth_names(root, DIR_NAME, want, n);
    CAS_ASSERT_MSG(truth == n, "the truth holds %zu entries, expected %zu",
                   truth, n);
    CAS_ASSERT_MSG(seen == n,
                   "the client listed %zu entries, the truth holds %zu", seen,
                   n);
    qsort(got, n, NAME_CAP, cmp_names);
    qsort(want, n, NAME_CAP, cmp_names);
    for (size_t i = 0; i < n; i++)
        CAS_ASSERT_MSG(strcmp(got[i], want[i]) == 0,
                       "entry %zu differs: the client says \"%s\", the truth "
                       "says \"%s\"",
                       i, got[i], want[i]);
    free(want);
}

/* PERF-04 (gap 65): every entry of a big directory reaches the client, and
 * the count the client reports is the count the truth holds. */
static void test_big_dir_client_count_matches_truth(void)
{
    const size_t n = dir_entries();
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-bigdir", NULL, NULL));
    make_entries(m.root, n);
    PGW_ASSERT_EQ_U(cas_truth_count(m.root, DIR_NAME), n);

    char (*got)[NAME_CAP] = names_alloc(n);
    size_t pages = 0;
    size_t max_page = 0;
    size_t seen = list_all(m.fs, &m.cred, 256, got, n, &pages, &max_page);
    printf("# big directory: %zu entries listed in %zu pages (largest %zu)\n",
           seen, pages, max_page);
    CAS_ASSERT_MSG(max_page <= 256, "the layer handed back %zu entries for a "
                   "page of 256", max_page);
    expect_names_match(m.root, got, seen, n);
    free(got);
    cas_mount_close(&m);
}
PGW_TEST(test_big_dir_client_count_matches_truth);

/* FS-03 (gap 65): an odd page size that does not divide the entry count loses
 * nothing and duplicates nothing; the cookie always moves forward. */
static void test_big_dir_pagination_loses_nothing(void)
{
    const size_t n = dir_entries();
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-bigpage", NULL, NULL));
    make_entries(m.root, n);

    char (*got)[NAME_CAP] = names_alloc(n);
    size_t pages = 0;
    size_t max_page = 0;
    size_t seen = list_all(m.fs, &m.cred, 37, got, n, &pages, &max_page);
    CAS_ASSERT_MSG(seen == n, "paging by 37 listed %zu of %zu entries", seen, n);
    CAS_ASSERT_MSG(max_page <= 37, "a page of 37 returned %zu entries",
                   max_page);
    CAS_ASSERT_MSG(pages * 37 >= n, "%zu pages of 37 cannot hold %zu entries",
                   pages, n);

    /* No duplicate: a sorted listing must never repeat a name. */
    qsort(got, n, NAME_CAP, cmp_names);
    for (size_t i = 1; i < n; i++)
        CAS_ASSERT_MSG(strcmp(got[i - 1], got[i]) != 0,
                       "\"%s\" was listed twice", got[i]);
    free(got);
    cas_mount_close(&m);
}
PGW_TEST(test_big_dir_pagination_loses_nothing);

/* FS-03 (gap 65): "." and ".." are never reported as entries of the listing,
 * while the raw POSIX layer does report them: the client sees the real entries
 * only, and the count is the count of real entries. */
static void test_big_dir_pseudo_entries_not_reported(void)
{
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-dotdot", NULL, NULL));
    PGW_ASSERT(ref9p_truth_mkdir(m.root, DIR_NAME, 0755));
    PGW_ASSERT(ref9p_truth_write(m.root, DIR_NAME "/a", "x", 1));
    PGW_ASSERT(ref9p_truth_write(m.root, DIR_NAME "/b", "x", 1));
    PGW_ASSERT(ref9p_truth_mkdir(m.root, DIR_NAME "/c", 0755));

    /* The truth, read the raw way, reports five names. */
    char abs[512];
    cas_mount_abs(&m, DIR_NAME, abs, sizeof(abs));
    DIR *d = opendir(abs);
    PGW_ASSERT(d != NULL);
    size_t raw = 0;
    bool saw_dot = false;
    bool saw_dotdot = false;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        raw++;
        saw_dot = saw_dot || strcmp(de->d_name, ".") == 0;
        saw_dotdot = saw_dotdot || strcmp(de->d_name, "..") == 0;
    }
    closedir(d);
    PGW_ASSERT(saw_dot && saw_dotdot);
    PGW_ASSERT_EQ_U(raw, 5);

    /* The client reports the three real entries and neither pseudo entry. */
    char (*got)[NAME_CAP] = names_alloc(8);
    size_t pages = 0;
    size_t max_page = 0;
    size_t seen = list_all(m.fs, &m.cred, 4, got, 8, &pages, &max_page);
    CAS_ASSERT_MSG(seen == 3, "the client listed %zu entries, expected 3",
                   seen);
    for (size_t i = 0; i < seen; i++)
        CAS_ASSERT_MSG(strcmp(got[i], ".") != 0 && strcmp(got[i], "..") != 0,
                       "the listing reports the pseudo entry \"%s\"", got[i]);
    PGW_ASSERT_EQ_U(cas_truth_count(m.root, DIR_NAME), 3);
    free(got);
    cas_mount_close(&m);
}
PGW_TEST(test_big_dir_pseudo_entries_not_reported);

/* PERF-04 (gap 65): the same listing through the 9P adapter, i.e. what a real
 * client sees, entry types included. */
static void test_big_dir_client_through_9p(void)
{
    const size_t n = dir_entries();
    cas_9p c;
    PGW_ASSERT(cas_9p_open(&c, "pgwcas-bigdir9p", NULL));
    make_entries(c.root, n);

    char (*got)[NAME_CAP] = names_alloc(n);
    size_t pages = 0;
    size_t max_page = 0;
    size_t seen = list_all(c.fs, &c.cred, 64, got, n, &pages, &max_page);
    printf("# big directory over 9P: %zu entries in %zu pages\n", seen, pages);
    expect_names_match(c.root, got, seen, n);

    /* The type of an entry survives the wire: one file, one directory. */
    pgw_fs_attr at;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(c.fs, &c.cred, DIR_NAME "/e000000", &at),
                    PGW_OK);
    PGW_ASSERT_EQ_I(at.type, PGW_FT_REG);
    PGW_ASSERT_EQ_U(at.size, 1);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(c.fs, &c.cred, DIR_NAME "/e000009", &at),
                    PGW_OK);
    PGW_ASSERT_EQ_I(at.type, PGW_FT_DIR);
    free(got);
    cas_9p_close(&c);
}
PGW_TEST(test_big_dir_client_through_9p);

/* FS-02, FS-03 (gap 65): entries created *through* the layer appear in the
 * listing with the right name and the right type. */
static void test_big_dir_entries_created_through_layer(void)
{
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-bigmake", NULL, NULL));
    PGW_ASSERT_EQ_I(pgw_rmfs_mkdir(m.fs, &m.cred, DIR_NAME, 0755), PGW_OK);
    const size_t n = 200;
    char rel[64];
    for (size_t i = 0; i < n; i++) {
        snprintf(rel, sizeof(rel), DIR_NAME "/e%06zu", i);
        if (i % 4u == 3u) {
            PGW_ASSERT_EQ_I(pgw_rmfs_mkdir(m.fs, &m.cred, rel, 0755), PGW_OK);
            continue;
        }
        pgw_rmfs_fd *fd = NULL;
        PGW_ASSERT_EQ_I(pgw_rmfs_open(m.fs, &m.cred, rel,
                                      PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                        PGW_OK);
        PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, fd), PGW_OK);
    }
    PGW_ASSERT_EQ_U(cas_truth_count(m.root, DIR_NAME), n);

    char (*got)[NAME_CAP] = names_alloc(n);
    size_t pages = 0;
    size_t max_page = 0;
    size_t seen = list_all(m.fs, &m.cred, 16, got, n, &pages, &max_page);
    expect_names_match(m.root, got, seen, n);
    free(got);
    cas_mount_close(&m);
}
PGW_TEST(test_big_dir_entries_created_through_layer);

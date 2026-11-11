/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * diff_verify_tree.c - walk the truth and check the whole reported tree.
 *
 * Two directions are checked for every directory (FS-03): nothing the PC holds
 * may be missing from the share, and the share may not invent an entry the PC
 * does not hold. Files are compared field by field by diff_verify_path().
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
#include "diff.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/rmfs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TREE_MAX_ENTRIES 4096
#define NAME_MAX_LEN 256
#define PATH_BUF 4096

typedef struct name_set {
    char (*names)[NAME_MAX_LEN];
    size_t n;
} name_set;

static int name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static name_set *set_alloc(void)
{
    name_set *s = calloc(1, sizeof(*s));
    if (s == NULL)
        return NULL;
    s->names = calloc(TREE_MAX_ENTRIES, NAME_MAX_LEN);
    if (s->names == NULL) {
        free(s);
        return NULL;
    }
    return s;
}

static void set_free(name_set *s)
{
    if (s == NULL)
        return;
    free(s->names);
    free(s);
}

static bool set_add(name_set *s, const char *name)
{
    if (s->n >= TREE_MAX_ENTRIES)
        return false;
    snprintf(s->names[s->n], NAME_MAX_LEN, "%s", name);
    s->n++;
    return true;
}

static bool set_has(const name_set *s, const char *name)
{
    for (size_t i = 0; i < s->n; i++) {
        if (strcmp(s->names[i], name) == 0)
            return true;
    }
    return false;
}

static void set_sort(name_set *s)
{
    qsort(s->names, s->n, NAME_MAX_LEN, name_cmp);
}

/* Read a directory through rmfs and collect the names it reports. */
static int rmfs_listing(pgw_rmfs *fs, const char *rel, name_set *out, char *err,
                        size_t cap)
{
    pgw_fs_cred cred = { 0, 0, 10 };
    pgw_rmfs_fd *fd = NULL;
    pgw_err e = pgw_rmfs_open(fs, &cred, rel, PGW_O_RDONLY | PGW_O_DIRECTORY, 0,
                              &fd);
    if (e != PGW_OK || fd == NULL) {
        snprintf(err, cap, "%s: rmfs opendir: %s", rel, pgw_err_name(e));
        return -1;
    }
    static pgw_fs_dirent ents[64];
    uint64_t off = 0;
    for (;;) {
        size_t got = 0;
        uint64_t next = 0;
        e = pgw_rmfs_readdir(fs, fd, off, ents, 64, &got, &next);
        if (e != PGW_OK) {
            snprintf(err, cap, "%s: rmfs readdir@%llu: %s", rel,
                     (unsigned long long)off, pgw_err_name(e));
            pgw_rmfs_close(fs, fd);
            return -1;
        }
        for (size_t i = 0; i < got; i++)
            set_add(out, ents[i].name);
        if (got == 0 || next <= off)
            break;
        off = next;
    }
    pgw_rmfs_close(fs, fd);
    return 0;
}

static int truth_listing(const char *abs, name_set *out, char *err, size_t cap)
{
    DIR *d = opendir(abs);
    if (d == NULL) {
        snprintf(err, cap, "%s: truth opendir: %s", abs, strerror(errno));
        return -1;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        set_add(out, de->d_name);
    }
    closedir(d);
    return 0;
}

/* Explicit concatenation: snprintf's truncation warning is noise here, and an
 * explicit bound documents the real constraint (the path must fit). */
static bool join_path(const char *dir, const char *name, char *out, size_t cap)
{
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    if (dl + 1u + nl + 1u > cap)
        return false;
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1u, name, nl);
    out[dl + 1u + nl] = '\0';
    return true;
}

/* Relative path of a child of `rel` ("." for the root). */
static bool join_rel(const char *rel, const char *name, char *out, size_t cap)
{
    size_t rl = strlen(rel);
    size_t nl = strlen(name);
    bool root = (rl == 0);
    size_t need = rl + (root ? 0u : 1u) + nl + 1u;
    if (need > cap)
        return false;
    memcpy(out, rel, rl);
    if (!root)
        out[rl] = '/';
    memcpy(out + rl + (root ? 0u : 1u), name, nl);
    out[need - 1u] = '\0';
    return true;
}

static int compare_listing(pgw_rmfs *fs, const char *root, const char *rel,
                           char *err, size_t cap)
{
    char abs[PATH_BUF + NAME_MAX_LEN + 2];
    if (!join_path(root, rel[0] ? rel : ".", abs, sizeof(abs))) {
        snprintf(err, cap, "%s: path too long", rel);
        return -1;
    }
    /* A directory the suite itself made unreadable (chmod 000) cannot be listed
     * on either side; the per-entry attribute check still covers it, so skip the
     * listing comparison instead of reporting a false divergence. */
    if (access(abs, R_OK) != 0)
        return 0;
    name_set *truth = set_alloc();
    name_set *got = set_alloc();
    if (truth == NULL || got == NULL) {
        set_free(truth);
        set_free(got);
        snprintf(err, cap, "%s: out of memory", rel);
        return -1;
    }
    int rc = truth_listing(abs, truth, err, cap);
    if (rc == 0)
        rc = rmfs_listing(fs, rel[0] ? rel : ".", got, err, cap);
    if (rc != 0) {
        set_free(truth);
        set_free(got);
        return rc;
    }
    set_sort(truth);
    set_sort(got);
    rc = 0;
    for (size_t i = 0; i < truth->n && rc == 0; i++) {
        if (!set_has(got, truth->names[i])) {
            snprintf(err, cap, "%s: entry \"%s\" present on the PC, missing "
                               "in the share", rel, truth->names[i]);
            rc = -1;
        }
    }
    for (size_t i = 0; i < got->n && rc == 0; i++) {
        if (!set_has(truth, got->names[i])) {
            snprintf(err, cap, "%s: entry \"%s\" reported by the share, absent "
                               "on the PC", rel, got->names[i]);
            rc = -1;
        }
    }
    set_free(truth);
    set_free(got);
    return rc;
}

static int walk(pgw_rmfs *fs, const char *root, const char *rel, char *err,
                size_t cap)
{
    if (compare_listing(fs, root, rel, err, cap) != 0)
        return -1;
    char abs[PATH_BUF + NAME_MAX_LEN + 2];
    if (!join_path(root, rel[0] ? rel : ".", abs, sizeof(abs))) {
        snprintf(err, cap, "%s: path too long", rel);
        return -1;
    }
    DIR *d = opendir(abs);
    if (d == NULL)
        return 0;
    struct dirent *de;
    int rc = 0;
    while ((de = readdir(d)) != NULL && rc == 0) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char child[PATH_BUF + NAME_MAX_LEN + 2];
        if (!join_rel(rel, de->d_name, child, sizeof(child)))
            continue;
        if (diff_verify_path(fs, root, child, err, cap) != 0) {
            rc = -1;
            break;
        }
        struct stat st;
        char child_abs[PATH_BUF + NAME_MAX_LEN + 2];
        if (!join_path(abs, de->d_name, child_abs, sizeof(child_abs)))
            continue;
        if (lstat(child_abs, &st) == 0 && S_ISDIR(st.st_mode))
            rc = walk(fs, root, child, err, cap);
    }
    closedir(d);
    return rc;
}

int diff_verify_tree(pgw_rmfs *fs, const char *root, char *err, size_t err_cap)
{
    return walk(fs, root, "", err, err_cap);
}

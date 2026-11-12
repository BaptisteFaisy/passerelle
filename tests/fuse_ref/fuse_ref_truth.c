/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_ref_truth.c - the oracle of the reference-server tests: read the served
 * directory directly, with plain POSIX calls, so that whatever the client
 * reports can be compared against the truth on disk.
 *
 * Implements requirements:
 *   - VFS-03: attributes, content and directory listings returned by the client
 *     are checked against the real folder, not against another view of the same
 *     in-memory state
 *   - FS-03: the names and d_type of a READDIR answer are the real ones
 *
 * Shared by every test file of tests/fuse_ref, so the comparison logic exists
 * exactly once.
 */
#define _POSIX_C_SOURCE 200809L
#include "fuse_ref_harness.h"

#include "pgw/test.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FUSE_REF_DIRENT_MAX 64u

void fuse_ref_join(const char *root, const char *name, char *out, size_t cap)
{
    if (cap == 0)
        return;
    size_t rlen = strlen(root);
    size_t nlen = strlen(name);
    if (rlen + nlen + 2u > cap) {
        out[0] = '\0'; /* does not fit: report an empty path, never a cut one */
        return;
    }
    memcpy(out, root, rlen);
    out[rlen] = '/';
    memcpy(out + rlen + 1u, name, nlen);
    out[rlen + nlen + 1u] = '\0';
}

void fuse_ref_copy_str(char *dst, size_t cap, const char *src)
{
    if (cap == 0)
        return;
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void fuse_ref_disk_path(const fuse_ref_ctx *c, const char *name, char *out, size_t cap)
{
    fuse_ref_join(c->root, name, out, cap);
}

bool fuse_ref_disk_lstat(const fuse_ref_ctx *c, const char *name, struct stat *st)
{
    char path[FUSE_REF_PATH_MAX];
    fuse_ref_disk_path(c, name, path, sizeof(path));
    return lstat(path, st) == 0;
}

bool fuse_ref_disk_read(const fuse_ref_ctx *c, const char *name, void *buf, size_t cap,
                        size_t *out_len)
{
    char path[FUSE_REF_PATH_MAX];
    fuse_ref_disk_path(c, name, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    size_t total = 0;
    while (total < cap) {
        ssize_t n = read(fd, (uint8_t *)buf + total, cap - total);
        if (n < 0) {
            close(fd);
            return false;
        }
        if (n == 0)
            break;
        total += (size_t)n;
    }
    close(fd);
    if (out_len != NULL)
        *out_len = total;
    return true;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

size_t fuse_ref_disk_names(const fuse_ref_ctx *c, const char *dir,
                           char names[][FUSE_REF_NAME_MAX], size_t max)
{
    char path[FUSE_REF_PATH_MAX];
    fuse_ref_disk_path(c, dir, path, sizeof(path));
    DIR *d = opendir(path);
    if (d == NULL)
        return 0;
    size_t n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < max) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        fuse_ref_copy_str(names[n], FUSE_REF_NAME_MAX, ent->d_name);
        n++;
    }
    closedir(d);
    qsort(names, n, FUSE_REF_NAME_MAX, cmp_str);
    return n;
}

size_t fuse_ref_client_names(fuse_ref_ctx *c, uint64_t nodeid,
                             char names[][FUSE_REF_NAME_MAX], size_t max)
{
    static pgw_fuse_dirent ents[FUSE_REF_DIRENT_MAX];
    uint64_t fh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_open(c->session, nodeid, PGW_O_RDONLY, true, &fh, 0),
                    PGW_OK);
    size_t total = 0;
    uint64_t off = 0;
    for (;;) {
        size_t got = 0;
        pgw_err rc = pgw_fuse_readdir(c->session, nodeid, fh, off, ents + total,
                                      FUSE_REF_DIRENT_MAX - total, &got, 0);
        PGW_ASSERT_EQ_I(rc, PGW_OK);
        if (got == 0 || total >= FUSE_REF_DIRENT_MAX)
            break;
        total += got;
        off = ents[total - 1u].off;
    }
    PGW_ASSERT_EQ_I(pgw_fuse_release(c->session, nodeid, fh, true, 0), PGW_OK);

    size_t n = 0;
    for (size_t i = 0; i < total && n < max; i++) {
        if (strcmp(ents[i].name, ".") == 0 || strcmp(ents[i].name, "..") == 0)
            continue;
        fuse_ref_copy_str(names[n], FUSE_REF_NAME_MAX, ents[i].name);
        n++;
    }
    qsort(names, n, FUSE_REF_NAME_MAX, cmp_str);
    return n;
}

void fuse_ref_assert_attr_matches(const fuse_ref_ctx *c, uint64_t nodeid,
                                  const char *name)
{
    struct stat st;
    memset(&st, 0, sizeof(st));
    PGW_ASSERT(fuse_ref_disk_lstat(c, name, &st));
    pgw_fuse_attr a;
    memset(&a, 0, sizeof(a));
    PGW_ASSERT_EQ_I(pgw_fuse_getattr(c->session, nodeid, &a, 0), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, (uint64_t)st.st_size);
    PGW_ASSERT_EQ_U(a.mode & 0177777u, (uint32_t)(st.st_mode & 0177777));
    PGW_ASSERT_EQ_U(a.nlink, (uint32_t)st.st_nlink);
    PGW_ASSERT_EQ_U(a.ino, (uint64_t)st.st_ino);
}

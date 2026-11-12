/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * backend_tree.c - POSIX backend: namespace operations, readdir, lifecycle.
 *
 * Covers the tree-side of pgw_fs_backend_ops (mkdir, unlink, rename, symlink,
 * readlink, link, readdir), the unified open() dispatch and the published
 * vtable. A directory handle snapshots its entries at open time, so readdir
 * offsets stay stable while the directory changes underneath (FS-03: no entry
 * lost, none duplicated, resume from an offset).
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "backend_priv.h"

#include "pgw/err.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* --- namespace operations ------------------------------------------------- */

static pgw_err be_tree_mkdir(void *impl, const char *relpath, uint32_t mode,
                             uint32_t prio)
{
    (void)prio;
    char path[PGW_BE_PATH_MAX];
    if (!pgw_posix_join((pgw_posix_backend *)impl, relpath, path, sizeof(path)))
        return PGW_ENAMETOOLONG;
    if (mkdir(path, (mode_t)(mode & PGW_S_PERM_MASK)) != 0)
        return pgw_code_from_host(errno);
    return PGW_OK;
}

static pgw_err be_tree_unlink(void *impl, const char *relpath, bool is_dir,
                              uint32_t prio)
{
    (void)prio;
    char path[PGW_BE_PATH_MAX];
    if (!pgw_posix_join((pgw_posix_backend *)impl, relpath, path, sizeof(path)))
        return PGW_ENAMETOOLONG;
    int rc = is_dir ? rmdir(path) : unlink(path);
    if (rc != 0)
        return pgw_code_from_host(errno);
    return PGW_OK;
}

static pgw_err be_tree_rename(void *impl, const char *oldpath,
                              const char *newpath, uint32_t prio)
{
    (void)prio;
    pgw_posix_backend *b = (pgw_posix_backend *)impl;
    char from[PGW_BE_PATH_MAX], to[PGW_BE_PATH_MAX];
    if (!pgw_posix_join(b, oldpath, from, sizeof(from)))
        return PGW_ENAMETOOLONG;
    if (!pgw_posix_join(b, newpath, to, sizeof(to)))
        return PGW_ENAMETOOLONG;
    if (rename(from, to) != 0)
        return pgw_code_from_host(errno);
    return PGW_OK;
}

static pgw_err be_tree_symlink(void *impl, const char *linkpath,
                               const char *target, uint32_t prio)
{
    (void)prio;
    char path[PGW_BE_PATH_MAX];
    if (!pgw_posix_join((pgw_posix_backend *)impl, linkpath, path, sizeof(path)))
        return PGW_ENAMETOOLONG;
    if (symlink(target, path) != 0)
        return pgw_code_from_host(errno);
    return PGW_OK;
}

static pgw_err be_tree_readlink(void *impl, const char *linkpath, char *target,
                                size_t cap, uint32_t prio)
{
    (void)prio;
    char path[PGW_BE_PATH_MAX];
    if (!pgw_posix_join((pgw_posix_backend *)impl, linkpath, path, sizeof(path)))
        return PGW_ENAMETOOLONG;
    if (cap == 0)
        return PGW_EINVAL;
    ssize_t n = readlink(path, target, cap - 1);
    if (n < 0)
        return pgw_code_from_host(errno);
    target[n] = '\0';
    return PGW_OK;
}

static pgw_err be_tree_link(void *impl, const char *existing,
                            const char *newpath, uint32_t prio)
{
    (void)prio;
    pgw_posix_backend *b = (pgw_posix_backend *)impl;
    char src[PGW_BE_PATH_MAX], dst[PGW_BE_PATH_MAX];
    if (!pgw_posix_join(b, existing, src, sizeof(src)))
        return PGW_ENAMETOOLONG;
    if (!pgw_posix_join(b, newpath, dst, sizeof(dst)))
        return PGW_ENAMETOOLONG;
    if (link(src, dst) != 0)
        return pgw_code_from_host(errno);
    return PGW_OK;
}

/* --- readdir with a stable snapshot --------------------------------------- */

/* Grow the snapshot to hold at least `need` entries. Returns false when the
 * ceiling is reached or the allocation fails: the caller then reports an error
 * instead of listing a truncated directory (gap 65). */
static bool snapshot_grow(be_handle *h, size_t need)
{
    size_t cap = (h->cap != 0) ? h->cap * 2 : PGW_BE_DIR_CHUNK;
    if (cap < need)
        cap = need;
    if (cap > PGW_BE_MAX_DIR_ENTRIES)
        return false;
    char (*names)[256] = realloc(h->names, cap * sizeof(*names));
    if (names == NULL)
        return false;
    h->names = names;
    uint8_t *types = realloc(h->types, cap * sizeof(*types));
    if (types == NULL)
        return false;
    h->types = types;
    h->cap = cap;
    return true;
}

static pgw_err snapshot_dir(be_handle *h, const char *path)
{
    DIR *d = opendir(path);
    if (d == NULL)
        return pgw_code_from_host(errno);
    struct dirent *de;
    pgw_err err = PGW_OK;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (h->n_entries == h->cap && !snapshot_grow(h, h->cap + 1)) {
            err = (h->cap >= PGW_BE_MAX_DIR_ENTRIES) ? PGW_EOVERFLOW
                                                     : PGW_ENOMEM;
            break;
        }
        size_t i = h->n_entries++;
        snprintf(h->names[i], sizeof(h->names[i]), "%s", de->d_name);
        h->types[i] = (uint8_t)de->d_type;
    }
    closedir(d);
    return err;
}

static pgw_err be_tree_readdir(void *impl, uint64_t handle, uint64_t off,
                               pgw_fs_dirent *out, size_t max_entries,
                               size_t *out_count, uint64_t *next_off,
                               uint32_t prio)
{
    (void)prio;
    be_handle *h = be_handle_get((pgw_posix_backend *)impl, handle);
    if (h == NULL || !h->is_dir)
        return PGW_ENOTDIR;
    size_t n = 0;
    while (off + n < (uint64_t)h->n_entries && n < max_entries) {
        size_t i = (size_t)(off + n);
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].name, sizeof(out[n].name), "%s", h->names[i]);
        out[n].type = pgw_ftype_from_dt(h->types[i]);
        out[n].next_off = (uint64_t)(i + 1);
        n++;
    }
    *out_count = n;
    *next_off = off + (uint64_t)n;
    return PGW_OK;
}

/* --- unified open --------------------------------------------------------- */

pgw_err be_tree_open(void *impl, const char *relpath, uint32_t pgw_oflags,
                     uint32_t create_mode, uint64_t *out_handle, uint32_t prio)
{
    (void)prio;
    pgw_posix_backend *b = (pgw_posix_backend *)impl;
    char path[PGW_BE_PATH_MAX];
    if (!pgw_posix_join(b, relpath, path, sizeof(path)))
        return PGW_ENAMETOOLONG;
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        be_handle *h = be_handle_alloc(b);
        if (h == NULL)
            return PGW_EMFILE;
        h->is_dir = true;
        snprintf(h->path, sizeof(h->path), "%s", path);
        pgw_err e = snapshot_dir(h, path);
        if (e != PGW_OK) {
            be_handle_free(h);
            return e;
        }
        *out_handle = (uint64_t)(h - b->handles) + 1;
        return PGW_OK;
    }
    return be_file_open_fd(b, relpath, pgw_oflags, create_mode, out_handle);
}

/* --- lifecycle and vtable ------------------------------------------------- */

static void be_tree_destroy(void *impl)
{
    pgw_posix_backend_close((pgw_posix_backend *)impl);
}

pgw_posix_backend *pgw_posix_backend_open(const char *root)
{
    struct stat st;
    if (root == NULL || stat(root, &st) != 0 || !S_ISDIR(st.st_mode))
        return NULL;
    pgw_posix_backend *b = calloc(1, sizeof(*b));
    if (b == NULL)
        return NULL;
    snprintf(b->root, sizeof(b->root), "%s", root);
    b->root_len = strlen(b->root);
    return b;
}

void pgw_posix_backend_close(pgw_posix_backend *b)
{
    if (b == NULL)
        return;
    for (size_t i = 0; i < PGW_BE_MAX_HANDLES; i++) {
        if (b->handles[i].in_use)
            be_handle_free(&b->handles[i]);
    }
    free(b);
}

const char *pgw_posix_backend_root(const pgw_posix_backend *b)
{
    return (b != NULL) ? b->root : NULL;
}

const pgw_fs_backend_ops pgw_posix_backend_ops = {
    .destroy = be_tree_destroy,
    .lookup = be_file_lookup,
    .getattr = be_file_getattr,
    .setattr = be_file_setattr,
    .open = be_tree_open,
    .close = be_file_close,
    .read = be_file_read,
    .write = be_file_write,
    .readdir = be_tree_readdir,
    .mkdir = be_tree_mkdir,
    .unlink = be_tree_unlink,
    .rename = be_tree_rename,
    .symlink = be_tree_symlink,
    .readlink = be_tree_readlink,
    .link = be_tree_link,
    .fsync = be_file_fsync,
    .statfs = be_file_statfs,
};

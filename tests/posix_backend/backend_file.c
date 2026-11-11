/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * backend_file.c - POSIX backend: handle table, attributes, file operations.
 *
 * Covers the file-side of pgw_fs_backend_ops: open (regular files), close,
 * read, write, fsync, lookup/getattr, setattr, statfs. Namespace operations
 * live in backend_tree.c. Errors go through pgw_code_from_host() so the layer
 * above always sees pgw_err and never a host errno (P9-04 by construction).
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "backend_priv.h"

#include "pgw/err.h"
#include "pgw/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* --- handle table --------------------------------------------------------- */

be_handle *be_handle_alloc(pgw_posix_backend *b)
{
    for (size_t i = 0; i < PGW_BE_MAX_HANDLES; i++) {
        if (!b->handles[i].in_use) {
            be_handle *h = &b->handles[i];
            memset(h, 0, sizeof(*h));
            h->in_use = true;
            h->fd = -1;
            return h;
        }
    }
    return NULL;
}

void be_handle_free(be_handle *h)
{
    if (h == NULL)
        return;
    if (h->fd >= 0)
        close(h->fd);
    free(h->names);
    free(h->types);
    memset(h, 0, sizeof(*h));
}

be_handle *be_handle_get(pgw_posix_backend *b, uint64_t handle)
{
    if (handle == 0 || handle > PGW_BE_MAX_HANDLES)
        return NULL;
    be_handle *h = &b->handles[handle - 1];
    return h->in_use ? h : NULL;
}

size_t pgw_posix_backend_open_handles(const pgw_posix_backend *b)
{
    size_t n = 0;
    for (size_t i = 0; i < PGW_BE_MAX_HANDLES; i++) {
        if (b->handles[i].in_use)
            n++;
    }
    return n;
}

/* --- paths and attributes ------------------------------------------------- */

bool pgw_posix_join(const pgw_posix_backend *b, const char *relpath,
                    char *out, size_t cap)
{
    if (b == NULL || relpath == NULL || out == NULL || cap == 0)
        return false;
    while (*relpath == '/')
        relpath++;
    if (*relpath == '\0')
        return snprintf(out, cap, "%s", b->root) < (int)cap;
    if (strstr(relpath, "..") != NULL)
        return false; /* never escape the share root */
    int n = snprintf(out, cap, "%s/%s", b->root, relpath);
    return n > 0 && (size_t)n < cap;
}

void be_fill_attr(const struct stat *st, pgw_fs_attr *out)
{
    memset(out, 0, sizeof(*out));
    out->type = pgw_ftype_from_host_mode((uint32_t)st->st_mode);
    out->mode = (uint32_t)st->st_mode & PGW_S_PERM_MASK;
    out->uid = (uint32_t)st->st_uid;
    out->gid = (uint32_t)st->st_gid;
    out->ino = (uint64_t)st->st_ino;
    out->size = (uint64_t)st->st_size;
    out->nlink = (uint64_t)st->st_nlink;
    out->atime_ns = (uint64_t)st->st_atim.tv_sec * 1000000000ULL +
                    (uint64_t)st->st_atim.tv_nsec;
    out->mtime_ns = (uint64_t)st->st_mtim.tv_sec * 1000000000ULL +
                    (uint64_t)st->st_mtim.tv_nsec;
    out->ctime_ns = (uint64_t)st->st_ctim.tv_sec * 1000000000ULL +
                    (uint64_t)st->st_ctim.tv_nsec;
}

static pgw_err attr_of(const char *path, pgw_fs_attr *out, bool follow)
{
    struct stat st;
    int rc = follow ? stat(path, &st) : lstat(path, &st);
    if (rc != 0)
        return pgw_code_from_host(errno);
    be_fill_attr(&st, out);
    return PGW_OK;
}

pgw_err be_file_lookup(void *impl, const char *relpath, pgw_fs_attr *out,
                       uint32_t prio)
{
    (void)prio;
    char path[PGW_BE_PATH_MAX];
    if (!pgw_posix_join((pgw_posix_backend *)impl, relpath, path, sizeof(path)))
        return PGW_ENAMETOOLONG;
    /* The backend convention (pgw/rmfs.h): never follow the final symlink. The
     * path layer of rmfs resolves links itself, which is what spec 5.3 asks
     * for, so the truth backend must behave like lstat(2) here. */
    return attr_of(path, out, false);
}

pgw_err be_file_getattr(void *impl, const char *relpath, pgw_fs_attr *out,
                        uint32_t prio)
{
    (void)prio;
    char path[PGW_BE_PATH_MAX];
    if (!pgw_posix_join((pgw_posix_backend *)impl, relpath, path, sizeof(path)))
        return PGW_ENAMETOOLONG;
    /* getattr FOLLOWS a final symlink (stat semantics) - see the backend
     * convention in pgw/rmfs.h. The host stat() then yields exactly what a real
     * 9P or virtiofs server would answer, including ELOOP and ENOENT. */
    return attr_of(path, out, true);
}

/* --- setattr -------------------------------------------------------------- */

static pgw_err setattr_ownership(const char *path, const pgw_fs_setattr *sa)
{
    if (sa->set_mode && chmod(path, (mode_t)(sa->mode & PGW_S_PERM_MASK)) != 0)
        return pgw_code_from_host(errno);
    if (!sa->set_uid && !sa->set_gid)
        return PGW_OK;
    uid_t uid = sa->set_uid ? (uid_t)sa->uid : (uid_t)-1;
    gid_t gid = sa->set_gid ? (gid_t)sa->gid : (gid_t)-1;
    if (chown(path, uid, gid) != 0)
        return pgw_code_from_host(errno);
    return PGW_OK;
}

static pgw_err setattr_times(const char *path, const pgw_fs_setattr *sa)
{
    if (!sa->set_atime && !sa->set_mtime)
        return PGW_OK;
    struct stat st;
    if (lstat(path, &st) != 0)
        return pgw_code_from_host(errno);
    struct timespec tv[2];
    tv[0] = st.st_atim;
    tv[1] = st.st_mtim;
    if (sa->set_atime) {
        tv[0].tv_sec = (time_t)(sa->atime_ns / 1000000000ULL);
        tv[0].tv_nsec = (long)(sa->atime_ns % 1000000000ULL);
    }
    if (sa->set_mtime) {
        tv[1].tv_sec = (time_t)(sa->mtime_ns / 1000000000ULL);
        tv[1].tv_nsec = (long)(sa->mtime_ns % 1000000000ULL);
    }
    if (utimensat(AT_FDCWD, path, tv, AT_SYMLINK_NOFOLLOW) != 0)
        return pgw_code_from_host(errno);
    return PGW_OK;
}

pgw_err be_file_setattr(void *impl, const char *relpath,
                        const pgw_fs_setattr *sa, uint32_t prio)
{
    (void)prio;
    char path[PGW_BE_PATH_MAX];
    if (!pgw_posix_join((pgw_posix_backend *)impl, relpath, path, sizeof(path)))
        return PGW_ENAMETOOLONG;
    pgw_err e = setattr_ownership(path, sa);
    if (e != PGW_OK)
        return e;
    if (sa->set_size && truncate(path, (off_t)sa->size) != 0)
        return pgw_code_from_host(errno);
    return setattr_times(path, sa);
}

/* --- open / close / read / write / fsync ---------------------------------- */

pgw_err be_file_open_fd(pgw_posix_backend *b, const char *relpath,
                        uint32_t pgw_oflags, uint32_t create_mode,
                        uint64_t *out_handle)
{
    char path[PGW_BE_PATH_MAX];
    if (!pgw_posix_join(b, relpath, path, sizeof(path)))
        return PGW_ENAMETOOLONG;
    int fd = open(path, (int)pgw_host_oflags(pgw_oflags),
                  (mode_t)(create_mode & PGW_S_PERM_MASK));
    if (fd < 0)
        return pgw_code_from_host(errno);
    be_handle *h = be_handle_alloc(b);
    if (h == NULL) {
        close(fd);
        return PGW_EMFILE;
    }
    h->fd = fd;
    snprintf(h->path, sizeof(h->path), "%s", path);
    *out_handle = (uint64_t)(h - b->handles) + 1;
    return PGW_OK;
}

pgw_err be_file_close(void *impl, uint64_t handle, uint32_t prio)
{
    (void)prio;
    be_handle *h = be_handle_get((pgw_posix_backend *)impl, handle);
    if (h == NULL)
        return PGW_EBADF;
    be_handle_free(h);
    return PGW_OK;
}

pgw_err be_file_read(void *impl, uint64_t handle, uint64_t off, void *buf,
                     size_t len, size_t *out_read, uint32_t prio)
{
    (void)prio;
    be_handle *h = be_handle_get((pgw_posix_backend *)impl, handle);
    if (h == NULL || h->fd < 0)
        return PGW_EBADF;
    ssize_t n = pread(h->fd, buf, len, (off_t)off);
    if (n < 0)
        return pgw_code_from_host(errno);
    *out_read = (size_t)n;
    pgw_stat_add(PGW_STAT_BYTES_READ, (uint64_t)n);
    return PGW_OK;
}

pgw_err be_file_write(void *impl, uint64_t handle, uint64_t off,
                      const void *buf, size_t len, size_t *out_written,
                      uint32_t prio)
{
    (void)prio;
    be_handle *h = be_handle_get((pgw_posix_backend *)impl, handle);
    if (h == NULL || h->fd < 0)
        return PGW_EBADF;
    ssize_t n = pwrite(h->fd, buf, len, (off_t)off);
    if (n < 0)
        return pgw_code_from_host(errno);
    *out_written = (size_t)n;
    pgw_stat_add(PGW_STAT_BYTES_WRITTEN, (uint64_t)n);
    return PGW_OK;
}

pgw_err be_file_fsync(void *impl, uint64_t handle, uint32_t prio)
{
    (void)prio;
    be_handle *h = be_handle_get((pgw_posix_backend *)impl, handle);
    if (h == NULL || h->fd < 0)
        return PGW_EBADF;
    if (fsync(h->fd) != 0)
        return pgw_code_from_host(errno);
    return PGW_OK;
}

pgw_err be_file_statfs(void *impl, pgw_fs_statvfs *out, uint32_t prio)
{
    (void)prio;
    pgw_posix_backend *b = (pgw_posix_backend *)impl;
    struct statvfs vfs;
    if (statvfs(b->root, &vfs) != 0)
        return pgw_code_from_host(errno);
    memset(out, 0, sizeof(*out));
    out->bsize = (uint32_t)vfs.f_bsize;
    out->namelen = (uint32_t)vfs.f_namemax;
    out->blocks = (uint64_t)vfs.f_blocks;
    out->bfree = (uint64_t)vfs.f_bfree;
    out->bavail = (uint64_t)vfs.f_bavail;
    out->files = (uint64_t)vfs.f_files;
    out->ffree = (uint64_t)vfs.f_ffree;
    return PGW_OK;
}

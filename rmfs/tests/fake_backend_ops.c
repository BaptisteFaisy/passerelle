/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/tests/fake_backend_ops.c - The vtable of the fake backend.
 *
 * Every entry of pgw_fs_backend_ops mapped onto a POSIX call on a temporary
 * directory, so layer 3 can be exercised without 9P or virtiofs (FS-01..FS-15).
 * The paths handed by layer 3 are always joined under the fake root, never
 * used as absolute host paths.
 */
#define _XOPEN_SOURCE 700

#include "fake_backend_priv.h"

#include "pgw/err.h"
#include "pgw/time.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static pgw_err host_errno(int e)
{
    return pgw_code_from_host(e);
}

/* --- attributes ----------------------------------------------------------- */

static pgw_err fb_lookup(void *impl, const char *rel, pgw_fs_attr *out,
                         uint32_t prio)
{
    fake_backend *b = (fake_backend *)impl;
    (void)prio;
    b->lookup_calls++;
    b->attr_calls++;
    b->last_prio = prio;
    pgw_err forced;
    if (fake_take_failure(b, &forced))
        return forced;
    char host[1024];
    fake_backend_host_path(b, rel, host, sizeof(host));
    struct stat st;
    if (lstat(host, &st) != 0)
        return host_errno(errno);
    fake_fill_attr(out, &st);
    return PGW_OK;
}

static pgw_err fb_getattr(void *impl, const char *rel, pgw_fs_attr *out,
                          uint32_t prio)
{
    fake_backend *b = (fake_backend *)impl;
    (void)prio;
    b->getattr_calls++;
    b->attr_calls++;
    b->last_prio = prio;
    if (b->hide_getattr)
        return PGW_ENOENT;
    pgw_err forced;
    if (fake_take_failure(b, &forced))
        return forced;
    char host[1024];
    fake_backend_host_path(b, rel, host, sizeof(host));
    struct stat st;
    if (stat(host, &st) != 0)
        return host_errno(errno);
    fake_fill_attr(out, &st);
    return PGW_OK;
}

static pgw_err fb_setattr(void *impl, const char *rel,
                          const pgw_fs_setattr *sa, uint32_t prio)
{
    fake_backend *b = (fake_backend *)impl;
    (void)prio;
    pgw_err forced;
    if (fake_take_failure(b, &forced))
        return forced;
    char host[1024];
    fake_backend_host_path(b, rel, host, sizeof(host));
    if (sa->set_size && truncate(host, (off_t)sa->size) != 0)
        return host_errno(errno);
    struct stat st;
    if (sa->set_mode || sa->set_uid || sa->set_gid) {
        if (stat(host, &st) != 0)
            return host_errno(errno);
        mode_t mode = (mode_t)(sa->set_mode ? (sa->mode & PGW_S_PERM_MASK)
                                           : (st.st_mode & 07777u));
        if (chmod(host, mode) != 0)
            return host_errno(errno);
        if (sa->set_uid || sa->set_gid) {
            uid_t uid = sa->set_uid ? (uid_t)sa->uid : st.st_uid;
            gid_t gid = sa->set_gid ? (gid_t)sa->gid : st.st_gid;
            if (chown(host, uid, gid) != 0)
                return host_errno(errno);
        }
    }
    if (sa->set_atime || sa->set_mtime) {
        struct timespec ts[2];
        ts[0].tv_sec = (time_t)(sa->atime_ns / 1000000000ULL);
        ts[0].tv_nsec = (long)(sa->atime_ns % 1000000000ULL);
        ts[1].tv_sec = (time_t)(sa->mtime_ns / 1000000000ULL);
        ts[1].tv_nsec = (long)(sa->mtime_ns % 1000000000ULL);
        if (utimensat(AT_FDCWD, host, ts, 0) != 0)
            return host_errno(errno);
    }
    return PGW_OK;
}

/* --- open / close --------------------------------------------------------- */

static int alloc_handle(void *impl)
{
    fake_handle *h = fake_handles(impl);
    for (int i = 0; i < FAKE_MAX_OPEN; i++) {
        if (!h[i].used)
            return i;
    }
    return -1;
}

static pgw_err fb_open_dir(fake_backend *b, const char *host,
                           uint64_t *out_handle)
{
    DIR *d = opendir(host);
    if (d == NULL)
        return host_errno(errno);
    int idx = alloc_handle(b);
    if (idx < 0) {
        closedir(d);
        return PGW_EMFILE;
    }
    fake_handle *h = fake_handles(b);
    h[idx].used = 1;
    h[idx].is_dir = 1;
    h[idx].dirp = d;
    *out_handle = (uint64_t)idx + 1u;
    return PGW_OK;
}

static pgw_err fb_open(void *impl, const char *rel, uint32_t pgw_oflags,
                       uint32_t create_mode, uint64_t *out_handle,
                       uint32_t prio)
{
    fake_backend *b = (fake_backend *)impl;
    (void)prio;
    b->open_calls++;
    b->last_prio = prio;
    pgw_err forced;
    if (fake_take_failure(b, &forced))
        return forced;
    char host[1024];
    fake_backend_host_path(b, rel, host, sizeof(host));
    if ((pgw_oflags & PGW_O_DIRECTORY) != 0)
        return fb_open_dir(b, host, out_handle);
    int fd = open(host, (int)pgw_host_oflags(pgw_oflags),
                  (mode_t)(create_mode & PGW_S_PERM_MASK));
    if (fd < 0)
        return host_errno(errno);
    int idx = alloc_handle(b);
    if (idx < 0) {
        close(fd);
        return PGW_EMFILE;
    }
    fake_handle *h = fake_handles(b);
    h[idx].used = 1;
    h[idx].is_dir = 0;
    h[idx].fd = fd;
    *out_handle = (uint64_t)idx + 1u;
    return PGW_OK;
}

static pgw_err fb_close(void *impl, uint64_t handle, uint32_t prio)
{
    fake_backend *b = (fake_backend *)impl;
    (void)prio;
    b->close_calls++;
    fake_handle *e = fake_handle_at(impl, handle);
    if (e == NULL)
        return PGW_EBADF;
    if (e->is_dir)
        closedir(e->dirp);
    else
        close(e->fd);
    e->used = 0;
    return PGW_OK;
}

/* --- data ----------------------------------------------------------------- */

static pgw_err fb_read(void *impl, uint64_t handle, uint64_t off, void *buf,
                       size_t len, size_t *out_read, uint32_t prio)
{
    fake_backend *b = (fake_backend *)impl;
    (void)prio;
    b->read_calls++;
    b->last_prio = prio;
    pgw_err forced;
    if (fake_take_failure(b, &forced))
        return forced;
    fake_handle *e = fake_handle_at(impl, handle);
    if (e == NULL || e->is_dir)
        return PGW_EBADF;
    ssize_t n = pread(e->fd, buf, len, (off_t)off);
    if (n < 0)
        return host_errno(errno);
    *out_read = (size_t)n;
    return PGW_OK;
}

static pgw_err fb_write(void *impl, uint64_t handle, uint64_t off,
                        const void *buf, size_t len, size_t *out_written,
                        uint32_t prio)
{
    fake_backend *b = (fake_backend *)impl;
    (void)prio;
    b->write_calls++;
    pgw_err forced;
    if (fake_take_failure(b, &forced))
        return forced;
    fake_handle *e = fake_handle_at(impl, handle);
    if (e == NULL || e->is_dir)
        return PGW_EBADF;
    ssize_t n = pwrite(e->fd, buf, len, (off_t)off);
    if (n < 0)
        return host_errno(errno);
    *out_written = (size_t)n;
    return PGW_OK;
}

static pgw_err fb_readdir(void *impl, uint64_t handle, uint64_t off,
                          pgw_fs_dirent *out, size_t max_entries,
                          size_t *out_count, uint64_t *next_off, uint32_t prio)
{
    fake_backend *b = (fake_backend *)impl;
    (void)prio;
    b->readdir_calls++;
    b->last_prio = prio;
    pgw_err forced;
    if (fake_take_failure(b, &forced))
        return forced;
    fake_handle *e = fake_handle_at(impl, handle);
    if (e == NULL || !e->is_dir)
        return PGW_ENOTDIR;
    if (off == 0)
        rewinddir(e->dirp);
    size_t n = 0;
    *next_off = off;
    while (n < max_entries) {
        struct dirent *de = readdir(e->dirp);
        if (de == NULL)
            break;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        out[n].ino = (uint64_t)de->d_ino;
        out[n].type = pgw_ftype_from_dt((uint8_t)de->d_type);
        snprintf(out[n].name, sizeof(out[n].name), "%s", de->d_name);
        n++;
        /* Directory offsets are entry indexes: resumable, never duplicated. */
        *next_off = off + (uint64_t)n;
        out[n - 1].next_off = *next_off;
    }
    *out_count = n;
    return PGW_OK;
}

static pgw_err fb_fsync(void *impl, uint64_t handle, uint32_t prio)
{
    (void)prio;
    fake_handle *e = fake_handle_at(impl, handle);
    if (e == NULL || e->is_dir)
        return PGW_EBADF;
    if (fsync(e->fd) != 0)
        return host_errno(errno);
    return PGW_OK;
}

/* --- names ---------------------------------------------------------------- */

static pgw_err fb_mkdir(void *impl, const char *rel, uint32_t mode,
                        uint32_t prio)
{
    fake_backend *b = (fake_backend *)impl;
    (void)prio;
    pgw_err forced;
    if (fake_take_failure(b, &forced))
        return forced;
    char host[1024];
    fake_backend_host_path(b, rel, host, sizeof(host));
    if (mkdir(host, (mode_t)(mode & PGW_S_PERM_MASK)) != 0)
        return host_errno(errno);
    return PGW_OK;
}

static pgw_err fb_unlink(void *impl, const char *rel, bool is_dir,
                         uint32_t prio)
{
    fake_backend *b = (fake_backend *)impl;
    (void)prio;
    b->unlink_calls++;
    pgw_err forced;
    if (fake_take_failure(b, &forced))
        return forced;
    char host[1024];
    fake_backend_host_path(b, rel, host, sizeof(host));
    if ((is_dir ? rmdir(host) : unlink(host)) != 0)
        return host_errno(errno);
    return PGW_OK;
}

static pgw_err fb_rename(void *impl, const char *oldrel, const char *newrel,
                         uint32_t prio)
{
    fake_backend *b = (fake_backend *)impl;
    (void)prio;
    pgw_err forced;
    if (fake_take_failure(b, &forced))
        return forced;
    char ho[1024];
    char hn[1024];
    fake_backend_host_path(b, oldrel, ho, sizeof(ho));
    fake_backend_host_path(b, newrel, hn, sizeof(hn));
    if (rename(ho, hn) != 0)
        return host_errno(errno);
    return PGW_OK;
}

static pgw_err fb_symlink(void *impl, const char *linkrel, const char *target,
                          uint32_t prio)
{
    fake_backend *b = (fake_backend *)impl;
    (void)prio;
    pgw_err forced;
    if (fake_take_failure(b, &forced))
        return forced;
    char host[1024];
    fake_backend_host_path(b, linkrel, host, sizeof(host));
    if (symlink(target, host) != 0)
        return host_errno(errno);
    return PGW_OK;
}

static pgw_err fb_readlink(void *impl, const char *linkrel, char *target,
                           size_t cap, uint32_t prio)
{
    fake_backend *b = (fake_backend *)impl;
    (void)prio;
    pgw_err forced;
    if (fake_take_failure(b, &forced))
        return forced;
    if (cap == 0)
        return PGW_EINVAL;
    char host[1024];
    fake_backend_host_path(b, linkrel, host, sizeof(host));
    ssize_t n = readlink(host, target, cap - 1);
    if (n < 0)
        return host_errno(errno);
    target[n] = '\0';
    return PGW_OK;
}

static pgw_err fb_link(void *impl, const char *existrel, const char *newrel,
                       uint32_t prio)
{
    fake_backend *b = (fake_backend *)impl;
    (void)prio;
    pgw_err forced;
    if (fake_take_failure(b, &forced))
        return forced;
    char he[1024];
    char hn[1024];
    fake_backend_host_path(b, existrel, he, sizeof(he));
    fake_backend_host_path(b, newrel, hn, sizeof(hn));
    if (link(he, hn) != 0)
        return host_errno(errno);
    return PGW_OK;
}

static pgw_err fb_statfs(void *impl, pgw_fs_statvfs *out, uint32_t prio)
{
    fake_backend *b = (fake_backend *)impl;
    (void)prio;
    pgw_err forced;
    if (fake_take_failure(b, &forced))
        return forced;
    struct statvfs sv;
    if (statvfs(b->root, &sv) != 0)
        return host_errno(errno);
    out->bsize = (uint32_t)sv.f_bsize;
    out->namelen = (uint32_t)sv.f_namemax;
    out->blocks = (uint64_t)sv.f_blocks;
    out->bfree = (uint64_t)sv.f_bfree;
    out->bavail = (uint64_t)sv.f_bavail;
    out->files = (uint64_t)sv.f_files;
    out->ffree = (uint64_t)sv.f_ffree;
    return PGW_OK;
}

void fake_backend_install_ops(pgw_fs_backend_ops *ops)
{
    memset(ops, 0, sizeof(*ops));
    ops->lookup = fb_lookup;
    ops->getattr = fb_getattr;
    ops->setattr = fb_setattr;
    ops->open = fb_open;
    ops->close = fb_close;
    ops->read = fb_read;
    ops->write = fb_write;
    ops->readdir = fb_readdir;
    ops->mkdir = fb_mkdir;
    ops->unlink = fb_unlink;
    ops->rename = fb_rename;
    ops->symlink = fb_symlink;
    ops->readlink = fb_readlink;
    ops->link = fb_link;
    ops->fsync = fb_fsync;
    ops->statfs = fb_statfs;
}

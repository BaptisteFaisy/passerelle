/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/src/rmfs_ops_io.c - Data-path operations of the resource manager.
 *
 * Implements requirements FS-01, FS-03, FS-04, FS-09, FS-10, FS-11, FS-12,
 * FS-14, QNX-04, QNX-08 and SEC-01:
 *   - open with O_CREAT/O_EXCL/O_TRUNC/O_APPEND and read-only mount refusal
 *     (FS-04, FS-09)
 *   - noexec / nosuid refusal before the backend is even asked (SEC-01)
 *   - read / write / readdir with counter and byte accounting (FS-01, FS-03)
 *   - deleted-but-open files stay readable through their descriptor (FS-11)
 *   - fsync reaching the PC (FS-10)
 *   - guest-local byte-range locks, never relayed to the server (FS-14)
 * No data cache is kept anywhere: the next read always asks the backend
 * (D4, FS-12).
 */
#include "rmfs_internal.h"

#include "path.h"
#include "pgw/limits.h"
#include "pgw/log.h"

#include <stdio.h>
#include <string.h>

/* Permissions required by the requested access mode (FS-09). */
static pgw_err check_open_access(const pgw_fs_attr *attr,
                                 const pgw_fs_cred *cred, uint32_t oflags)
{
    if (rmfs_write_requested(oflags)) {
        if (!rmfs_perm_ok(attr, cred, 0200))
            return PGW_EACCES;
    } else {
        if (!rmfs_perm_ok(attr, cred, 0400))
            return PGW_EACCES;
    }
    return PGW_OK;
}

/* An O_TRUNC on an existing file needs write permission on the file itself. */
static pgw_err check_trunc(const pgw_fs_attr *attr, const pgw_fs_cred *cred,
                           uint32_t oflags)
{
    if ((oflags & PGW_O_TRUNC) == 0)
        return PGW_OK;
    if (attr->type == PGW_FT_DIR)
        return PGW_EISDIR;
    return rmfs_perm_ok(attr, cred, 0200) ? PGW_OK : PGW_EACCES;
}

pgw_err pgw_rmfs_open(pgw_rmfs *fs, const pgw_fs_cred *cred, const char *path,
                      uint32_t pgw_oflags, uint32_t mode, pgw_rmfs_fd **out_fd)
{
    if (fs == NULL || cred == NULL || path == NULL || out_fd == NULL)
        return PGW_EINVAL;
    *out_fd = NULL;
    uint32_t prio = cred->prio;
    bool creating = (pgw_oflags & PGW_O_CREAT) != 0;

    pgw_err err = PGW_OK;
    if (creating || rmfs_write_requested(pgw_oflags))
        err = rmfs_require_writable(fs); /* FS-09: ro mount */
    /* O_NOFOLLOW: the last component must not be a symlink (FS-08). */
    bool follow = (pgw_oflags & PGW_O_NOFOLLOW) == 0;
    char resolved[PGW_PATH_MAX];
    pgw_fs_attr attr;
    if (err == PGW_OK)
        err = rmfs_resolve(fs, path, prio, follow, resolved, sizeof(resolved));
    if (err == PGW_OK && !follow) {
        pgw_fs_attr probe;
        if (rmfs_getattr_cached(fs, resolved, prio, false, &probe) == PGW_OK &&
            probe.type == PGW_FT_LNK)
            err = PGW_ELOOP; /* POSIX: open(O_NOFOLLOW) on a link */
    }

    bool existed = true;
    if (err == PGW_OK) {
        pgw_err gerr = rmfs_getattr_cached(fs, resolved, prio, true, &attr);
        if (gerr == PGW_ENOENT && creating) {
            existed = false;
            err = rmfs_check_parent_write(fs, cred, resolved, prio);
        } else if (gerr != PGW_OK) {
            err = gerr;
        } else {
            err = check_open_access(&attr, cred, pgw_oflags);
            if (err == PGW_OK)
                err = check_trunc(&attr, cred, pgw_oflags);
            if (err == PGW_OK && (pgw_oflags & PGW_O_EXCL) != 0 && creating)
                err = PGW_EEXIST; /* FS-04 */
            if (err == PGW_OK)
                err = rmfs_check_exec(fs, &attr, pgw_oflags); /* SEC-01 */
        }
    }
    if (err == PGW_OK && existed && attr.type == PGW_FT_DIR &&
        rmfs_write_requested(pgw_oflags))
        err = PGW_EISDIR;

    if (err == PGW_OK) {
        rmfs_args a = {0};
        a.fs = fs;
        a.prio = prio;
        a.p1 = resolved;
        a.flags = pgw_oflags & ~PGW_RMFS_O_EXEC; /* guest-only bit */
        a.mode = mode & PGW_S_PERM_MASK;
        err = rmfs_run(fs, prio, call_open, &a);
        if (err == PGW_OK) {
            pgw_rmfs_fd *fd = rmfs_open_table_insert(fs->fds, a.handle,
                                                     resolved, pgw_oflags, mode,
                                                     cred);
            if (fd == NULL) {
                rmfs_args c = {0};
                c.fs = fs;
                c.prio = prio;
                c.handle = a.handle;
                rmfs_run(fs, prio, call_close, &c);
                err = PGW_ENOMEM;
            } else {
                *out_fd = fd;
                rmfs_cache_invalidate(fs, resolved);
            }
        }
    }
    rmfs_count(err);
    PGW_DBG("rmfs: open %s flags=0x%x -> %s", path, (unsigned)pgw_oflags,
            pgw_err_name(err));
    return err;
}

pgw_err pgw_rmfs_close(pgw_rmfs *fs, pgw_rmfs_fd *fd)
{
    if (fs == NULL || fd == NULL)
        return PGW_EINVAL;
    /* Read the handle before the table may free the descriptor. */
    uint64_t handle = fd->handle;
    uint32_t prio = fd->prio;
    pgw_err err = PGW_OK;
    if (rmfs_open_table_remove(fs->fds, fd)) {
        rmfs_args a = {0};
        a.fs = fs;
        a.prio = prio;
        a.handle = handle;
        err = rmfs_run(fs, prio, call_close, &a);
    }
    rmfs_count(err);
    PGW_DBG("rmfs: close -> %s", pgw_err_name(err));
    return err;
}

/* An fd that is only readable must refuse a write, and vice versa. */
static pgw_err check_fd_access(const pgw_rmfs_fd *fd, bool want_write)
{
    uint32_t flags = fd->oflags;
    bool writable = (flags & (PGW_O_WRONLY | PGW_O_RDWR)) != 0;
    bool readable = (flags & (PGW_O_WRONLY)) == 0;
    if (want_write && !writable)
        return PGW_EBADF;
    if (!want_write && !readable)
        return PGW_EBADF;
    return PGW_OK;
}

pgw_err pgw_rmfs_read(pgw_rmfs *fs, pgw_rmfs_fd *fd, uint64_t off, void *buf,
                      size_t len, size_t *out_read)
{
    if (fs == NULL || fd == NULL || buf == NULL || out_read == NULL)
        return PGW_EINVAL;
    *out_read = 0;
    pgw_err err = check_fd_access(fd, false);
    if (err == PGW_OK) {
        rmfs_args a = {0};
        a.fs = fs;
        a.prio = fd->prio;
        a.handle = fd->handle;
        a.off = off;
        a.buf = buf;
        a.len = len;
        a.out_n = out_read;
        err = rmfs_run(fs, fd->prio, call_read, &a);
    }
    if (err == PGW_OK)
        pgw_stat_add(PGW_STAT_BYTES_READ, (uint64_t)*out_read);
    rmfs_count(err);
    PGW_DBG("rmfs: read off=%llu len=%zu -> %s (%zu bytes)",
            (unsigned long long)off, len, pgw_err_name(err), *out_read);
    return err;
}

pgw_err pgw_rmfs_write(pgw_rmfs *fs, pgw_rmfs_fd *fd, uint64_t off,
                       const void *buf, size_t len, size_t *out_written)
{
    if (fs == NULL || fd == NULL || buf == NULL || out_written == NULL)
        return PGW_EINVAL;
    *out_written = 0;
    pgw_err err = rmfs_require_writable(fs);
    if (err == PGW_OK)
        err = check_fd_access(fd, true);
    if (err == PGW_OK && (fd->oflags & PGW_O_APPEND) != 0) {
        /* FS-04: append writes always land at the current end of file. */
        pgw_fs_attr attr;
        pgw_err gerr = rmfs_getattr_cached(fs, fd->path, fd->prio, true,
                                          &attr);
        if (gerr == PGW_OK)
            off = attr.size;
    }
    if (err == PGW_OK) {
        rmfs_args a = {0};
        a.fs = fs;
        a.prio = fd->prio;
        a.handle = fd->handle;
        a.off = off;
        a.cbuf = buf;
        a.len = len;
        a.out_n = out_written;
        err = rmfs_run(fs, fd->prio, call_write, &a);
    }
    if (err == PGW_OK) {
        pgw_stat_add(PGW_STAT_BYTES_WRITTEN, (uint64_t)*out_written);
        rmfs_cache_invalidate(fs, fd->path); /* FS-12: size changed */
    }
    rmfs_count(err);
    PGW_DBG("rmfs: write off=%llu len=%zu -> %s (%zu bytes)",
            (unsigned long long)off, len, pgw_err_name(err), *out_written);
    return err;
}

pgw_err pgw_rmfs_fstat(pgw_rmfs *fs, pgw_rmfs_fd *fd, pgw_fs_attr *out)
{
    if (fs == NULL || fd == NULL || out == NULL)
        return PGW_EINVAL;
    pgw_err err = rmfs_getattr_cached(fs, fd->path, fd->prio, true, out);
    rmfs_count(err);
    PGW_DBG("rmfs: fstat -> %s", pgw_err_name(err));
    return err;
}

pgw_err pgw_rmfs_readdir(pgw_rmfs *fs, pgw_rmfs_fd *fd, uint64_t off,
                         pgw_fs_dirent *out, size_t max_entries,
                         size_t *out_count, uint64_t *next_off)
{
    if (fs == NULL || fd == NULL || out == NULL || out_count == NULL ||
        next_off == NULL || max_entries == 0)
        return PGW_EINVAL;
    *out_count = 0;
    *next_off = off;
    rmfs_args a = {0};
    a.fs = fs;
    a.prio = fd->prio;
    a.handle = fd->handle;
    a.off = off;
    a.dirents = out;
    a.max_entries = max_entries;
    a.out_count = out_count;
    a.next_off = next_off;
    pgw_err err = rmfs_run(fs, fd->prio, call_readdir, &a);
    rmfs_count(err);
    PGW_DBG("rmfs: readdir off=%llu -> %s (%zu entries)",
            (unsigned long long)off, pgw_err_name(err), *out_count);
    return err;
}

pgw_err pgw_rmfs_fsync(pgw_rmfs *fs, pgw_rmfs_fd *fd)
{
    if (fs == NULL || fd == NULL)
        return PGW_EINVAL;
    rmfs_args a = {0};
    a.fs = fs;
    a.prio = fd->prio;
    a.handle = fd->handle;
    pgw_err err = rmfs_run(fs, fd->prio, call_fsync, &a);
    rmfs_count(err);
    PGW_DBG("rmfs: fsync -> %s", pgw_err_name(err));
    return err;
}

/* --- FS-14: guest-local byte-range locks --------------------------------- */

pgw_err pgw_rmfs_lock(pgw_rmfs *fs, const char *path, uint32_t owner_pid,
                      pgw_lock_type type, uint64_t start, uint64_t len)
{
    if (fs == NULL || path == NULL)
        return PGW_EINVAL;
    if (type != PGW_LOCK_RD && type != PGW_LOCK_WR && type != PGW_LOCK_UN)
        return PGW_EINVAL;
    char resolved[PGW_PATH_MAX];
    pgw_err err = rmfs_path_normalize(path, resolved, sizeof(resolved));
    if (err == PGW_OK && type != PGW_LOCK_UN)
        err = rmfs_require_writable(fs);
    if (err == PGW_OK)
        err = rmfs_lock_apply(fs->locks, resolved, owner_pid, type, start, len);
    rmfs_count(err);
    PGW_DBG("rmfs: lock %s pid=%u type=%d -> %s", path, owner_pid, (int)type,
            pgw_err_name(err));
    return err;
}

pgw_err pgw_rmfs_getlock(pgw_rmfs *fs, const char *path, uint32_t owner_pid,
                         pgw_lock_type *type, uint64_t *start, uint64_t *len,
                         uint32_t *holder_pid)
{
    if (fs == NULL || path == NULL || type == NULL || start == NULL ||
        len == NULL || holder_pid == NULL)
        return PGW_EINVAL;
    char resolved[PGW_PATH_MAX];
    pgw_err err = rmfs_path_normalize(path, resolved, sizeof(resolved));
    if (err == PGW_OK)
        err = rmfs_lock_test(fs->locks, resolved, owner_pid, type, start, len,
                             holder_pid);
    rmfs_count(err);
    PGW_DBG("rmfs: getlock %s pid=%u -> %s", path, owner_pid,
            pgw_err_name(err));
    return err;
}

void pgw_rmfs_unlock_pid(pgw_rmfs *fs, uint32_t owner_pid)
{
    if (fs == NULL)
        return;
    rmfs_lock_release_pid(fs->locks, owner_pid);
    PGW_DBG("rmfs: released all locks of pid %u", owner_pid);
}

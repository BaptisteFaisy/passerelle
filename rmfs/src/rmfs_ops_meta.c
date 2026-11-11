/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/src/rmfs_ops_meta.c - Metadata operations of the resource manager.
 *
 * Implements requirements FS-02, FS-05, FS-07, FS-08, FS-09, FS-10, FS-11,
 * FS-15 and SEC-01:
 *   - stat / lstat with nanosecond timestamps and a stable inode (FS-02)
 *   - mkdir, unlink/rmdir, rename across directories, symlink, readlink,
 *     hard link (FS-05, FS-08)
 *   - chmod / chown / truncate / utimens with guest-side permission checks
 *     (FS-07, FS-09); setuid/setgid stripped on a nosuid mount (SEC-01)
 *   - unlink of a file that is still open marks the descriptors and keeps
 *     them readable (FS-11)
 *   - statfs for df (FS-10)
 * Every operation bumps the counters of QNX-08 and logs at DEBUG.
 */
#include "rmfs_internal.h"

#include "path.h"
#include "pgw/limits.h"
#include "pgw/log.h"

#include <stdio.h>
#include <string.h>

/* Resolve then read the attributes in a single pass over the backend: the
 * walk that looks for symlinks already asked for the last component, so its
 * answer is reused (PERF-02: one round trip per component, not two). */
static pgw_err meta_getattr(pgw_rmfs *fs, const char *path, uint32_t prio,
                            bool follow, char *resolved, size_t cap,
                            pgw_fs_attr *attr)
{
    return rmfs_resolve_and_stat(fs, path, prio, follow, resolved, cap, attr);
}

pgw_err pgw_rmfs_stat(pgw_rmfs *fs, const pgw_fs_cred *cred, const char *path,
                      pgw_fs_attr *out)
{
    if (fs == NULL || path == NULL || out == NULL)
        return PGW_EINVAL;
    /* stat itself is not gated; the request runs at the caller priority so
     * that QNX-03 holds for metadata too. */
    uint32_t prio = cred != NULL ? cred->prio : 0;
    char resolved[PGW_PATH_MAX];
    pgw_err err = meta_getattr(fs, path, prio, true, resolved, sizeof(resolved),
                               out);
    rmfs_count(err);
    PGW_DBG("rmfs: stat %s -> %s", path, pgw_err_name(err));
    return err;
}

pgw_err pgw_rmfs_lstat(pgw_rmfs *fs, const pgw_fs_cred *cred, const char *path,
                       pgw_fs_attr *out)
{
    if (fs == NULL || path == NULL || out == NULL)
        return PGW_EINVAL;
    uint32_t prio = cred != NULL ? cred->prio : 0;
    char resolved[PGW_PATH_MAX];
    pgw_err err = meta_getattr(fs, path, prio, false, resolved, sizeof(resolved),
                               out);
    rmfs_count(err);
    PGW_DBG("rmfs: lstat %s -> %s", path, pgw_err_name(err));
    return err;
}

/* Only the owner (or root) may change a mode (FS-09). */
static pgw_err check_owner(const pgw_fs_attr *attr, const pgw_fs_cred *cred)
{
    if (cred == NULL)
        return PGW_EINVAL;
    if (cred->uid == 0 || cred->uid == attr->uid)
        return PGW_OK;
    return PGW_EPERM;
}

pgw_err pgw_rmfs_chmod(pgw_rmfs *fs, const pgw_fs_cred *cred, const char *path,
                       uint32_t mode)
{
    if (fs == NULL || path == NULL)
        return PGW_EINVAL;
    pgw_err err = rmfs_require_writable(fs);
    char resolved[PGW_PATH_MAX];
    pgw_fs_attr attr;
    if (err == PGW_OK)
        err = meta_getattr(fs, path, cred != NULL ? cred->prio : 0, true,
                           resolved, sizeof(resolved), &attr);
    if (err == PGW_OK)
        err = check_owner(&attr, cred);
    if (err == PGW_OK && fs->opts.nosuid)
        mode &= ~(PGW_S_ISUID | PGW_S_ISGID); /* SEC-01 */
    if (err == PGW_OK) {
        rmfs_args a = {0};
        a.fs = fs;
        a.prio = cred->prio;
        a.p1 = resolved;
        a.sa.set_mode = true;
        a.sa.mode = mode & PGW_S_PERM_MASK;
        err = rmfs_run(fs, a.prio, call_setattr, &a);
        rmfs_cache_invalidate(fs, resolved);
    }
    rmfs_count(err);
    PGW_DBG("rmfs: chmod %s 0%03o -> %s", path, (unsigned)(mode & 07777u),
            pgw_err_name(err));
    return err;
}

/* Root may change anything; an owner may only keep its uid and set its own
 * group (POSIX chown rules, applied inside the guest). */
static pgw_err check_chown(const pgw_fs_attr *attr, const pgw_fs_cred *cred,
                           uint32_t uid, uint32_t gid)
{
    if (cred == NULL)
        return PGW_EINVAL;
    if (cred->uid == 0)
        return PGW_OK;
    if (cred->uid != attr->uid)
        return PGW_EPERM;
    if (uid != (uint32_t)-1 && uid != attr->uid)
        return PGW_EPERM;
    if (gid != (uint32_t)-1 && gid != cred->gid)
        return PGW_EPERM;
    return PGW_OK;
}

pgw_err pgw_rmfs_chown(pgw_rmfs *fs, const pgw_fs_cred *cred, const char *path,
                       uint32_t uid, uint32_t gid)
{
    if (fs == NULL || path == NULL)
        return PGW_EINVAL;
    pgw_err err = rmfs_require_writable(fs);
    char resolved[PGW_PATH_MAX];
    pgw_fs_attr attr;
    if (err == PGW_OK)
        err = meta_getattr(fs, path, cred != NULL ? cred->prio : 0, true,
                           resolved, sizeof(resolved), &attr);
    if (err == PGW_OK)
        err = check_chown(&attr, cred, uid, gid);
    if (err == PGW_OK) {
        rmfs_args a = {0};
        a.fs = fs;
        a.prio = cred->prio;
        a.p1 = resolved;
        a.sa.set_uid = (uid != (uint32_t)-1);
        a.sa.uid = uid;
        a.sa.set_gid = (gid != (uint32_t)-1);
        a.sa.gid = gid;
        err = rmfs_run(fs, a.prio, call_setattr, &a);
        rmfs_cache_invalidate(fs, resolved);
    }
    rmfs_count(err);
    PGW_DBG("rmfs: chown %s %u:%u -> %s", path, uid, gid, pgw_err_name(err));
    return err;
}

pgw_err pgw_rmfs_truncate(pgw_rmfs *fs, const pgw_fs_cred *cred,
                          const char *path, uint64_t size)
{
    if (fs == NULL || path == NULL)
        return PGW_EINVAL;
    pgw_err err = rmfs_require_writable(fs);
    char resolved[PGW_PATH_MAX];
    pgw_fs_attr attr;
    if (err == PGW_OK)
        err = meta_getattr(fs, path, cred != NULL ? cred->prio : 0, true,
                           resolved, sizeof(resolved), &attr);
    if (err == PGW_OK && !rmfs_perm_ok(&attr, cred, 0200))
        err = PGW_EACCES;
    if (err == PGW_OK) {
        rmfs_args a = {0};
        a.fs = fs;
        a.prio = cred->prio;
        a.p1 = resolved;
        a.sa.set_size = true;
        a.sa.size = size;
        err = rmfs_run(fs, a.prio, call_setattr, &a);
        rmfs_cache_invalidate(fs, resolved);
    }
    rmfs_count(err);
    PGW_DBG("rmfs: truncate %s %llu -> %s", path, (unsigned long long)size,
            pgw_err_name(err));
    return err;
}

pgw_err pgw_rmfs_utimens(pgw_rmfs *fs, const pgw_fs_cred *cred, const char *path,
                         uint64_t atime_ns, uint64_t mtime_ns)
{
    if (fs == NULL || path == NULL)
        return PGW_EINVAL;
    pgw_err err = rmfs_require_writable(fs);
    char resolved[PGW_PATH_MAX];
    pgw_fs_attr attr;
    if (err == PGW_OK)
        err = meta_getattr(fs, path, cred != NULL ? cred->prio : 0, true,
                           resolved, sizeof(resolved), &attr);
    if (err == PGW_OK && cred != NULL && cred->uid != 0 &&
        cred->uid != attr.uid && !rmfs_perm_ok(&attr, cred, 0200))
        err = PGW_EACCES;
    if (err == PGW_OK) {
        rmfs_args a = {0};
        a.fs = fs;
        a.prio = cred->prio;
        a.p1 = resolved;
        a.sa.set_atime = true;
        a.sa.atime_ns = atime_ns;
        a.sa.set_mtime = true;
        a.sa.mtime_ns = mtime_ns;
        err = rmfs_run(fs, a.prio, call_setattr, &a);
        rmfs_cache_invalidate(fs, resolved);
    }
    rmfs_count(err);
    PGW_DBG("rmfs: utimens %s -> %s", path, pgw_err_name(err));
    return err;
}

pgw_err pgw_rmfs_mkdir(pgw_rmfs *fs, const pgw_fs_cred *cred, const char *path,
                       uint32_t mode)
{
    if (fs == NULL || path == NULL)
        return PGW_EINVAL;
    pgw_err err = rmfs_require_writable(fs);
    char resolved[PGW_PATH_MAX];
    if (err == PGW_OK)
        err = rmfs_resolve(fs, path, cred != NULL ? cred->prio : 0, false,
                           resolved, sizeof(resolved));
    if (err == PGW_OK)
        err = rmfs_check_parent_write(fs, cred, resolved, cred->prio);
    if (err == PGW_OK) {
        rmfs_args a = {0};
        a.fs = fs;
        a.prio = cred->prio;
        a.p1 = resolved;
        a.mode = mode & PGW_S_PERM_MASK;
        err = rmfs_run(fs, a.prio, call_mkdir, &a);
        rmfs_cache_invalidate(fs, resolved);
    }
    rmfs_count(err);
    PGW_DBG("rmfs: mkdir %s -> %s", path, pgw_err_name(err));
    return err;
}

pgw_err pgw_rmfs_unlink(pgw_rmfs *fs, const pgw_fs_cred *cred, const char *path,
                        bool is_dir)
{
    if (fs == NULL || path == NULL)
        return PGW_EINVAL;
    pgw_err err = rmfs_require_writable(fs);
    char resolved[PGW_PATH_MAX];
    if (err == PGW_OK)
        err = rmfs_resolve(fs, path, cred != NULL ? cred->prio : 0, false,
                           resolved, sizeof(resolved));
    if (err == PGW_OK)
        err = rmfs_check_parent_write(fs, cred, resolved, cred->prio);
    if (err == PGW_OK) {
        rmfs_args a = {0};
        a.fs = fs;
        a.prio = cred->prio;
        a.p1 = resolved;
        a.flags = is_dir ? 1u : 0u;
        err = rmfs_run(fs, a.prio, call_unlink, &a);
        if (err == PGW_OK) {
            /* FS-11: open descriptors survive the unlink. */
            rmfs_open_table_mark_unlinked(fs->fds, resolved);
            rmfs_cache_invalidate(fs, resolved);
        }
    }
    rmfs_count(err);
    PGW_DBG("rmfs: unlink %s (%s) -> %s", path, is_dir ? "dir" : "file",
            pgw_err_name(err));
    return err;
}

pgw_err pgw_rmfs_rename(pgw_rmfs *fs, const pgw_fs_cred *cred,
                        const char *oldpath, const char *newpath)
{
    if (fs == NULL || oldpath == NULL || newpath == NULL)
        return PGW_EINVAL;
    uint32_t prio = cred != NULL ? cred->prio : 0;
    pgw_err err = rmfs_require_writable(fs);
    char rold[PGW_PATH_MAX];
    char rnew[PGW_PATH_MAX];
    if (err == PGW_OK)
        err = rmfs_resolve(fs, oldpath, prio, false, rold, sizeof(rold));
    if (err == PGW_OK)
        err = rmfs_resolve(fs, newpath, prio, false, rnew, sizeof(rnew));
    if (err == PGW_OK)
        err = rmfs_check_parent_write(fs, cred, rold, prio);
    if (err == PGW_OK)
        err = rmfs_check_parent_write(fs, cred, rnew, prio);
    if (err == PGW_OK) {
        rmfs_args a = {0};
        a.fs = fs;
        a.prio = prio;
        a.p1 = rold;
        a.p2 = rnew;
        err = rmfs_run(fs, prio, call_rename, &a);
        if (err == PGW_OK) {
            rmfs_open_table_rename(fs->fds, rold, rnew);
            rmfs_cache_invalidate(fs, rold);
            rmfs_cache_invalidate(fs, rnew);
        }
    }
    rmfs_count(err);
    PGW_DBG("rmfs: rename %s -> %s : %s", oldpath, newpath, pgw_err_name(err));
    return err;
}

pgw_err pgw_rmfs_symlink(pgw_rmfs *fs, const pgw_fs_cred *cred,
                         const char *target, const char *linkpath)
{
    if (fs == NULL || target == NULL || linkpath == NULL)
        return PGW_EINVAL;
    uint32_t prio = cred != NULL ? cred->prio : 0;
    pgw_err err = rmfs_require_writable(fs);
    char rlink[PGW_PATH_MAX];
    if (err == PGW_OK)
        err = rmfs_resolve(fs, linkpath, prio, false, rlink, sizeof(rlink));
    if (err == PGW_OK)
        err = rmfs_check_parent_write(fs, cred, rlink, prio);
    if (err == PGW_OK) {
        /* FS-15: a name is at most PGW_NAME_MAX bytes; the link body is a
         * path, bounded by PGW_PATH_MAX. */
        if (target[0] == '\0' || strlen(target) >= PGW_PATH_MAX)
            err = PGW_ENAMETOOLONG;
    }
    if (err == PGW_OK) {
        rmfs_args a = {0};
        a.fs = fs;
        a.prio = prio;
        a.p1 = rlink;
        a.p2 = target;
        err = rmfs_run(fs, prio, call_symlink, &a);
        rmfs_cache_invalidate(fs, rlink);
    }
    rmfs_count(err);
    PGW_DBG("rmfs: symlink %s -> %s : %s", linkpath, target,
            pgw_err_name(err));
    return err;
}

pgw_err pgw_rmfs_readlink(pgw_rmfs *fs, const pgw_fs_cred *cred,
                          const char *path, char *target, size_t cap)
{
    if (fs == NULL || path == NULL || target == NULL || cap == 0)
        return PGW_EINVAL;
    uint32_t prio = cred != NULL ? cred->prio : 0;
    char resolved[PGW_PATH_MAX];
    pgw_err err = rmfs_resolve(fs, path, prio, false, resolved,
                               sizeof(resolved));
    if (err == PGW_OK) {
        rmfs_args a = {0};
        a.fs = fs;
        a.prio = prio;
        a.p1 = resolved;
        a.target = target;
        a.cap = cap;
        err = rmfs_run(fs, prio, call_readlink, &a);
    }
    rmfs_count(err);
    PGW_DBG("rmfs: readlink %s -> %s", path, pgw_err_name(err));
    return err;
}

pgw_err pgw_rmfs_link(pgw_rmfs *fs, const pgw_fs_cred *cred,
                      const char *existing, const char *newpath)
{
    if (fs == NULL || existing == NULL || newpath == NULL)
        return PGW_EINVAL;
    uint32_t prio = cred != NULL ? cred->prio : 0;
    pgw_err err = rmfs_require_writable(fs);
    char rexist[PGW_PATH_MAX];
    char rnew[PGW_PATH_MAX];
    if (err == PGW_OK)
        err = rmfs_resolve(fs, existing, prio, true, rexist, sizeof(rexist));
    if (err == PGW_OK)
        err = rmfs_resolve(fs, newpath, prio, false, rnew, sizeof(rnew));
    if (err == PGW_OK)
        err = rmfs_check_parent_write(fs, cred, rnew, prio);
    if (err == PGW_OK) {
        rmfs_args a = {0};
        a.fs = fs;
        a.prio = prio;
        a.p1 = rexist;
        a.p2 = rnew;
        err = rmfs_run(fs, prio, call_link, &a);
        rmfs_cache_invalidate(fs, rnew);
    }
    rmfs_count(err);
    PGW_DBG("rmfs: link %s -> %s : %s", existing, newpath, pgw_err_name(err));
    return err;
}

pgw_err pgw_rmfs_statfs(pgw_rmfs *fs, const pgw_fs_cred *cred,
                        pgw_fs_statvfs *out)
{
    if (fs == NULL || out == NULL)
        return PGW_EINVAL;
    rmfs_args a = {0};
    a.fs = fs;
    a.prio = cred != NULL ? cred->prio : 0;
    a.stvfs = out;
    pgw_err err = rmfs_run(fs, a.prio, call_statfs, &a);
    rmfs_count(err);
    PGW_DBG("rmfs: statfs -> %s", pgw_err_name(err));
    return err;
}

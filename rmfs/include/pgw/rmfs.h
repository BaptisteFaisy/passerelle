/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pgw/rmfs.h - Layer 3 (common filesystem resource manager interface).
 *
 * Implements requirements FS-01..FS-15, QNX-01..QNX-09, PERF-05, SEC-01:
 *   - Protocol-neutral VFS operations (backed by 9P2000.L or FUSE/virtiofs)
 *   - Caller identity & permission checks inside guest (FS-09, SEC-01)
 *   - Mount options: ro, noexec, nosuid (SEC-01), bounded attr cache (FS-13)
 *   - Local POSIX-like byte-range locks between guest processes (FS-14)
 *   - Priority-ordered submission scheduler (QNX-04, PERF-05)
 *   - Open-file table keeping deleted-while-open files readable (FS-11)
 *   - Symlink resolution yielding redirect responses (spec 5.3) + loop detect
 */
#ifndef PGW_RMFS_H
#define PGW_RMFS_H

#include "pgw/err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pgw_fs_attr {
    pgw_ftype type;
    uint32_t mode;   /* permission bits 07777 */
    uint32_t uid;
    uint32_t gid;
    uint64_t ino;
    uint64_t size;
    uint64_t nlink;
    uint64_t atime_ns;
    uint64_t mtime_ns;
    uint64_t ctime_ns;
} pgw_fs_attr;

typedef struct pgw_fs_statvfs {
    uint32_t bsize;
    uint32_t namelen;
    uint64_t blocks;
    uint64_t bfree;
    uint64_t bavail;
    uint64_t files;
    uint64_t ffree;
} pgw_fs_statvfs;

typedef struct pgw_fs_dirent {
    uint64_t ino;
    uint64_t next_off;
    pgw_ftype type;
    char name[256];
} pgw_fs_dirent;

typedef struct pgw_fs_cred {
    uint32_t uid;
    uint32_t gid;
    /* Scheduler urgency: **bigger is more urgent**, which is the opposite of
     * the QNX numbering. Convert with pgw_prio_from_qnx() (pgw/prio.h) at the
     * boundary where the caller's QNX priority is read (QNX-03/04, PERF-05). */
    uint32_t prio;
} pgw_fs_cred;

typedef struct pgw_fs_opts {
    bool readonly;
    bool noexec;
    bool nosuid;          /* true by default (SEC-01) */
    uint64_t attr_ttl_ns; /* 0 = no cache (D4, FS-13) */
    uint32_t max_inflight;
} pgw_fs_opts;

typedef struct pgw_fs_setattr {
    bool set_mode;
    uint32_t mode;
    bool set_uid;
    uint32_t uid;
    bool set_gid;
    uint32_t gid;
    bool set_size;
    uint64_t size;
    bool set_atime;
    uint64_t atime_ns;
    bool set_mtime;
    uint64_t mtime_ns;
} pgw_fs_setattr;

/* Backend handle + vtable implemented by 9P and virtiofs adapters.
 *
 * SYMLINK CONVENTION (it is what makes spec 5.3 work, and getting it backwards
 * silently breaks both lstat and loop detection):
 *
 *   lookup(impl, relpath, ...)  -> attributes WITHOUT following a final
 *                                  symbolic link, i.e. lstat(2) semantics.
 *                                  The path walk uses it so that a link
 *                                  component is *seen* and turned into a
 *                                  redirect answer for the caller.
 *   getattr(impl, relpath, ...) -> attributes FOLLOWING a final symbolic link,
 *                                  i.e. stat(2) semantics; it may legitimately
 *                                  return PGW_ELOOP or PGW_ENOENT.
 *
 * An adapter whose protocol cannot distinguish the two (9P2000.L: a walk ends on
 * the link itself) may implement both with the same call - rmfs resolves links
 * itself through readlink before asking for followed attributes, so the result
 * is the same. What must never happen is `lookup` following links: the walk
 * would then never see a link and FS-08 (loop detection) becomes unreachable.
 *
 * Every callback returns a pgw_err (PGW_OK on success) and leaves the output
 * untouched on failure. None of them may block a caller thread (QNX-05). */
typedef struct pgw_fs_backend_ops {
    void (*destroy)(void *impl);
    pgw_err (*lookup)(void *impl, const char *relpath, pgw_fs_attr *out,
                      uint32_t prio);
    pgw_err (*getattr)(void *impl, const char *relpath, pgw_fs_attr *out,
                       uint32_t prio);
    pgw_err (*setattr)(void *impl, const char *relpath, const pgw_fs_setattr *sa,
                       uint32_t prio);
    pgw_err (*open)(void *impl, const char *relpath, uint32_t pgw_oflags,
                    uint32_t create_mode, uint64_t *out_handle, uint32_t prio);
    pgw_err (*close)(void *impl, uint64_t handle, uint32_t prio);
    pgw_err (*read)(void *impl, uint64_t handle, uint64_t off, void *buf,
                    size_t len, size_t *out_read, uint32_t prio);
    pgw_err (*write)(void *impl, uint64_t handle, uint64_t off,
                     const void *buf, size_t len, size_t *out_written,
                     uint32_t prio);
    pgw_err (*readdir)(void *impl, uint64_t handle, uint64_t off,
                       pgw_fs_dirent *out, size_t max_entries,
                       size_t *out_count, uint64_t *next_off, uint32_t prio);
    pgw_err (*mkdir)(void *impl, const char *relpath, uint32_t mode,
                     uint32_t prio);
    pgw_err (*unlink)(void *impl, const char *relpath, bool is_dir,
                      uint32_t prio);
    pgw_err (*rename)(void *impl, const char *oldpath, const char *newpath,
                      uint32_t prio);
    pgw_err (*symlink)(void *impl, const char *linkpath, const char *target,
                       uint32_t prio);
    pgw_err (*readlink)(void *impl, const char *linkpath, char *target,
                        size_t cap, uint32_t prio);
    pgw_err (*link)(void *impl, const char *existing, const char *newpath,
                    uint32_t prio);
    pgw_err (*fsync)(void *impl, uint64_t handle, uint32_t prio);
    pgw_err (*statfs)(void *impl, pgw_fs_statvfs *out, uint32_t prio);
} pgw_fs_backend_ops;

typedef struct pgw_rmfs pgw_rmfs;
typedef struct pgw_rmfs_fd pgw_rmfs_fd;

pgw_rmfs *pgw_rmfs_create(const pgw_fs_backend_ops *ops, void *impl,
                          const pgw_fs_opts *opts);
void pgw_rmfs_destroy(pgw_rmfs *fs);

/* Mount options defaults & checks. */
void pgw_fs_opts_default(pgw_fs_opts *opts);
const pgw_fs_opts *pgw_rmfs_opts(const pgw_rmfs *fs);
size_t pgw_rmfs_open_count(const pgw_rmfs *fs);

/* Operations invoked by QNX resmgr dispatchers or PC conformance tests. */
pgw_err pgw_rmfs_stat(pgw_rmfs *fs, const pgw_fs_cred *cred,
                      const char *path, pgw_fs_attr *out);
pgw_err pgw_rmfs_lstat(pgw_rmfs *fs, const pgw_fs_cred *cred,
                       const char *path, pgw_fs_attr *out);
pgw_err pgw_rmfs_open(pgw_rmfs *fs, const pgw_fs_cred *cred,
                      const char *path, uint32_t pgw_oflags, uint32_t mode,
                      pgw_rmfs_fd **out_fd);
pgw_err pgw_rmfs_close(pgw_rmfs *fs, pgw_rmfs_fd *fd);
pgw_err pgw_rmfs_read(pgw_rmfs *fs, pgw_rmfs_fd *fd, uint64_t off,
                      void *buf, size_t len, size_t *out_read);
pgw_err pgw_rmfs_write(pgw_rmfs *fs, pgw_rmfs_fd *fd, uint64_t off,
                       const void *buf, size_t len, size_t *out_written);
pgw_err pgw_rmfs_fstat(pgw_rmfs *fs, pgw_rmfs_fd *fd, pgw_fs_attr *out);
pgw_err pgw_rmfs_readdir(pgw_rmfs *fs, pgw_rmfs_fd *fd, uint64_t off,
                         pgw_fs_dirent *out, size_t max_entries,
                         size_t *out_count, uint64_t *next_off);
pgw_err pgw_rmfs_chmod(pgw_rmfs *fs, const pgw_fs_cred *cred,
                       const char *path, uint32_t mode);
pgw_err pgw_rmfs_chown(pgw_rmfs *fs, const pgw_fs_cred *cred,
                       const char *path, uint32_t uid, uint32_t gid);
pgw_err pgw_rmfs_truncate(pgw_rmfs *fs, const pgw_fs_cred *cred,
                          const char *path, uint64_t size);
pgw_err pgw_rmfs_utimens(pgw_rmfs *fs, const pgw_fs_cred *cred,
                         const char *path, uint64_t atime_ns, uint64_t mtime_ns);
pgw_err pgw_rmfs_mkdir(pgw_rmfs *fs, const pgw_fs_cred *cred,
                       const char *path, uint32_t mode);
pgw_err pgw_rmfs_unlink(pgw_rmfs *fs, const pgw_fs_cred *cred,
                        const char *path, bool is_dir);
pgw_err pgw_rmfs_rename(pgw_rmfs *fs, const pgw_fs_cred *cred,
                        const char *oldpath, const char *newpath);
pgw_err pgw_rmfs_symlink(pgw_rmfs *fs, const pgw_fs_cred *cred,
                         const char *target, const char *linkpath);
pgw_err pgw_rmfs_readlink(pgw_rmfs *fs, const pgw_fs_cred *cred,
                          const char *path, char *target, size_t cap);
pgw_err pgw_rmfs_link(pgw_rmfs *fs, const pgw_fs_cred *cred,
                      const char *existing, const char *newpath);
pgw_err pgw_rmfs_fsync(pgw_rmfs *fs, pgw_rmfs_fd *fd);
pgw_err pgw_rmfs_statfs(pgw_rmfs *fs, const pgw_fs_cred *cred,
                        pgw_fs_statvfs *out);

/* Guest-local POSIX byte-range locks (FS-14). */
typedef enum pgw_lock_type {
    PGW_LOCK_RD = 0,
    PGW_LOCK_WR = 1,
    PGW_LOCK_UN = 2
} pgw_lock_type;

pgw_err pgw_rmfs_lock(pgw_rmfs *fs, const char *path, uint32_t owner_pid,
                      pgw_lock_type type, uint64_t start, uint64_t len);
pgw_err pgw_rmfs_getlock(pgw_rmfs *fs, const char *path, uint32_t owner_pid,
                         pgw_lock_type *type, uint64_t *start, uint64_t *len,
                         uint32_t *holder_pid);
void pgw_rmfs_unlock_pid(pgw_rmfs *fs, uint32_t owner_pid);

/* Priority queue scheduler used when transport slots are saturated (QNX-04,
 * PERF-05): ensures high-priority requests overtake low-priority bulk I/O. */
typedef struct pgw_prio_sched pgw_prio_sched;
typedef void (*pgw_prio_work_fn)(void *ctx);

pgw_prio_sched *pgw_prio_sched_create(uint32_t max_inflight);
void pgw_prio_sched_destroy(pgw_prio_sched *s);
pgw_err pgw_prio_sched_enqueue(pgw_prio_sched *s, uint32_t prio,
                               pgw_prio_work_fn fn, void *ctx);
size_t pgw_prio_sched_drain_next(pgw_prio_sched *s);
size_t pgw_prio_sched_pending(const pgw_prio_sched *s);

/* Adapters hooking proto9p and protofuse into pgw_rmfs. */
pgw_rmfs *pgw_rmfs_from_9p(void *p9_session, const pgw_fs_opts *opts);
pgw_rmfs *pgw_rmfs_from_fuse(void *fuse_session, const pgw_fs_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* PGW_RMFS_H */

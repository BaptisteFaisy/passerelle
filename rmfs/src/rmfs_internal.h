/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/src/rmfs_internal.h - Internal state of the layer 3 resource manager.
 *
 * Implements requirements FS-01..FS-15, QNX-04, QNX-05, QNX-08, PERF-05,
 * SEC-01 and ROB-02.
 *
 * The public contract pgw/rmfs.h is never modified: everything private to the
 * implementation lives here.
 */
#ifndef RMFS_INTERNAL_H
#define RMFS_INTERNAL_H

#include "attr_cache.h"
#include "lock.h"
#include "open_table.h"
#include "pgw/rmfs.h"
#include "pgw/rmfs_glue.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* PGW_RMFS_O_EXEC, the guest-only execution flag, comes from pgw/rmfs_glue.h
 * (it enforces `noexec` / `nosuid`, FS-09, SEC-01). */

struct pgw_rmfs {
    const pgw_fs_backend_ops *ops;
    void *impl;
    pgw_fs_opts opts;
    rmfs_open_table *fds;
    rmfs_attr_cache *cache;
    rmfs_lock_table *locks;
    pgw_prio_sched *sched;
    uint32_t inflight; /* backend requests currently in flight (QNX-04) */
};

/* --- permission helpers (FS-09) ----------------------------------------- */

/* `base` is one of 0400 (read), 0200 (write), 0100 (execute). The owner
 * bits are used when the caller owns the file, the group bits when the
 * caller belongs to the file group, the other bits otherwise. uid 0 (root)
 * bypasses the check. */
bool rmfs_perm_ok(const pgw_fs_attr *attr, const pgw_fs_cred *cred,
                  uint32_t base);

/* True when the open flags request a write access. */
bool rmfs_write_requested(uint32_t pgw_oflags);

/* Enforce `noexec` and `nosuid` (SEC-01). Returns PGW_EACCES when the
 * request must be refused. */
pgw_err rmfs_check_exec(const pgw_rmfs *fs, const pgw_fs_attr *attr,
                        uint32_t pgw_oflags);

/* --- backend submission through the priority queue (QNX-04, PERF-05) ----- */

/* Run `run` through the priority scheduler: the request is queued with the
 * caller's priority and the queue is drained in priority order, so a
 * high-priority request overtakes the pending bulk I/O. */
pgw_err rmfs_run(pgw_rmfs *fs, uint32_t prio, pgw_err (*run)(void *arg),
                 void *arg);

/* Cache key of a path, distinguishing "follow" from "do not follow" so that
 * lstat and stat never answer from each other's entry (FS-13). */
void cache_key(char *out, size_t cap, bool follow, const char *path);

/* --- attribute access with the optional cache (FS-12, FS-13) ------------- */

pgw_err rmfs_getattr_cached(pgw_rmfs *fs, const char *relpath, uint32_t prio,
                            bool follow, pgw_fs_attr *out);
void rmfs_cache_invalidate(pgw_rmfs *fs, const char *relpath);

/* --- backend call arguments (one struct, one invoker per vtable entry) --- */

/* Every backend call is expressed as an rmfs_args plus a one-line invoker so
 * that the submission path (priority queue) is written exactly once. */
typedef struct rmfs_args {
    pgw_rmfs *fs;
    uint32_t prio;
    const char *p1;
    const char *p2;
    uint64_t off;
    uint64_t handle;
    void *buf;         /* read destination */
    const void *cbuf;  /* write source */
    size_t len;
    size_t *out_n;
    uint32_t flags;
    uint32_t mode;
    pgw_fs_attr *attr;
    pgw_fs_setattr sa;
    pgw_fs_statvfs *stvfs;
    pgw_fs_dirent *dirents;
    size_t max_entries;
    size_t *out_count;
    uint64_t *next_off;
    char *target;
    size_t cap;
} rmfs_args;

pgw_err call_lookup(void *v);
pgw_err call_getattr(void *v);
pgw_err call_setattr(void *v);
pgw_err call_open(void *v);
pgw_err call_close(void *v);
pgw_err call_read(void *v);
pgw_err call_write(void *v);
pgw_err call_readdir(void *v);
pgw_err call_mkdir(void *v);
pgw_err call_unlink(void *v);
pgw_err call_rename(void *v);
pgw_err call_symlink(void *v);
pgw_err call_readlink(void *v);
pgw_err call_link(void *v);
pgw_err call_fsync(void *v);
pgw_err call_statfs(void *v);

/* --- shared checks ------------------------------------------------------- */

/* PGW_EROFS on a read-only mount, PGW_OK otherwise (FS-09). */
pgw_err rmfs_require_writable(const pgw_rmfs *fs);

/* Write permission on the parent directory of `resolved` (FS-09). An
 * unknown parent is left to the server: the check is a guest-side guard,
 * not a second implementation of the backend. */
pgw_err rmfs_check_parent_write(pgw_rmfs *fs, const pgw_fs_cred *cred,
                                 const char *resolved, uint32_t prio);

/* Resolve `path` then read its attributes in one step: the pair every
 * operation needs before it can check a permission (FS-09). */
pgw_err rmfs_resolve_and_stat(pgw_rmfs *fs, const char *path, uint32_t prio,
                              bool follow, char *resolved, size_t cap,
                              pgw_fs_attr *attr);

/* Counter bookkeeping shared by every operation (QNX-08). */
void rmfs_count(pgw_err err);

/* --- misc ---------------------------------------------------------------- */

/* Resolve `path` for an operation: normalizes it and applies the symlink
 * redirections (locally for the internal path, the QNX client restarts the
 * resolution itself - spec 5.3). */
pgw_err rmfs_resolve(pgw_rmfs *fs, const char *path, uint32_t prio,
                     bool follow_final, char *out, size_t cap);

#endif /* RMFS_INTERNAL_H */

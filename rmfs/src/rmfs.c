/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/src/rmfs.c - Lifecycle, mount options, permissions and submission.
 *
 * Implements requirements FS-09, FS-12, FS-13, QNX-04, QNX-05, QNX-08,
 * PERF-05, ROB-02 and SEC-01:
 *   - mount option defaults (nosuid on by default, SEC-01) and clamping of
 *     the attribute cache TTL to PGW_ATTR_CACHE_NS_MAX (FS-13)
 *   - permission checks against the caller identity (FS-09)
 *   - noexec / nosuid enforcement (SEC-01)
 *   - backend submission through the priority scheduler (QNX-04, PERF-05)
 *   - release of everything a dead client held (ROB-02)
 */
#include "rmfs_internal.h"

#include "path.h"
#include "pgw/limits.h"
#include "pgw/log.h"
#include "pgw/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pgw_fs_opts_default(pgw_fs_opts *opts)
{
    if (opts == NULL)
        return;
    opts->readonly = false;
    opts->noexec = false;
    opts->nosuid = true; /* SEC-01: no privilege elevation by default */
    opts->attr_ttl_ns = PGW_ATTR_CACHE_NS_DEFAULT;
    opts->max_inflight = PGW_INFLIGHT_DEFAULT;
}

static void rmfs_opts_normalize(pgw_fs_opts *o)
{
    if (o->attr_ttl_ns > PGW_ATTR_CACHE_NS_MAX)
        o->attr_ttl_ns = PGW_ATTR_CACHE_NS_MAX; /* hard ceiling (FS-13) */
    if (o->max_inflight == 0)
        o->max_inflight = PGW_INFLIGHT_DEFAULT;
    if (o->max_inflight > PGW_INFLIGHT_MAX)
        o->max_inflight = PGW_INFLIGHT_MAX;
}

pgw_rmfs *pgw_rmfs_create(const pgw_fs_backend_ops *ops, void *impl,
                          const pgw_fs_opts *opts)
{
    if (ops == NULL)
        return NULL;
    pgw_rmfs *fs = (pgw_rmfs *)calloc(1, sizeof(*fs));
    if (fs == NULL)
        return NULL;
    fs->ops = ops;
    fs->impl = impl;
    pgw_fs_opts_default(&fs->opts);
    if (opts != NULL)
        fs->opts = *opts;
    rmfs_opts_normalize(&fs->opts);

    fs->fds = rmfs_open_table_create();
    fs->locks = rmfs_lock_table_create();
    fs->cache = rmfs_attr_cache_create(fs->opts.attr_ttl_ns);
    fs->sched = pgw_prio_sched_create(fs->opts.max_inflight);
    if (fs->fds == NULL || fs->locks == NULL || fs->cache == NULL ||
        fs->sched == NULL) {
        pgw_rmfs_destroy(fs);
        return NULL;
    }
    PGW_DBG("rmfs: mount created (ro=%d noexec=%d nosuid=%d ttl=%llu ns)",
            (int)fs->opts.readonly, (int)fs->opts.noexec, (int)fs->opts.nosuid,
            (unsigned long long)fs->opts.attr_ttl_ns);
    return fs;
}

void pgw_rmfs_destroy(pgw_rmfs *fs)
{
    if (fs == NULL)
        return;
    if (fs->fds != NULL)
        rmfs_open_table_destroy(fs->fds, fs->ops, fs->impl);
    rmfs_attr_cache_destroy(fs->cache);
    rmfs_lock_table_destroy(fs->locks);
    pgw_prio_sched_destroy(fs->sched);
    if (fs->ops->destroy != NULL)
        fs->ops->destroy(fs->impl);
    free(fs);
}

const pgw_fs_opts *pgw_rmfs_opts(const pgw_rmfs *fs)
{
    return (fs != NULL) ? &fs->opts : NULL;
}

size_t pgw_rmfs_open_count(const pgw_rmfs *fs)
{
    return (fs != NULL) ? rmfs_open_table_count(fs->fds) : 0;
}

/* Cache key of a path, distinguishing "follow" from "do not follow" so that
 * lstat and stat never answer from each other's entry. */
void cache_key(char *out, size_t cap, bool follow, const char *path)
{
    snprintf(out, cap, "%c:%s", follow ? 's' : 'l', path);
}

/* --- permissions ---------------------------------------------------------- */

bool rmfs_perm_ok(const pgw_fs_attr *attr, const pgw_fs_cred *cred,
                  uint32_t base)
{
    if (attr == NULL || cred == NULL)
        return false;
    if (cred->uid == 0)
        return true; /* root inside the guest */
    if (attr->uid == cred->uid)
        return (attr->mode & base) != 0;
    if (attr->gid == cred->gid)
        return (attr->mode & (base >> 3)) != 0;
    return (attr->mode & (base >> 6)) != 0;
}

bool rmfs_write_requested(uint32_t pgw_oflags)
{
    return (pgw_oflags & (PGW_O_WRONLY | PGW_O_RDWR | PGW_O_TRUNC |
                          PGW_O_APPEND | PGW_O_CREAT)) != 0;
}

pgw_err rmfs_check_exec(const pgw_rmfs *fs, const pgw_fs_attr *attr,
                        uint32_t pgw_oflags)
{
    if (fs == NULL || attr == NULL)
        return PGW_EINVAL;
    if ((pgw_oflags & PGW_RMFS_O_EXEC) == 0)
        return PGW_OK;
    if (fs->opts.noexec)
        return PGW_EACCES; /* FS-09: noexec mount */
    if (fs->opts.nosuid && (attr->mode & (PGW_S_ISUID | PGW_S_ISGID)) != 0)
        return PGW_EACCES; /* SEC-01: no privilege elevation */
    return PGW_OK;
}

/* --- submission through the priority scheduler ---------------------------- */

typedef struct rmfs_work {
    pgw_err (*run)(void *arg);
    void *arg;
    pgw_err rc;
    bool done;
} rmfs_work;

static void rmfs_work_trampoline(void *v)
{
    rmfs_work *w = (rmfs_work *)v;
    w->rc = w->run(w->arg);
    w->done = true;
}

pgw_err rmfs_run(pgw_rmfs *fs, uint32_t prio, pgw_err (*run)(void *arg),
                 void *arg)
{
    if (fs == NULL || run == NULL)
        return PGW_EINVAL;

    /* Fast path: a transport slot is free, submit at once. No queue, no
     * allocation, no scheduling decision. */
    if (fs->inflight < fs->opts.max_inflight) {
        fs->inflight++;
        pgw_err rc = run(arg);
        fs->inflight--;
        return rc;
    }

    /* Saturated: the request is parked with the caller's priority, and the
     * pending requests leave in priority order - a high-priority 4 KiB read
     * overtakes the bulk low-priority copy (QNX-04, PERF-05). A waiting
     * request never holds a driver thread (QNX-05): the enqueue returns, the
     * work runs when a slot frees. */
    pgw_stat_add(PGW_STAT_QUEUE_FULL, 1);
    rmfs_work w = {run, arg, PGW_OK, false};
    pgw_err err = pgw_prio_sched_enqueue(fs->sched, prio, rmfs_work_trampoline,
                                         &w);
    if (err != PGW_OK)
        return err;
    while (!w.done) {
        if (pgw_prio_sched_drain_next(fs->sched) == 0)
            break;
    }
    return w.rc;
}

/* --- attribute cache ------------------------------------------------------ */

void rmfs_cache_invalidate(pgw_rmfs *fs, const char *relpath)
{
    char key[PGW_PATH_MAX + 2];
    if (fs == NULL || relpath == NULL)
        return;
    cache_key(key, sizeof(key), true, relpath);
    rmfs_attr_cache_invalidate(fs->cache, key);
    cache_key(key, sizeof(key), false, relpath);
    rmfs_attr_cache_invalidate(fs->cache, key);
}

pgw_err rmfs_resolve(pgw_rmfs *fs, const char *path, uint32_t prio,
                     bool follow_final, char *out, size_t cap)
{
    if (fs == NULL)
        return PGW_EINVAL;
    return rmfs_path_follow(fs->ops, fs->impl, path, prio, follow_final, out,
                            cap);
}

/* Backend getattr/lookup behind the priority scheduler. */
typedef struct getattr_args {
    pgw_rmfs *fs;
    const char *path;
    pgw_fs_attr *out;
    uint32_t prio;
    bool follow;
} getattr_args;

static pgw_err getattr_invoke(void *arg)
{
    getattr_args *a = (getattr_args *)arg;
    if (!a->follow && a->fs->ops->lookup != NULL)
        return a->fs->ops->lookup(a->fs->impl, a->path, a->out, a->prio);
    return a->fs->ops->getattr(a->fs->impl, a->path, a->out, a->prio);
}

pgw_err rmfs_getattr_cached(pgw_rmfs *fs, const char *relpath, uint32_t prio,
                            bool follow, pgw_fs_attr *out)
{
    if (fs == NULL || relpath == NULL || out == NULL)
        return PGW_EINVAL;
    char key[PGW_PATH_MAX + 2];
    cache_key(key, sizeof(key), follow, relpath);
    if (rmfs_attr_cache_get(fs->cache, key, out))
        return PGW_OK;
    getattr_args a = {fs, relpath, out, prio, follow};
    pgw_err err = rmfs_run(fs, prio, getattr_invoke, &a);
    if (err == PGW_OK)
        rmfs_attr_cache_put(fs->cache, key, out);
    return err;
}

/* --- shared checks ------------------------------------------------------- */

pgw_err rmfs_require_writable(const pgw_rmfs *fs)
{
    if (fs == NULL)
        return PGW_EINVAL;
    return fs->opts.readonly ? PGW_EROFS : PGW_OK;
}

void rmfs_count(pgw_err err)
{
    pgw_stat_add(PGW_STAT_REQ, 1);
    pgw_stat_add(PGW_STAT_RESP, 1);
    if (err != PGW_OK)
        pgw_stat_add(PGW_STAT_ERR, 1);
}

static void path_parent(const char *path, char *out, size_t cap)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        snprintf(out, cap, ".");
        return;
    }
    size_t len = (size_t)(slash - path);
    if (len == 0)
        len = 1; /* parent of "/x" is "/" */
    if (len >= cap)
        len = cap - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

pgw_err rmfs_check_parent_write(pgw_rmfs *fs, const pgw_fs_cred *cred,
                                const char *resolved, uint32_t prio)
{
    if (fs == NULL || cred == NULL || resolved == NULL)
        return PGW_EINVAL;
    char parent[PGW_PATH_MAX];
    path_parent(resolved, parent, sizeof(parent));
    pgw_fs_attr pa;
    if (rmfs_getattr_cached(fs, parent, prio, true, &pa) != PGW_OK)
        return PGW_OK; /* unknown parent: the backend is the judge */
    if (!rmfs_perm_ok(&pa, cred, 0200))
        return PGW_EACCES;
    return PGW_OK;
}

pgw_err rmfs_resolve_and_stat(pgw_rmfs *fs, const char *path, uint32_t prio,
                              bool follow, char *resolved, size_t cap,
                              pgw_fs_attr *attr)
{
    if (fs == NULL || path == NULL || resolved == NULL || attr == NULL)
        return PGW_EINVAL;

    /* Served by the attribute cache when it is enabled (FS-13). */
    char norm[PGW_PATH_MAX];
    pgw_err err = rmfs_path_normalize(path, norm, sizeof(norm));
    if (err != PGW_OK)
        return err;
    char key[PGW_PATH_MAX + 2];
    cache_key(key, sizeof(key), follow, norm);
    if (rmfs_attr_cache_get(fs->cache, key, attr)) {
        snprintf(resolved, cap, "%s", norm);
        return PGW_OK;
    }

    /* One pass: the walk that looks for symlinks already asked the server for
     * the last element, so its answer is reused instead of a second request
     * (PERF-02: one round trip per component, no more). */
    rmfs_redirect redir;
    bool have = false;
    err = rmfs_path_resolve_stat(fs->ops, fs->impl, norm, prio, follow,
                                 resolved, cap, &redir, attr, &have);
    if (err != PGW_OK)
        return err;
    if (redir.is_redirect) {
        /* A link is still in the way after following: resolve it for real. */
        err = rmfs_path_follow(fs->ops, fs->impl, norm, prio, follow, resolved,
                               cap);
        if (err != PGW_OK)
            return err;
        have = false;
    }
    if (!have) {
        getattr_args a = {fs, resolved, attr, prio, follow};
        err = rmfs_run(fs, prio, getattr_invoke, &a);
        if (err != PGW_OK)
            return err;
    }
    rmfs_attr_cache_put(fs->cache, key, attr);
    return PGW_OK;
}

/* --- ROB-02: release everything a dead client held ------------------------ */

pgw_err pgw_rmfs_release_pid(pgw_rmfs *fs, uint32_t owner_pid)
{
    if (fs == NULL)
        return PGW_EINVAL;
    rmfs_open_table_release_pid(fs->fds, owner_pid, fs->ops, fs->impl);
    rmfs_lock_release_pid(fs->locks, owner_pid);
    rmfs_attr_cache_clear(fs->cache);
    PGW_DBG("rmfs: released client pid %u (%zu fds still open)", owner_pid,
            rmfs_open_table_count(fs->fds));
    return PGW_OK;
}

void pgw_rmfs_fd_set_owner(pgw_rmfs_fd *fd, uint32_t owner_pid)
{
    rmfs_open_table_set_owner(fd, owner_pid);
}

pgw_err pgw_rmfs_fd_ref(pgw_rmfs_fd *fd)
{
    if (fd == NULL)
        return PGW_EINVAL;
    rmfs_open_table_ref(fd);
    return PGW_OK;
}

uint32_t pgw_rmfs_fd_refcount(const pgw_rmfs_fd *fd)
{
    return (fd != NULL) ? fd->refcount : 0;
}

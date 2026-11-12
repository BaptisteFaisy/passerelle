/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/src/path.c - Path normalization and symlink resolution.
 *
 * Implements requirements FS-08, FS-15, and specification 5.3:
 *   - Rejects empty names (PGW_ENOENT) and components > PGW_NAME_MAX or
 *     paths > PGW_PATH_MAX (PGW_ENAMETOOLONG) (FS-15)
 *   - Normalizes '//', '.', '..'; ".." past the root clamps to the root
 *   - A symlink met mid-path (or at the end when following) produces a
 *     redirect response: target + remaining path. The driver never follows
 *     the link itself (spec 5.3: the QNX client restarts the resolution)
 *   - rmfs_path_follow() applies redirects locally for the PC test build and
 *     for internal operations, capped at PGW_SYMLINK_MAX (FS-08 -> ELOOP)
 */
#include "path.h"

#include "pgw/limits.h"
#include "pgw/log.h"

#include <stdio.h>
#include <string.h>

/* Append one component to `out`, which is being built in place. */
static pgw_err path_push(char *out, size_t cap, size_t *pos, const char *seg,
                         size_t len)
{
    if (*pos > 0) {
        if (*pos + 1 >= cap)
            return PGW_ENAMETOOLONG;
        out[(*pos)++] = '/';
    }
    if (*pos + len >= cap)
        return PGW_ENAMETOOLONG;
    memcpy(out + *pos, seg, len);
    *pos += len;
    out[*pos] = '\0';
    return PGW_OK;
}

/* Drop the last component of the path being built in place (".." handling). */
static void path_pop(char *out, size_t *pos)
{
    while (*pos > 0 && out[*pos - 1] != '/')
        (*pos)--;
    if (*pos > 0)
        (*pos)--; /* also drop the separator */
    out[*pos] = '\0';
}

pgw_err rmfs_path_normalize(const char *in, char *out, size_t out_cap)
{
    if (in == NULL || out == NULL || out_cap < 2)
        return PGW_EINVAL;
    if (in[0] == '\0')
        return PGW_ENOENT;

    /* Single pass, in place: no component array, so no arbitrary depth cap
     * and no large stack frame. */
    size_t pos = 0;
    out[0] = '\0';
    const char *p = in;
    while (*p != '\0') {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        const char *start = p;
        while (*p != '\0' && *p != '/')
            p++;
        size_t len = (size_t)(p - start);
        if (len > PGW_NAME_MAX)
            return PGW_ENAMETOOLONG; /* FS-15 */
        if (len == 1 && start[0] == '.')
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            path_pop(out, &pos); /* ".." past the root clamps to the root */
            continue;
        }
        pgw_err err = path_push(out, out_cap, &pos, start, len);
        if (err != PGW_OK)
            return err;
    }
    if (pos == 0) {
        out[0] = '.';
        out[1] = '\0';
    }
    return PGW_OK;
}

/* Probe one path component **without following** a final symlink: the walk has
 * to see the link to turn it into a redirect (spec 5.3, FS-08). Per the backend
 * convention (pgw/rmfs.h) that is `lookup`; `getattr` is the fallback for an
 * adapter that only provides one of the two. */
static pgw_err probe_attr(const pgw_fs_backend_ops *ops, void *impl,
                          const char *rel, uint32_t prio, pgw_fs_attr *out)
{
    if (ops == NULL)
        return PGW_ENOSYS;
    if (ops->lookup != NULL)
        return ops->lookup(impl, rel, out, prio);
    if (ops->getattr != NULL)
        return ops->getattr(impl, rel, out, prio);
    return PGW_ENOSYS;
}

/* Fill the redirect response for a symlink met at `prefix` (spec 5.3). */
/* A relative link target is relative to the directory that holds the link, not
 * to the share root: POSIX resolves `d2/l -> d1/f` as `d2/d1/f`. Rebasing the
 * target here keeps redirect_join() - and every caller - free of that rule.
 * An absolute target (leading '/') and a link sitting at the root need nothing.
 * A target too long to rebase is left alone: the resolution that follows fails
 * loudly instead of silently pointing somewhere else. */
static void rebase_relative_target(const char *link_path, char *target,
                                   size_t cap)
{
    if (target[0] == '\0' || target[0] == '/')
        return;
    char dir[PGW_PATH_MAX];
    char name[PGW_NAME_MAX + 1];
    if (rmfs_split_path(link_path, dir, sizeof(dir), name, sizeof(name)) != PGW_OK)
        return;
    if (dir[0] == '\0' || strcmp(dir, ".") == 0)
        return;
    char rebased[PGW_PATH_MAX];
    int n = snprintf(rebased, sizeof(rebased), "%s/%s", dir, target);
    if (n <= 0 || (size_t)n >= sizeof(rebased))
        return;
    snprintf(target, cap, "%s", rebased);
}

static pgw_err make_redirect(const pgw_fs_backend_ops *ops, void *impl,
                             const char *prefix, const char *rest,
                             uint32_t prio, rmfs_redirect *redir)
{
    redir->is_redirect = true;
    redir->remainder[0] = '\0';
    if (rest != NULL && rest[0] != '\0')
        snprintf(redir->remainder, sizeof(redir->remainder), "%s", rest);
    redir->target[0] = '\0';
    if (ops->readlink != NULL) {
        pgw_err e = ops->readlink(impl, prefix, redir->target,
                                  sizeof(redir->target), prio);
        if (e != PGW_OK) {
            redir->is_redirect = false;
            return e;
        }
    }
    rebase_relative_target(prefix, redir->target, sizeof(redir->target));
    return PGW_OK;
}

/* Bounded append: copies at most `cap - 1 - *pos` bytes of `src`, keeps the
 * result NUL-terminated and advances *pos. False when the path would overflow. */
static bool path_append(char *dst, size_t cap, size_t *pos, const char *src,
                        size_t len)
{
    if (*pos + len + 1 > cap)
        return false;
    memcpy(dst + *pos, src, len);
    *pos += len;
    dst[*pos] = '\0';
    return true;
}

pgw_err rmfs_path_resolve_stat(const pgw_fs_backend_ops *ops, void *impl,
                               const char *path, uint32_t prio,
                               bool follow_final, char *resolved,
                               size_t res_cap, rmfs_redirect *redir,
                               pgw_fs_attr *final_attr, bool *have_attr)
{
    if (path == NULL || resolved == NULL || res_cap == 0)
        return PGW_EINVAL;
    if (redir != NULL)
        redir->is_redirect = false;
    if (have_attr != NULL)
        *have_attr = false;

    char norm[PGW_PATH_MAX];
    pgw_err err = rmfs_path_normalize(path, norm, sizeof(norm));
    if (err != PGW_OK)
        return err;
    if (strcmp(norm, ".") == 0 || redir == NULL) {
        snprintf(resolved, res_cap, "%s", norm);
        return PGW_OK;
    }

    char prefix[PGW_PATH_MAX];
    size_t pos = 0;
    prefix[0] = '\0';
    const char *p = norm;
    while (*p != '\0') {
        const char *slash = strchr(p, '/');
        size_t seg_len = (slash != NULL) ? (size_t)(slash - p) : strlen(p);
        if (pos > 0 && !path_append(prefix, sizeof(prefix), &pos, "/", 1))
            return PGW_ENAMETOOLONG;
        if (!path_append(prefix, sizeof(prefix), &pos, p, seg_len))
            return PGW_ENAMETOOLONG;

        bool is_last = (slash == NULL);
        pgw_fs_attr attr;
        pgw_err perr = probe_attr(ops, impl, prefix, prio, &attr);
        if (perr == PGW_OK && attr.type == PGW_FT_LNK &&
            (!is_last || follow_final)) {
            const char *rest = is_last ? "" : slash + 1;
            snprintf(resolved, res_cap, "%s", norm);
            return make_redirect(ops, impl, prefix, rest, prio, redir);
        }
        if (is_last) {
            if (perr == PGW_OK && final_attr != NULL && have_attr != NULL) {
                *final_attr = attr; /* reuse it: no second round trip */
                *have_attr = true;
            }
            break;
        }
        p = slash + 1;
    }
    snprintf(resolved, res_cap, "%s", norm);
    return PGW_OK;
}

pgw_err rmfs_path_resolve(const pgw_fs_backend_ops *ops, void *impl,
                          const char *path, uint32_t prio, bool follow_final,
                          char *resolved, size_t res_cap, rmfs_redirect *redir)
{
    return rmfs_path_resolve_stat(ops, impl, path, prio, follow_final, resolved,
                                  res_cap, redir, NULL, NULL);
}

/* Join a redirect into the next path to resolve: target [+ "/" + remainder]. */
static pgw_err redirect_join(const rmfs_redirect *redir, char *out, size_t cap)
{
    if (redir->target[0] == '\0')
        return PGW_EINVAL;
    size_t pos = 0;
    out[0] = '\0';
    if (!path_append(out, cap, &pos, redir->target, strlen(redir->target)))
        return PGW_ENAMETOOLONG;
    if (redir->remainder[0] != '\0') {
        if (!path_append(out, cap, &pos, "/", 1))
            return PGW_ENAMETOOLONG;
        if (!path_append(out, cap, &pos, redir->remainder,
                         strlen(redir->remainder)))
            return PGW_ENAMETOOLONG;
    }
    return PGW_OK;
}

pgw_err rmfs_path_follow(const pgw_fs_backend_ops *ops, void *impl,
                         const char *path, uint32_t prio, bool follow_final,
                         char *resolved, size_t res_cap)
{
    char cur[PGW_PATH_MAX];
    snprintf(cur, sizeof(cur), "%s", path != NULL ? path : "");
    for (uint32_t depth = 0; depth <= PGW_SYMLINK_MAX; depth++) {
        rmfs_redirect redir;
        pgw_err err = rmfs_path_resolve(ops, impl, cur, prio, follow_final,
                                        resolved, res_cap, &redir);
        if (err != PGW_OK)
            return err;
        if (!redir.is_redirect)
            return PGW_OK;
        err = redirect_join(&redir, cur, sizeof(cur));
        if (err != PGW_OK)
            return err;
    }
    return PGW_ELOOP; /* symlink loop, FS-08 */
}

pgw_err rmfs_split_path(const char *rel, char *dir, size_t dir_cap, char *name,
                        size_t name_cap)
{
    if (rel == NULL || dir == NULL || name == NULL || dir_cap == 0 ||
        name_cap == 0)
        return PGW_EINVAL;
    if (rel[0] == '\0' || strcmp(rel, ".") == 0)
        return PGW_EINVAL;
    const char *slash = strrchr(rel, '/');
    if (slash == NULL) {
        snprintf(dir, dir_cap, "%s", ".");
        snprintf(name, name_cap, "%s", rel);
        return PGW_OK;
    }
    size_t dlen = (size_t)(slash - rel);
    if (dlen == 0)
        snprintf(dir, dir_cap, "%s", ".");
    else if (dlen >= dir_cap)
        return PGW_ENAMETOOLONG;
    else {
        memcpy(dir, rel, dlen);
        dir[dlen] = '\0';
    }
    snprintf(name, name_cap, "%s", slash + 1);
    return PGW_OK;
}

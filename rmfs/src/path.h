/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/src/path.h - Path resolution, normalization, and symlink redirection.
 *
 * Implements requirements FS-08, FS-15, and specification 5.3:
 *   - Path normalization (collapsing '//', '.', '..')
 *   - Bounds checks: component length <= PGW_NAME_MAX (FS-15)
 *   - Symlink handling: a symlink met on the way yields a *redirect response*
 *     (link target + remaining path); the driver never follows it itself
 *     (spec 5.3: under QNX the client restarts the resolution)
 *   - Loop detection capped at PGW_SYMLINK_MAX redirects (FS-08) -> PGW_ELOOP
 */
#ifndef RMFS_PATH_H
#define RMFS_PATH_H

#include "pgw/limits.h"
#include "pgw/rmfs.h"

#include <stdbool.h>

/* Redirect response, the mirror of QNX _IO_RESOLVE_LINK (spec 5.3): the
 * driver hands the symlink target plus the unresolved remainder back to the
 * client, which restarts resolution itself. */
typedef struct rmfs_redirect {
    bool is_redirect;
    char target[PGW_PATH_MAX];   /* symlink body */
    char remainder[PGW_PATH_MAX]; /* path elements after the link, may be "" */
} rmfs_redirect;

/* Normalize `in` into `out`: drop leading '/', collapse "//", ".", ".."
 * (".." past the root clamps to the root). Rejects empty names (PGW_ENOENT),
 * components > PGW_NAME_MAX and paths > PGW_PATH_MAX (PGW_ENAMETOOLONG).
 * The root normalizes to ".". */
pgw_err rmfs_path_normalize(const char *in, char *out, size_t out_cap);

/* One resolution step over a normalized path (spec 5.3).
 * Walks components asking the backend for each attribute (lstat semantics).
 * When a symlink is met before the last component, or at the last component
 * with follow_final, fills *redir (target + remainder) and returns PGW_OK
 * with redir->is_redirect = true: the caller decides what to do with it.
 * Otherwise copies the normalized path into `resolved`. */
pgw_err rmfs_path_resolve(const pgw_fs_backend_ops *ops, void *impl,
                          const char *path, uint32_t prio, bool follow_final,
                          char *resolved, size_t res_cap,
                          rmfs_redirect *redir);

/* Same, but also hands back the attributes the resolution already fetched for
 * the final component (`have_attr` says whether it did). This is what keeps a
 * stat at one round trip per component instead of two: the walk that looked
 * for symlinks already asked for the last element (PERF-02). */
pgw_err rmfs_path_resolve_stat(const pgw_fs_backend_ops *ops, void *impl,
                               const char *path, uint32_t prio,
                               bool follow_final, char *resolved,
                               size_t res_cap, rmfs_redirect *redir,
                               pgw_fs_attr *final_attr, bool *have_attr);

/* Fully resolve a path by applying redirects locally (used by the PC test
 * build and by stat/open inside rmfs). Each applied redirect counts against
 * PGW_SYMLINK_MAX; exceeding it yields PGW_ELOOP (FS-08). */
pgw_err rmfs_path_follow(const pgw_fs_backend_ops *ops, void *impl,
                         const char *path, uint32_t prio, bool follow_final,
                         char *resolved, size_t res_cap);

/* Split a normalized relative path into its parent directory ("" for the root)
 * and its last component. Both protocols need it: 9P works with a dfid plus a
 * name, FUSE with a parent nodeid plus a name. Returns PGW_EINVAL for "." or
 * an empty path (there is no name to operate on). */
pgw_err rmfs_split_path(const char *rel, char *dir, size_t dir_cap, char *name,
                        size_t name_cap);

#endif /* RMFS_PATH_H */

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_mount.h - rmfs mounted over the FUSE adapter.
 *
 * The FUSE adapter of rmfs was built but no bench had ever run through it
 * (gap 25 of docs/gap-analysis.md). This header is the mount the benches of
 * tests/fuse_e2e use: the reference FUSE server of tests/fuse_ref serves a
 * real directory (the truth), a session is attached to it and rmfs is built
 * on top of the FUSE adapter, so the layer sees the share the way a guest
 * would see it.
 *
 * The mount comes in two shapes:
 *   - fuse_mount_open(): a plain mount, for the directed cases;
 *   - fuse_diff_mount(): the provider the differential bench asks for its
 *     mount (diff.h), so the very same bench runs on the FUSE adapter and
 *     on the POSIX backend without a second copy of it.
 *
 * Covers VFS-05 (the same conformance bench through the FUSE adapter).
 */
#ifndef PGW_TESTS_FUSE_E2E_MOUNT_H
#define PGW_TESTS_FUSE_E2E_MOUNT_H

#include "cas.h"
#include "diff.h"

#include "pgw/rmfs.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct fuse_mount {
    pgw_rmfs *fs;   /* the layer under test */
    char root[512]; /* the served directory: the truth */
    void *ctx;      /* the reference server and its session */
} fuse_mount;

/* Start the reference server over a fresh directory, attach a session and
 * build rmfs on top of the FUSE adapter. `opts` NULL means the defaults. */
bool fuse_mount_open(fuse_mount *m, const pgw_fs_opts *opts);
void fuse_mount_close(fuse_mount *m);

/* The provider of the differential bench (diff.h). */
bool fuse_diff_mount(diff_mount *m, void *user, char *err, size_t err_cap);

#endif /* PGW_TESTS_FUSE_E2E_MOUNT_H */

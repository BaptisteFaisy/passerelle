/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * posix_backend.h - a pgw_fs_backend_ops implementation over a real directory.
 *
 * Why this exists: the differential conformance bench (spec 7.2) needs a
 * backend whose behaviour is the *truth*, so that whatever the layer under test
 * reports can be compared against it. On the PC that truth is a plain POSIX
 * directory; in a QNX guest the same bench runs against the real share. This
 * backend is test infrastructure, not shipped code: it is deliberately simple,
 * single-threaded and synchronous.
 *
 * Used by tests/e2e (differential and directed cases) and reusable by
 * host/fsproof --local, which runs the same suite through this backend.
 */
#ifndef PGW_TESTS_POSIX_BACKEND_H
#define PGW_TESTS_POSIX_BACKEND_H

#include "pgw/rmfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pgw_posix_backend pgw_posix_backend;

/* The vtable to hand to pgw_rmfs_create(). */
extern const pgw_fs_backend_ops pgw_posix_backend_ops;

/* Open a backend rooted at `root` (must exist). Returns NULL on failure. */
pgw_posix_backend *pgw_posix_backend_open(const char *root);
void pgw_posix_backend_close(pgw_posix_backend *b);

/* Absolute path of the root, for the tests that inspect the tree directly. */
const char *pgw_posix_backend_root(const pgw_posix_backend *b);

/* Number of handles currently open; must return to 0 at the end of a suite
 * (ROB-02, P9-07 style leak check). */
size_t pgw_posix_backend_open_handles(const pgw_posix_backend *b);

/* Join root + relpath into `out` (bounded). Returns false when the result
 * would not fit or when `relpath` escapes the root (".." component). */
bool pgw_posix_join(const pgw_posix_backend *b, const char *relpath,
                    char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PGW_TESTS_POSIX_BACKEND_H */

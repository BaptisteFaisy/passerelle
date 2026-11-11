/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * testfs.h - shared fixture: a temporary tree, a POSIX backend and an rmfs.
 *
 * Every end-to-end test needs the same three objects wired together; this
 * fixture does it once so no test file re-implements the setup (and so a change
 * of mount options is made in exactly one place).
 */
#ifndef PGW_TESTS_TESTFS_H
#define PGW_TESTS_TESTFS_H

#include "posix_backend.h"

#include "pgw/rmfs.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct testfs {
    char *root;              /* temporary directory, the "PC folder" */
    pgw_posix_backend *be;   /* truth backend rooted there */
    pgw_rmfs *fs;            /* layer under test */
    pgw_fs_cred cred;        /* root caller, priority 10 */
} testfs;

/* Create the fixture. `opts` NULL means the defaults (no attribute cache, no
 * data cache, nosuid). `tag` names the temporary directory in /tmp. */
bool testfs_open(testfs *t, const char *tag, const pgw_fs_opts *opts);

/* Destroy the filesystem and the backend; the temporary directory itself is
 * removed when the process exits (pgw_test_tmpdir). */
void testfs_close(testfs *t);

/* Absolute path of an entry inside the fixture root. */
void testfs_abs(const testfs *t, const char *rel, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PGW_TESTS_TESTFS_H */

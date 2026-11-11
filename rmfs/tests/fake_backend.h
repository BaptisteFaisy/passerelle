/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/tests/fake_backend.h - A backend over a real temporary directory.
 *
 * The conformance bench of layer 3 must not depend on a protocol: this fake
 * implements pgw_fs_backend_ops directly on top of a directory created by
 * pgw_test_tmpdir(), so the same assertions that will run inside a QNX guest
 * against 9P or virtiofs run on the PC in milliseconds (FS-01..FS-15).
 *
 * It also counts the backend calls it receives, which is what lets a test
 * prove that the attribute cache actually avoided a round trip (FS-13) and
 * that no data is ever cached (D4, FS-12).
 */
#ifndef RMFS_TESTS_FAKE_BACKEND_H
#define RMFS_TESTS_FAKE_BACKEND_H

#include "pgw/rmfs.h"

#include <stddef.h>

#define FAKE_MAX_OPEN 32

typedef struct fake_backend {
    char root[512];
    const pgw_fs_backend_ops *ops; /* vtable to hand to pgw_rmfs_create() */
    size_t lookup_calls;
    size_t getattr_calls;
    size_t attr_calls;    /* lookup + getattr: every attribute round trip */
    size_t open_calls;
    size_t close_calls;
    size_t read_calls;
    size_t write_calls;
    size_t readdir_calls;
    size_t unlink_calls;
    size_t destroyed;
    uint32_t last_prio;   /* priority of the most recent backend call */
    bool fail_next;      /* make the next call fail with `fail_err` */
    pgw_err fail_err;
    bool hide_getattr;   /* make getattr report ENOENT (simulated loss) */
} fake_backend;

/* Create the temporary directory and the backend. NULL on failure. */
fake_backend *fake_backend_new(void);
void fake_backend_free(fake_backend *b);

/* Absolute host path of a backend-relative path, into `out`. */
void fake_backend_host_path(const fake_backend *b, const char *rel, char *out,
                            size_t cap);

/* Write a file directly on the host side, bypassing the backend: this is the
 * "modified on the PC" scenario of FS-12. */
int fake_backend_host_write(const fake_backend *b, const char *rel,
                            const char *data, size_t len);

#endif /* RMFS_TESTS_FAKE_BACKEND_H */

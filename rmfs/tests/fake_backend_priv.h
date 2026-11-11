/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/tests/fake_backend_priv.h - Internals shared by the fake backend.
 *
 * The fake backend is split in two translation units (lifecycle / path
 * helpers, then the vtable) so that no file of the test support crosses the
 * project's 500-line rule. Everything both halves need lives here.
 */
#ifndef RMFS_TESTS_FAKE_BACKEND_PRIV_H
#define RMFS_TESTS_FAKE_BACKEND_PRIV_H

#include "fake_backend.h"

#include "pgw/err.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

/* One open backend handle: a host fd, or a DIR* for a directory. This is the
 * analogue of a 9P fid / FUSE fh: an opaque small integer to layer 3. */
typedef struct fake_handle {
    int used;
    int is_dir;
    int fd;
    DIR *dirp;
} fake_handle;

/* The handle table follows the fake_backend struct in the same allocation. */
static inline fake_handle *fake_handles(void *impl)
{
    return (fake_handle *)(void *)((char *)impl + sizeof(fake_backend));
}

/* Fill `out` from a host `struct stat` (nanosecond timestamps, FS-02). */
void fake_fill_attr(pgw_fs_attr *out, const struct stat *st);

/* Consume a pending injected failure. Returns true when the call must fail. */
bool fake_take_failure(fake_backend *b, pgw_err *out);

/* Handle table access: `handle` is 1-based, 0 means "none". */
fake_handle *fake_handle_at(void *impl, uint64_t handle);

/* Install the vtable into `ops` (called once by fake_backend_new). */
void fake_backend_install_ops(pgw_fs_backend_ops *ops);

#endif /* RMFS_TESTS_FAKE_BACKEND_PRIV_H */

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * backend_priv.h - internals shared by the two halves of the POSIX backend.
 *
 * The backend is split in two translation units so that neither file grows past
 * the project's 500-line limit: backend_file.c owns the file-side callbacks
 * (open/read/write/fsync/attributes/statfs), backend_tree.c owns the
 * namespace-side callbacks (mkdir/unlink/rename/symlink/readlink/link/readdir),
 * the unified open() dispatch and the published vtable.
 */
#ifndef PGW_TESTS_BACKEND_PRIV_H
#define PGW_TESTS_BACKEND_PRIV_H

#include "posix_backend.h"

#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#define PGW_BE_MAX_HANDLES 256
#define PGW_BE_MAX_DIR_ENTRIES 4096
#define PGW_BE_PATH_MAX 4096

/* One open handle. A directory handle snapshots its entries at open time so
 * that readdir offsets are stable across calls (FS-03). */
typedef struct be_handle {
    bool in_use;
    bool is_dir;
    int fd; /* file descriptor, -1 for directories */
    char path[PGW_BE_PATH_MAX];
    char (*names)[256]; /* directory snapshot */
    uint8_t *types;
    size_t n_entries;
} be_handle;

struct pgw_posix_backend {
    char root[PGW_BE_PATH_MAX];
    size_t root_len;
    be_handle handles[PGW_BE_MAX_HANDLES];
};

/* --- handle table (backend_file.c) ---------------------------------------- */

be_handle *be_handle_alloc(pgw_posix_backend *b);
void be_handle_free(be_handle *h);
be_handle *be_handle_get(pgw_posix_backend *b, uint64_t handle);

/* Fill a pgw_fs_attr from an already-obtained struct stat. */
void be_fill_attr(const struct stat *st, pgw_fs_attr *out);

/* --- file-side callbacks (backend_file.c) --------------------------------- */

pgw_err be_file_lookup(void *impl, const char *relpath, pgw_fs_attr *out,
                       uint32_t prio);
pgw_err be_file_getattr(void *impl, const char *relpath, pgw_fs_attr *out,
                        uint32_t prio);
pgw_err be_file_setattr(void *impl, const char *relpath,
                        const pgw_fs_setattr *sa, uint32_t prio);
pgw_err be_file_read(void *impl, uint64_t handle, uint64_t off, void *buf,
                     size_t len, size_t *out_read, uint32_t prio);
pgw_err be_file_write(void *impl, uint64_t handle, uint64_t off,
                      const void *buf, size_t len, size_t *out_written,
                      uint32_t prio);
pgw_err be_file_fsync(void *impl, uint64_t handle, uint32_t prio);
pgw_err be_file_statfs(void *impl, pgw_fs_statvfs *out, uint32_t prio);
pgw_err be_file_close(void *impl, uint64_t handle, uint32_t prio);

/* Open a regular file and store the handle id. Not exposed through the vtable:
 * be_tree_open() dispatches between this and the directory snapshot. */
pgw_err be_file_open_fd(pgw_posix_backend *b, const char *relpath,
                        uint32_t pgw_oflags, uint32_t create_mode,
                        uint64_t *out_handle);

/* --- tree-side callbacks (backend_tree.c) --------------------------------- */

pgw_err be_tree_open(void *impl, const char *relpath, uint32_t pgw_oflags,
                     uint32_t create_mode, uint64_t *out_handle, uint32_t prio);

#endif /* PGW_TESTS_BACKEND_PRIV_H */

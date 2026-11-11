/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/src/open_table.h - Open file descriptor table and refcount definitions.
 *
 * Implements requirements FS-11 and ROB-02:
 *   - Reference-counted open file descriptors
 *   - Deleted-while-open files remain readable and accessible via fd (FS-11)
 *   - Release of all fds opened by a deceased process/owner (ROB-02)
 */
#ifndef RMFS_OPEN_TABLE_H
#define RMFS_OPEN_TABLE_H

#include "pgw/limits.h"
#include "pgw/rmfs.h"

struct pgw_rmfs_fd {
    uint64_t handle;
    char path[PGW_PATH_MAX];
    uint32_t oflags;
    uint32_t mode;
    uint32_t owner_uid;
    uint32_t owner_gid;
    uint32_t owner_pid;
    uint32_t prio;      /* caller priority at open time (QNX-03) */
    uint32_t refcount;
    bool unlinked;
    struct pgw_rmfs_fd *next;
};

typedef struct rmfs_open_table rmfs_open_table;

rmfs_open_table *rmfs_open_table_create(void);
void rmfs_open_table_destroy(rmfs_open_table *t, const pgw_fs_backend_ops *ops,
                             void *impl);

pgw_rmfs_fd *rmfs_open_table_insert(rmfs_open_table *t, uint64_t handle,
                                    const char *path, uint32_t oflags,
                                    uint32_t mode, const pgw_fs_cred *cred);
void rmfs_open_table_ref(pgw_rmfs_fd *fd);
bool rmfs_open_table_remove(rmfs_open_table *t, pgw_rmfs_fd *fd);
size_t rmfs_open_table_count(const rmfs_open_table *t);
void rmfs_open_table_mark_unlinked(rmfs_open_table *t, const char *path);
void rmfs_open_table_set_owner(pgw_rmfs_fd *fd, uint32_t owner_pid);
void rmfs_open_table_rename(rmfs_open_table *t, const char *oldpath,
                            const char *newpath);
void rmfs_open_table_release_pid(rmfs_open_table *t, uint32_t owner_pid,
                                 const pgw_fs_backend_ops *ops, void *impl);

#endif /* RMFS_OPEN_TABLE_H */

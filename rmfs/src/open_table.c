/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/src/open_table.c - Table of open file descriptors with refcounting.
 *
 * Implements requirements FS-11 and ROB-02:
 *   - Each open fd tracks its backend handle, path, flags and caller credentials.
 *   - When a file is unlinked while open, it is marked unlinked but remains
 *     fully readable and valid until closed (FS-11).
 *   - Reference counting ensures dup-like sharing or multiple users release
 *     the backend handle only upon final close.
 *   - rmfs_open_table_release_pid() cleans up every descriptor held by a
 *     terminated process (ROB-02).
 */
#include "open_table.h"

#include "pgw/limits.h"
#include "pgw/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rmfs_open_table {
    pgw_rmfs_fd *head;
    size_t count;
};

rmfs_open_table *rmfs_open_table_create(void)
{
    return (rmfs_open_table *)calloc(1, sizeof(rmfs_open_table));
}

void rmfs_open_table_destroy(rmfs_open_table *t, const pgw_fs_backend_ops *ops,
                             void *impl)
{
    if (t == NULL)
        return;
    pgw_rmfs_fd *cur = t->head;
    while (cur != NULL) {
        pgw_rmfs_fd *next = cur->next;
        if (ops != NULL && ops->close != NULL && cur->handle != PGW_FH_NONE)
            ops->close(impl, cur->handle, 0);
        free(cur);
        cur = next;
    }
    free(t);
}

pgw_rmfs_fd *rmfs_open_table_insert(rmfs_open_table *t, uint64_t handle,
                                    const char *path, uint32_t oflags,
                                    uint32_t mode, const pgw_fs_cred *cred)
{
    if (t == NULL || path == NULL)
        return NULL;
    pgw_rmfs_fd *fd = (pgw_rmfs_fd *)calloc(1, sizeof(*fd));
    if (fd == NULL)
        return NULL;
    fd->handle = handle;
    snprintf(fd->path, sizeof(fd->path), "%s", path);
    fd->oflags = oflags;
    fd->mode = mode;
    if (cred != NULL) {
        fd->owner_uid = cred->uid;
        fd->owner_gid = cred->gid;
        fd->prio = cred->prio;
    }
    /* owner_pid stays 0 until the resource-manager glue tags the descriptor
     * with the client's process id (pgw_rmfs_fd_set_owner). */
    fd->refcount = 1;
    fd->unlinked = false;
    fd->next = t->head;
    t->head = fd;
    t->count++;
    return fd;
}

void rmfs_open_table_ref(pgw_rmfs_fd *fd)
{
    if (fd != NULL)
        fd->refcount++;
}

bool rmfs_open_table_remove(rmfs_open_table *t, pgw_rmfs_fd *fd)
{
    if (t == NULL || fd == NULL)
        return false;
    if (fd->refcount > 1) {
        fd->refcount--;
        return false; /* still referenced */
    }
    pgw_rmfs_fd **prev = &t->head;
    while (*prev != NULL) {
        if (*prev == fd) {
            *prev = fd->next;
            t->count--;
            free(fd);
            return true; /* closed and freed */
        }
        prev = &(*prev)->next;
    }
    return false;
}

size_t rmfs_open_table_count(const rmfs_open_table *t)
{
    return (t != NULL) ? t->count : 0;
}

void rmfs_open_table_mark_unlinked(rmfs_open_table *t, const char *path)
{
    if (t == NULL || path == NULL)
        return;
    for (pgw_rmfs_fd *cur = t->head; cur != NULL; cur = cur->next) {
        if (strcmp(cur->path, path) == 0)
            cur->unlinked = true;
    }
}

void rmfs_open_table_set_owner(pgw_rmfs_fd *fd, uint32_t owner_pid)
{
    if (fd != NULL)
        fd->owner_pid = owner_pid;
}

void rmfs_open_table_rename(rmfs_open_table *t, const char *oldpath,
                            const char *newpath)
{
    if (t == NULL || oldpath == NULL || newpath == NULL)
        return;
    for (pgw_rmfs_fd *cur = t->head; cur != NULL; cur = cur->next) {
        if (strcmp(cur->path, oldpath) == 0)
            snprintf(cur->path, sizeof(cur->path), "%s", newpath);
    }
}

void rmfs_open_table_release_pid(rmfs_open_table *t, uint32_t owner_pid,
                                 const pgw_fs_backend_ops *ops, void *impl)
{
    if (t == NULL)
        return;
    pgw_rmfs_fd **prev = &t->head;
    while (*prev != NULL) {
        pgw_rmfs_fd *cur = *prev;
        if (cur->owner_pid == owner_pid) {
            *prev = cur->next;
            t->count--;
            if (ops != NULL && ops->close != NULL && cur->handle != PGW_FH_NONE)
                ops->close(impl, cur->handle, 0);
            free(cur);
        } else {
            prev = &cur->next;
        }
    }
}

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/src/rmfs_calls.c - One invoker per backend vtable entry.
 *
 * Implements requirements QNX-04, QNX-05 and QNX-08:
 *   - Every backend call of the module goes through exactly one invoker, so
 *     the submission path (priority queue) is written once and every
 *     operation is counted in the same place.
 *   - No invoker allocates: the argument block lives on the caller's stack.
 */
#include "rmfs_internal.h"

#include <stdio.h>
#include <string.h>

pgw_err call_lookup(void *v)
{
    rmfs_args *a = (rmfs_args *)v;
    return a->fs->ops->lookup(a->fs->impl, a->p1, a->attr, a->prio);
}

pgw_err call_getattr(void *v)
{
    rmfs_args *a = (rmfs_args *)v;
    return a->fs->ops->getattr(a->fs->impl, a->p1, a->attr, a->prio);
}

pgw_err call_setattr(void *v)
{
    rmfs_args *a = (rmfs_args *)v;
    return a->fs->ops->setattr(a->fs->impl, a->p1, &a->sa, a->prio);
}

pgw_err call_open(void *v)
{
    rmfs_args *a = (rmfs_args *)v;
    return a->fs->ops->open(a->fs->impl, a->p1, a->flags, a->mode, &a->handle,
                            a->prio);
}

pgw_err call_close(void *v)
{
    rmfs_args *a = (rmfs_args *)v;
    return a->fs->ops->close(a->fs->impl, a->handle, a->prio);
}

pgw_err call_read(void *v)
{
    rmfs_args *a = (rmfs_args *)v;
    return a->fs->ops->read(a->fs->impl, a->handle, a->off, a->buf, a->len,
                            a->out_n, a->prio);
}

pgw_err call_write(void *v)
{
    rmfs_args *a = (rmfs_args *)v;
    return a->fs->ops->write(a->fs->impl, a->handle, a->off, a->cbuf, a->len,
                             a->out_n, a->prio);
}

pgw_err call_readdir(void *v)
{
    rmfs_args *a = (rmfs_args *)v;
    return a->fs->ops->readdir(a->fs->impl, a->handle, a->off, a->dirents,
                               a->max_entries, a->out_count, a->next_off,
                               a->prio);
}

pgw_err call_mkdir(void *v)
{
    rmfs_args *a = (rmfs_args *)v;
    return a->fs->ops->mkdir(a->fs->impl, a->p1, a->mode, a->prio);
}

pgw_err call_unlink(void *v)
{
    rmfs_args *a = (rmfs_args *)v;
    return a->fs->ops->unlink(a->fs->impl, a->p1, a->flags != 0, a->prio);
}

pgw_err call_rename(void *v)
{
    rmfs_args *a = (rmfs_args *)v;
    return a->fs->ops->rename(a->fs->impl, a->p1, a->p2, a->prio);
}

pgw_err call_symlink(void *v)
{
    rmfs_args *a = (rmfs_args *)v;
    return a->fs->ops->symlink(a->fs->impl, a->p1, a->p2, a->prio);
}

pgw_err call_readlink(void *v)
{
    rmfs_args *a = (rmfs_args *)v;
    return a->fs->ops->readlink(a->fs->impl, a->p1, a->target, a->cap, a->prio);
}

pgw_err call_link(void *v)
{
    rmfs_args *a = (rmfs_args *)v;
    return a->fs->ops->link(a->fs->impl, a->p1, a->p2, a->prio);
}

pgw_err call_fsync(void *v)
{
    rmfs_args *a = (rmfs_args *)v;
    return a->fs->ops->fsync(a->fs->impl, a->handle, a->prio);
}

pgw_err call_statfs(void *v)
{
    rmfs_args *a = (rmfs_args *)v;
    return a->fs->ops->statfs(a->fs->impl, a->stvfs, a->prio);
}

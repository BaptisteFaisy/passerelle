/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/srv_qnx.c - dispatch channel, node/OCB lifetime, attach
 * and thread pool.
 *
 * One filesystem resource manager serves one mount point; mounting two shares
 * means running two instances (VIO-06 is met at the process level, which is
 * also what makes QNX-06 meaningful: each instance holds only its own device's
 * capabilities).
 *
 * A node can have more than one OCB: mapping a file (executing a binary from
 * the share) makes iofunc attach a second one for the memory manager. Each OCB
 * holds a reference on its node, and the node is freed by whoever drops the
 * last reference - not by the first close, which would clunk a fid another
 * mapping is still using.
 *
 * Covered requirements: QNX-01 (the manager serves every path below its mount
 * point), QNX-02 (nothing but the kernel, libc and the PCI server is needed),
 * QNX-05 (a thread pool wider than the request pipeline), QNX-07 (clean detach),
 * FS-11, SEC-01 (nosuid/ro/noexec published in the mount flags).
 */
/* The project compiles with -std=c11 (no GNU extensions). QNX ignores these
 * feature macros: its own headers always expose S_IF*, strnlen and the POSIX
 * realtime interfaces. The host needs them declared to build this file against
 * the stubs in tests/qnx-stub, so both are spelled out here once. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
/* QNX 8's thread pool passes its work item to block/handler/context_alloc under
 * THREAD_POOL_PARAM_T. The dispatch layer's own functions (dispatch_block() and
 * friends) take dispatch_context_t, so the pool parameter is pinned to that
 * type here - the same define the QNX 8 Resource Manager examples use - instead
 * of the resmgr_context_t default, which would make every dispatch_* assignment
 * a type mismatch (QNX-05). */
#define THREAD_POOL_PARAM_T dispatch_context_t
#include "pgw/drvsrv.h"

#include "pgw/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mount.h>
#include <sys/stat.h>

/* One server per process (see the file comment). The iofunc callbacks have no
 * user pointer, so the current server is reached through this. */
static pgw_srv *g_srv;

/* Node whose last OCB just went: iofunc may still touch the attribute until
 * close returns, so the free happens in the close handler, on this thread. */
static _Thread_local pgw_node *t_released;

#define SRV_MSG_MAX 4096u

static IOFUNC_OCB_T *srv_ocb_calloc(resmgr_context_t *ctp, IOFUNC_ATTR_T *attr)
{
    (void)ctp;
    pgw_ocb *ocb = calloc(1, sizeof *ocb);
    if (ocb == NULL)
        return NULL; /* the client gets ENOMEM, the server keeps running */
    if (attr != &g_srv->root_attr) {
        ocb->node = (pgw_node *)(void *)attr;
        ocb->node->ocbs++;
    }
    return &ocb->ocb;
}

static void srv_ocb_free(IOFUNC_OCB_T *raw)
{
    pgw_ocb *ocb = (pgw_ocb *)(void *)raw;
    if (ocb == NULL)
        return;
    pgw_node *node = ocb->node;
    free(ocb);
    if (node != NULL && node->ocbs > 0 && --node->ocbs == 0)
        t_released = node;
}

static void node_free(pgw_node *node)
{
    if (node == NULL)
        return;
    if (node->fd != NULL && g_srv != NULL && g_srv->fs != NULL) {
        /* Release the backend handle: 9P clunks its fid (P9-07), virtiofs
         * releases its file handle and returns its references (VFS-04). */
        (void)pgw_rmfs_close(g_srv->fs, node->fd);
        node->fd = NULL;
    }
    free(node);
}

pgw_srv *pgw_srv_current(void)
{
    return g_srv;
}

pgw_err pgw_node_open(resmgr_context_t *ctp, io_open_t *msg, const char *path,
                      pgw_rmfs_fd *fd, const pgw_fs_attr *a, bool writable)
{
    if (ctp == NULL || msg == NULL || path == NULL || a == NULL)
        return PGW_EINVAL;
    pgw_node *node = calloc(1, sizeof *node);
    if (node == NULL)
        return PGW_ENOMEM;
    size_t n = strnlen(path, PGW_PATH_MAX - 1u);
    memcpy(node->path, path, n);
    node->path[n] = '\0';
    node->fd = fd;
    node->writable = writable;
    node->ocbs = 0;
    iofunc_attr_init(&node->attr, S_IFREG, NULL, NULL);
    if (g_srv != NULL)
        node->attr.mount = &g_srv->mount;
    pgw_attr_from_fs(&node->attr, a, g_srv != NULL ? &g_srv->mount : NULL);

    /* iofunc allocates the OCB through our hook, which takes the first
     * reference on the node. */
    int rc = iofunc_ocb_attach(ctp, msg, NULL, &node->attr, NULL);
    if (rc != EOK) {
        /* iofunc may have allocated and released an OCB for us: its free hook
         * then marked the node as released. Only clear that mark when it is
         * *this* node, so a node released by an earlier close on this thread
         * is not lost. */
        if (t_released == node)
            t_released = NULL;
        node_free(node);
        return pgw_code_from_host(rc);
    }
    return PGW_OK;
}

/* Called by the close handler: free the node whose last OCB went, if any. */
pgw_node *pgw_node_released(void)
{
    pgw_node *node = t_released;
    t_released = NULL;
    return node;
}

void pgw_node_dispose(pgw_node *node)
{
    node_free(node);
}

pgw_err pgw_srv_init(pgw_srv *srv, pgw_rmfs *fs, const pgw_mount_opts *opts,
                     const char *name)
{
    if (srv == NULL || fs == NULL || opts == NULL || name == NULL)
        return PGW_EINVAL;
    memset(srv, 0, sizeof *srv);
    srv->fs = fs;
    srv->opts = opts;
    srv->name = name;
    srv->mntid = -1;

    srv->dpp = dispatch_create();
    if (srv->dpp == NULL)
        return pgw_code_from_host(errno);

    srv->rattr.nparts_max = 1;
    srv->rattr.msg_max_size = SRV_MSG_MAX;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &srv->connect, _RESMGR_IO_NFUNCS,
                     &srv->io);
    pgw_resmgr_connect_init(&srv->connect);
    pgw_resmgr_io_init(&srv->io);

    /* SYMLINK: the client resolves symlinks, we redirect it (spec 5.3).
     * NO_TRUNC: an over-long name is an error, never silently shortened.
     * SYNC_IO: without it iofunc_sync_verify() refuses fsync() with EINVAL.
     * CHOWN_RESTRICTED: only root changes an owner, as POSIX requires. */
    srv->mount.conf = IOFUNC_PC_SYMLINK | IOFUNC_PC_NO_TRUNC |
                      IOFUNC_PC_CHOWN_RESTRICTED | IOFUNC_PC_SYNC_IO;
    /* SEC-01: nosuid by default; ro and noexec come from the option list. The
     * process manager reads these flags before executing a file from the share,
     * so a setuid bit coming from the PC is ignored. */
    srv->mount.flags = _MOUNT_NOSUID;
    if (opts->fs.readonly)
        srv->mount.flags |= _MOUNT_READONLY;
    if (opts->fs.noexec)
        srv->mount.flags |= _MOUNT_NOEXEC;
    srv->mount.blocksize = 4096;

    srv->mount_funcs.nfuncs = 2; /* only the OCB allocation is ours */
    srv->mount_funcs.ocb_calloc = srv_ocb_calloc;
    srv->mount_funcs.ocb_free = srv_ocb_free;
    srv->mount.funcs = &srv->mount_funcs;

    iofunc_attr_init(&srv->root_attr, S_IFDIR | 0555, NULL, NULL);
    srv->root_attr.mount = &srv->mount;
    g_srv = srv;
    return PGW_OK;
}

pgw_err pgw_srv_attach(pgw_srv *srv, const char *mount_point)
{
    if (srv == NULL || srv->dpp == NULL || mount_point == NULL)
        return PGW_EINVAL;
    /* _RESMGR_FLAG_DIR: every path at or below the mount point is ours, which
     * is exactly what QNX-01 asks of a filesystem. */
    int id = resmgr_attach(srv->dpp, &srv->rattr, mount_point, _FTYPE_ANY,
                           _RESMGR_FLAG_DIR, &srv->connect, &srv->io,
                           &srv->root_attr);
    if (id == -1)
        return pgw_code_from_host(errno);
    srv->mntid = id;
    PGW_INFO("%s: attached %s (nosuid%s%s)", srv->name, mount_point,
             (srv->mount.flags & _MOUNT_READONLY) != 0u ? ",ro" : "",
             (srv->mount.flags & _MOUNT_NOEXEC) != 0u ? ",noexec" : "");
    return PGW_OK;
}

pgw_err pgw_srv_serve(pgw_srv *srv, uint32_t threads)
{
    if (srv == NULL || srv->dpp == NULL)
        return PGW_EINVAL;
    if (threads < 2u)
        threads = 2u;
    if (threads > PGW_INFLIGHT_MAX)
        threads = PGW_INFLIGHT_MAX;

    thread_pool_attr_t pattr;
    memset(&pattr, 0, sizeof pattr);
    pattr.handle = srv->dpp;
    /* The dispatch layer's own functions take dispatch_context_t, which is why
     * THREAD_POOL_PARAM_T is pinned to that type at the top of the file. */
    pattr.context_alloc = dispatch_context_alloc;
    pattr.block_func = dispatch_block;
    pattr.unblock_func = dispatch_unblock;
    pattr.handler_func = dispatch_handler;
    pattr.context_free = dispatch_context_free;
    /* One thread per request in flight plus one that always stays blocked on
     * RECEIVE: that is what keeps a slow client from stalling the others
     * (QNX-05). */
    pattr.lo_water = 2;
    pattr.increment = 1;
    pattr.hi_water = (unsigned short)threads;
    pattr.maximum = (unsigned short)threads;

    srv->pool = thread_pool_create(&pattr, POOL_FLAG_EXIT_SELF);
    if (srv->pool == NULL)
        return pgw_code_from_host(errno);
    PGW_INFO("%s: serving with up to %u threads", srv->name, threads);
    thread_pool_start(srv->pool); /* returns only if the pool cannot start */
    return PGW_EIO;
}

pgw_err pgw_srv_detach(pgw_srv *srv)
{
    if (srv == NULL)
        return PGW_EINVAL;
    if (srv->dpp != NULL && srv->mntid >= 0) {
        if (resmgr_detach(srv->dpp, srv->mntid, _RESMGR_DETACH_PATHNAME) == -1) {
            PGW_WARN("%s: detach failed: %s", srv->name, strerror(errno));
        }
        srv->mntid = -1;
    }
    if (srv->dpp != NULL) {
        (void)dispatch_destroy(srv->dpp);
        srv->dpp = NULL;
    }
    g_srv = NULL;
    return PGW_OK;
}

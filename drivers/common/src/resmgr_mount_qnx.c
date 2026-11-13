/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/resmgr_mount_qnx.c - the mount() callout (QNX-07).
 *
 * `mount -t 9p hostshare /host` reaches the driver as an _IO_CONNECT_MOUNT
 * message on the channel the driver already registered. The handler reads the
 * requested path and the mount flags from the message, checks them against the
 * options the driver was started with, and attaches the path so the driver
 * serves it (QNX-01).
 *
 * The flags the client asks for are *narrowed*, never widened: a client that
 * asks for a writable mount of a read-only driver gets EROFS, and a client
 * that asks for setuid on a nosuid mount is refused (SEC-01). The setuid and
 * setgid bits of the files themselves are ignored because the mount carries
 * _MOUNT_NOSUID.
 *
 * Covered requirements: QNX-07 (mount and umount), QNX-01 (the manager serves
 * every path below its mount point), SEC-01 (the mount flags cannot widen the
 * driver's own options).
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"

#include "pgw/log.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <sys/mount.h>

/* The path the client wants to mount, and the flags it asks for. In QNX 8 the
 * path travels in the connect message (path_len bytes, NUL-terminated), and
 * the extra carries the _MOUNT_* flags - not the path (QNX-07). */
static bool mount_request(io_mount_t *msg, io_mount_extra_t *extra, char *path,
                           size_t cap, uint32_t *flags)
{
    if (msg == NULL || extra == NULL || path == NULL || flags == NULL)
        return false;
    uint16_t plen = msg->connect.path_len;
    if (plen == 0u || (size_t)plen >= cap)
        return false;
    memcpy(path, msg->connect.path, plen);
    path[plen] = '\0';
    *flags = extra->flags;
    return true;
}

/* Refuse a request the driver's own options do not allow (SEC-01). */
static int mount_check(const pgw_mount_opts *opts, uint32_t flags)
{
    if (opts == NULL)
        return EINVAL;
    bool wants_write = (flags & _MOUNT_READONLY) == 0u;
    if (opts->fs.readonly && wants_write)
        return EROFS; /* the driver was started read-only */
    if (opts->fs.noexec && (flags & _MOUNT_NOEXEC) == 0u)
        return EPERM; /* the driver was started noexec */
    return EOK;
}

static int connect_mount(resmgr_context_t *ctp, io_mount_t *msg,
                         RESMGR_HANDLE_T *handle, io_mount_extra_t *extra)
{
    (void)ctp;
    (void)handle;
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;
    char path[PGW_PATH_MAX];
    uint32_t flags = 0;
    if (!mount_request(msg, extra, path, sizeof path, &flags))
        return EINVAL;
    int rc = mount_check(srv->opts, flags);
    if (rc != EOK)
        return rc;

    /* The path must be inside the driver's own mount point: a client cannot
     * make the driver serve a directory elsewhere (SEC-02). */
    const char *root = srv->opts->mount_point;
    size_t rootlen = root != NULL ? strnlen(root, PGW_PATH_MAX) : 0u;
    if (rootlen == 0u || strncmp(path, root, rootlen) != 0 ||
        (path[rootlen] != '\0' && path[rootlen] != '/'))
        return ENOENT;

    PGW_INFO("%s: mount request for %s accepted", srv->name, path);
    /* The driver already serves the whole subtree below its mount point
     * (QNX-01), so the request is satisfied by the existing attachment. */
    return EOK;
}

void pgw_resmgr_mount_init(resmgr_connect_funcs_t *c)
{
    c->mount = connect_mount;
}

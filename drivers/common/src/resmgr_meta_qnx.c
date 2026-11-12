/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/resmgr_meta_qnx.c - attribute changes: chmod, chown,
 * utime and truncate/space.
 *
 * The iofunc layer validates the POSIX rules (who may change what) against the
 * caller's identity and updates the cached attribute; this file then sends the
 * change to the PC and re-reads the attribute, so the driver never reports a
 * state the host does not have (FS-12). A read-only mount refuses every one of
 * them (SEC-01).
 *
 * Covered requirements: FS-07, FS-09, FS-12, SEC-01, QNX-03.
 */
/* The project compiles with -std=c11 (no GNU extensions). QNX ignores these
 * feature macros: its own headers always expose S_IF*, strnlen and the POSIX
 * realtime interfaces. The host needs them declared to build this file against
 * the stubs in tests/qnx-stub, so both are spelled out here once. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "pgw/drvsrv.h"

#include "pgw/log.h"
#include "pgw/time.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>

#include <sys/stat.h>

/* <fcntl.h> of the SDP defines these three; the guard is only here so the PC
 * stub build (which uses the host <fcntl.h>) compiles. The values are the SVR4
 * ones QNX documents, and drivers/tests/qnx_abi_assert.c pins them against the
 * real header on target (see docs/qnx-abi.md). */
#ifndef F_ALLOCSP
#define F_ALLOCSP 10
#endif
#ifndef F_FREESP
#define F_FREESP 11
#endif
#ifndef F_GROWSP
#define F_GROWSP 12
#endif

static bool readonly_mount(void)
{
    pgw_srv *srv = pgw_srv_current();
    return srv == NULL || srv->opts->fs.readonly;
}

/* Send the change, then re-read the attributes from the PC. */
static int commit(pgw_ocb *ocb, pgw_err e)
{
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL || ocb->node == NULL)
        return EIO;
    if (e == PGW_OK) {
        pgw_fs_attr a;
        if (pgw_rmfs_fstat(srv->fs, ocb->node->fd, &a) == PGW_OK)
            pgw_attr_from_fs(&ocb->node->attr, &a, &srv->mount);
    }
    return pgw_qnx_status(e);
}

static int io_chmod(resmgr_context_t *ctp, io_chmod_t *msg, RESMGR_OCB_T *raw)
{
    pgw_ocb *ocb = (pgw_ocb *)(void *)raw;
    if (readonly_mount())
        return EROFS;
    int rc = iofunc_chmod(ctp, msg, raw, ocb->ocb.attr);
    if (rc != EOK)
        return rc;
    pgw_fs_cred cred;
    if (pgw_cred_from_ctp(ctp, &cred) != PGW_OK)
        return EACCES;
    uint32_t mode = ocb->ocb.attr->mode & PGW_S_PERM_MASK;
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;
    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    pgw_trace_req(PGW_OP_CHMOD, 0);
    pgw_err e = pgw_rmfs_chmod(srv->fs, &cred, ocb->node->path, mode);
    pgw_trace_rep(PGW_OP_CHMOD, 0, pgw_qnx_status(e));
    pgw_prio_leave(&prio);
    return commit(ocb, e);
}

static int io_chown(resmgr_context_t *ctp, io_chown_t *msg, RESMGR_OCB_T *raw)
{
    pgw_ocb *ocb = (pgw_ocb *)(void *)raw;
    if (readonly_mount())
        return EROFS;
    int rc = iofunc_chown(ctp, msg, raw, ocb->ocb.attr);
    if (rc != EOK)
        return rc;
    pgw_fs_cred cred;
    if (pgw_cred_from_ctp(ctp, &cred) != PGW_OK)
        return EACCES;
    uint32_t uid = ocb->ocb.attr->uid;
    uint32_t gid = ocb->ocb.attr->gid;
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;
    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    pgw_trace_req(PGW_OP_CHOWN, 0);
    pgw_err e = pgw_rmfs_chown(srv->fs, &cred, ocb->node->path, uid, gid);
    pgw_trace_rep(PGW_OP_CHOWN, 0, pgw_qnx_status(e));
    pgw_prio_leave(&prio);
    return commit(ocb, e);
}

static int io_utime(resmgr_context_t *ctp, io_utime_t *msg, RESMGR_OCB_T *raw)
{
    pgw_ocb *ocb = (pgw_ocb *)(void *)raw;
    if (readonly_mount())
        return EROFS;
    int rc = iofunc_utime(ctp, msg, raw, ocb->ocb.attr);
    if (rc != EOK)
        return rc;
    /* For "now", iofunc may only mark the times stale: make them real. */
    (void)iofunc_time_update(ocb->ocb.attr);
    pgw_fs_cred cred;
    if (pgw_cred_from_ctp(ctp, &cred) != PGW_OK)
        return EACCES;
    uint64_t atime_ns = pgw_ns_join((uint64_t)ocb->ocb.attr->atime,
                                    ocb->ocb.attr->atime_ns);
    uint64_t mtime_ns = pgw_ns_join((uint64_t)ocb->ocb.attr->mtime,
                                    ocb->ocb.attr->mtime_ns);
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;
    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    pgw_trace_req(PGW_OP_UTIME, 0);
    pgw_err e = pgw_rmfs_utimens(srv->fs, &cred, ocb->node->path, atime_ns, mtime_ns);
    pgw_trace_rep(PGW_OP_UTIME, 0, pgw_qnx_status(e));
    pgw_prio_leave(&prio);
    return commit(ocb, e);
}

/* Where a space request starts, and how big the file should become. Returns
 * EINVAL for an unknown whence or subtype, ENOTSUP when the range would punch a
 * hole: 9P and virtiofs can only set a size (FS-07). */
static int space_target(const io_space_t *msg, const pgw_ocb *ocb, uint64_t size,
                        uint64_t *newsize)
{
    uint64_t base;
    switch (msg->i.whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = (uint64_t)ocb->ocb.offset;
        break;
    case SEEK_END:
        base = size;
        break;
    default:
        return EINVAL;
    }
    uint64_t start = base + msg->i.start;
    switch (msg->i.subtype) {
    case F_FREESP:
        if (msg->i.len != 0 && start + msg->i.len < size)
            return ENOTSUP; /* cannot punch a hole through these protocols */
        *newsize = start;
        return EOK;
    case F_GROWSP:
    case F_ALLOCSP:
        *newsize = start + msg->i.len > size ? start + msg->i.len : size;
        return EOK;
    default:
        return EINVAL;
    }
}

static int io_space(resmgr_context_t *ctp, io_space_t *msg, RESMGR_OCB_T *raw)
{
    pgw_ocb *ocb = (pgw_ocb *)(void *)raw;
    if (readonly_mount())
        return EROFS;
    int rc = iofunc_space_verify(ctp, msg, raw, NULL);
    if (rc != EOK)
        return rc;
    if (ocb->node == NULL || ocb->node->fd == NULL)
        return EBADF;

    uint64_t size = (uint64_t)ocb->ocb.attr->nbytes;
    uint64_t newsize = 0;
    rc = space_target(msg, ocb, size, &newsize);
    if (rc != EOK)
        return rc;

    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;
    pgw_fs_cred cred;
    if (pgw_cred_from_ctp(ctp, &cred) != PGW_OK)
        return EACCES;
    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    pgw_trace_req(PGW_OP_TRUNC, 0);
    pgw_err e = PGW_OK;
    if (newsize != size)
        e = pgw_rmfs_truncate(srv->fs, &cred, ocb->node->path, newsize);
    pgw_trace_rep(PGW_OP_TRUNC, 0, pgw_qnx_status(e));
    pgw_prio_leave(&prio);
    if (e != PGW_OK)
        return pgw_qnx_status(e);
    ocb->ocb.attr->nbytes = (int64_t)newsize;
    msg->o = newsize;
    return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o);
}

void pgw_resmgr_meta_init(resmgr_io_funcs_t *io)
{
    io->chmod = io_chmod;
    io->chown = io_chown;
    io->utime = io_utime;
    io->utime64 = io_utime;
    io->space = io_space;
}

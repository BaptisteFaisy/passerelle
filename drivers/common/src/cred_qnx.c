/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/cred_qnx.c - caller identity and unblock detection.
 *
 * Every operation is authorised in the guest, against the identity the kernel
 * reports for the sender (FS-09): the PC side only ever checks the host user, so
 * a permission decision made there would be wrong for a guest process running
 * as someone else.
 *
 * QNX 8 stopped carrying the client's credentials in the message header
 * (struct _msg_info64 has no `cred` member any more): they are asked for on the
 * client's own server connection with ConnectClientInfo(). The driver only ever
 * reads euid/egid of the returned struct _cred_info.
 *
 * Covered requirements: FS-09, SEC-01 (ro/noexec/nosuid are enforced by the
 * mount flags, the rest by these credentials), QNX-05 (a client that asks to be
 * unblocked must be noticed).
 */
/* The project compiles with -std=c11 (no GNU extensions). The feature macros
 * below make the POSIX realtime interfaces visible on the host build (which
 * compiles this file against tests/qnx-stub) and fix the rest of the POSIX
 * surface on QNX. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "pgw/drvsrv.h"

#include "pgw/log.h"
#include "pgw/prio.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

pgw_err pgw_cred_from_ctp(resmgr_context_t *ctp, pgw_fs_cred *cred)
{
    if (ctp == NULL || cred == NULL)
        return PGW_EINVAL;

    /* The fixed-size grouplist of struct _client_info holds __NGROUPS_MAX
     * entries, which is exactly the buffer ConnectClientInfo() needs for a
     * supplementary-group list the drivers do not read (FS-09). */
    struct _client_info info;
    memset(&info, 0, sizeof info);
    if (ConnectClientInfo(ctp->info.scoid, &info, __NGROUPS_MAX) == -1) {
        /* No credentials, no operation: fail closed rather than guess. */
        PGW_ERR("cred: ConnectClientInfo() failed: %s", strerror(errno));
        return pgw_code_from_host(errno);
    }
    cred->uid = info.cred.euid;
    cred->gid = info.cred.egid;
    /* The priority travels with the request: the submission scheduler orders
     * pending requests by it (QNX-03, QNX-04, PERF-05). QNX numbers priorities
     * the other way round (1 is the most urgent), so the conversion happens
     * here, once: downstream, `prio` always means "bigger is more urgent". */
    cred->prio = pgw_prio_from_qnx((uint32_t)ctp->info.priority);
    return PGW_OK;
}

int pgw_qnx_status(pgw_err e)
{
    if (e == PGW_OK)
        return EOK;
    int v = pgw_host_errno(e);
    return v != 0 ? v : EIO;
}

bool pgw_client_unblocking(resmgr_context_t *ctp)
{
    if (ctp == NULL)
        return true;
    struct _msg_info mi;
    if (MsgInfo(ctp->rcvid, &mi) == -1)
        return true; /* the client is gone: stop working for it (ROB-02) */
    return ((uint32_t)mi.flags & _NTO_MI_UNBLOCK_REQ) != 0u;
}
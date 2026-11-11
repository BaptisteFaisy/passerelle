/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/cred_qnx.c - caller identity and unblock detection.
 *
 * Every operation is authorised in the guest, against the identity the kernel
 * reports for the sender (FS-09): the PC side only ever checks the host user, so
 * a permission decision made there would be wrong for a guest process running
 * as someone else.
 *
 * Covered requirements: FS-09, SEC-01 (ro/noexec/nosuid are enforced by the
 * mount flags, the rest by these credentials), QNX-05 (a client that asks to be
 * unblocked must be noticed).
 */
/* The project compiles with -std=c11 (no GNU extensions). QNX ignores these
 * feature macros: its own headers always expose S_IF*, strnlen and the POSIX
 * realtime interfaces. The host needs them declared to build this file against
 * the stubs in tests/qnx-stub, so both are spelled out here once. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "pgw/drvsrv.h"

#include <errno.h>
#include <stddef.h>

void pgw_cred_from_ctp(resmgr_context_t *ctp, pgw_fs_cred *cred)
{
    if (ctp == NULL || cred == NULL)
        return;
    cred->uid = ctp->info.cred.euid;
    cred->gid = ctp->info.cred.egid;
    /* The priority travels with the request: the submission scheduler orders
     * pending requests by it (QNX-03, QNX-04, PERF-05). */
    cred->prio = ctp->info.priority;
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
    return (mi.flags & _NTO_MI_UNBLOCK_REQ) != 0;
}

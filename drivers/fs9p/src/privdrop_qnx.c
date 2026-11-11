/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fs9p/src/privdrop_qnx.c - the 9P driver's abilities and root drop.
 *
 * The driver needs exactly four things beyond an ordinary process, and it asks
 * for each one with a documented reason:
 *
 *   mem_phys        map the device's own BAR (VIO-02) - and nothing else, the
 *                   range is bounded by the security policy (SEC-02);
 *   interrupt       attach the interrupt service thread (VIO-04);
 *   pathspace       register the mount point (QNX-01);
 *   public_channel  create the channel dispatch_create() needs (QNX-01).
 *
 * The set is locked before root is dropped, so nothing that runs afterwards can
 * widen it; image/passerelle.sec grants the same four through the security
 * policy, which is the QNX 8.0 way of expressing the same minimum (QNX-06).
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"
#include "pgw/fs9p.h"
#include "pgw/log.h"

#include <stddef.h>

#include <sys/procmgr.h>

/* The user the driver runs as once it has dropped root. It is a normal account
 * from the image (image/passerelle.build), never root (QNX-06). */
#define FS9P_USER "passerelle"

static const pgw_ability g_abilities[] = {
    { PROCMGR_AID_MEM_PHYS, "map the device BAR only (SEC-02)" },
    { PROCMGR_AID_INTERRUPT, "InterruptAttachThread on its own line (VIO-04)" },
    { PROCMGR_AID_PATHSPACE, "resmgr_attach of the mount point (QNX-01)" },
    { PROCMGR_AID_PUBLIC_CHANNEL, "dispatch channel of the resource manager" },
};

pgw_err fs9p_privdrop(void)
{
    return pgw_privdrop_apply(g_abilities,
                              sizeof g_abilities / sizeof g_abilities[0],
                              FS9P_USER);
}

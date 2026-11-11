/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fsfuse/src/privdrop_qnx.c - the virtiofs driver's abilities and root
 * drop.
 *
 * The same four abilities as the 9P driver, for the same reasons: map the
 * device's own BAR, take its interrupt, register the mount point, create the
 * dispatch channel. The set is locked before root is dropped, and
 * image/passerelle.sec grants the same four through the security policy
 * (QNX-06, SEC-01, SEC-02).
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"
#include "pgw/fsfuse.h"
#include "pgw/fsfuse_qnx.h"
#include "pgw/log.h"

#include <stddef.h>

#include <sys/procmgr.h>

/* The user the driver runs as once it has dropped root. It is a normal account
 * from the image (image/passerelle.build), never root (QNX-06). */
#define FSFUSE_USER "passerelle"

static const pgw_ability g_abilities[] = {
    { PROCMGR_AID_MEM_PHYS, "map the device BAR only (SEC-02)" },
    { PROCMGR_AID_INTERRUPT, "InterruptAttachThread on its own line (VIO-04)" },
    { PROCMGR_AID_PATHSPACE, "resmgr_attach of the mount point (QNX-01)" },
    { PROCMGR_AID_PUBLIC_CHANNEL, "dispatch channel of the resource manager" },
};

pgw_err fsfuse_privdrop(void)
{
    return pgw_privdrop_apply(g_abilities,
                              sizeof g_abilities / sizeof g_abilities[0],
                              FSFUSE_USER);
}

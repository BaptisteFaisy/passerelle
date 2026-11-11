/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/attr_qnx.c - rmfs attributes to QNX iofunc attributes.
 *
 * The wire carries Linux file types and permission bits; QNX has its own
 * S_IF* numbers and its own time layout. The conversion is done here once, on
 * top of the tables of common/ (pgw_host_mode, pgw_ns_split), so neither driver
 * ever writes a second mode table (project rule: zero duplication).
 *
 * Covered requirements: FS-02 (size, type, rights, owner, nanosecond times,
 * link count, stable inode number), FS-09 (the mode is what the permission
 * check reads), SEC-01 (nosuid is enforced by the mount flags, not here).
 */
/* The project compiles with -std=c11 (no GNU extensions). QNX ignores these
 * feature macros: its own headers always expose S_IF*, strnlen and the POSIX
 * realtime interfaces. The host needs them declared to build this file against
 * the stubs in tests/qnx-stub, so both are spelled out here once. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "pgw/drvsrv.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/time.h"

#include <limits.h>
#include <stdint.h>

#include <sys/stat.h>

/* off_t is 64-bit on QNX 8.0 for every target; a size that does not fit is
 * clamped rather than wrapped, so a corrupt answer cannot make a file look
 * negative or tiny. */
static int64_t clamp_off(uint64_t v)
{
    return v > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)v;
}

void pgw_attr_from_fs(iofunc_attr_t *attr, const pgw_fs_attr *a,
                      iofunc_mount_t *mount)
{
    if (attr == NULL || a == NULL)
        return;
    attr->mode = pgw_host_mode(a->type, a->mode);
    attr->uid = a->uid;
    attr->gid = a->gid;
    attr->nlink = a->nlink > (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)a->nlink;
    attr->nbytes = clamp_off(a->size);
    /* Inode 0 means "unused entry" to some tools; the server never sends 0 but
     * the guard costs nothing. */
    attr->inode = a->ino != 0u ? a->ino : 1u;
    attr->rdev = 0; /* the share holds no device node (SEC-02) */

    uint64_t sec = 0;
    uint32_t nsec = 0;
    pgw_ns_split(a->atime_ns, &sec, &nsec);
    attr->atime = (time_t)sec;
    attr->atime_ns = nsec;
    pgw_ns_split(a->mtime_ns, &sec, &nsec);
    attr->mtime = (time_t)sec;
    attr->mtime_ns = nsec;
    pgw_ns_split(a->ctime_ns, &sec, &nsec);
    attr->ctime = (time_t)sec;
    attr->ctime_ns = nsec;

    if (mount != NULL)
        attr->mount = mount;
    /* The times come from the server: stop iofunc from stamping "now" over
     * them and from treating them as dirty. */
    attr->flags &= ~(IOFUNC_ATTR_MTIME | IOFUNC_ATTR_ATIME | IOFUNC_ATTR_CTIME |
                     IOFUNC_ATTR_DIRTY_MASK);
}

/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/resmgr_dir_qnx.c - read of a directory, and the read
 * dispatcher that sends a message to the file path or to this one.
 *
 * A QNX directory read returns a packed list of records, each 8-byte aligned
 * and carrying the cookie to resume at. Entries that do not fit the client's
 * buffer are dropped and the next read resumes from the last one that did fit,
 * so no entry is lost or duplicated (FS-03), and the cookie lives in the OCB
 * offset so rewinddir() restarts the listing.
 *
 * Covered requirements: FS-03, FS-15 (names up to 255 bytes), QNX-03, QNX-05.
 */
/* The project compiles with -std=c11 (no GNU extensions). QNX ignores these
 * feature macros: its own headers always expose S_IF*, strnlen and the POSIX
 * realtime interfaces. The host needs them declared to build this file against
 * the stubs in tests/qnx-stub, so both are spelled out here once. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "pgw/drvsrv.h"

#include "pgw/dirent_qnx.h"
#include "pgw/limits.h"
#include "pgw/log.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <sys/stat.h>

/* Entries asked from the backend per message, and the reply buffer: the reply
 * must fit one QNX message, so it is bounded independently of the client's
 * request. */
#define DIR_ENTRIES 16u
#define DIR_BUF     (8u * 1024u)

static _Thread_local struct {
    uint8_t raw[DIR_BUF];
    pgw_fs_dirent ents[DIR_ENTRIES];
} g_dir;

/* Pack entries into QNX dirent records until the client's buffer is full.
 * Returns the number of bytes written; *next holds the cookie to resume at,
 * which is the offset of the last entry that did fit (FS-03). */
static size_t pack_entries(const pgw_fs_dirent *ents, size_t count, uint8_t *out,
                           size_t cap, uint64_t *next)
{
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        size_t namelen = strnlen(ents[i].name, PGW_NAME_MAX);
        size_t reclen = pgw_dirent_reclen(namelen);
        if (namelen == 0 || used + reclen > cap)
            break; /* the rest goes to the next read */
        struct pgw_dirent_qnx *d = (struct pgw_dirent_qnx *)(void *)(out + used);
        memset(d, 0, reclen);
        d->d_ino = (ino_t)(ents[i].ino != 0u ? ents[i].ino : 1u);
        d->d_offset = (off_t)ents[i].next_off;
        d->d_reclen = (int16_t)reclen;
        d->d_namelen = (int16_t)namelen;
        memcpy(d->d_name, ents[i].name, namelen);
        used += reclen;
        *next = ents[i].next_off;
    }
    return used;
}

int pgw_io_readdir(resmgr_context_t *ctp, io_read_t *msg, pgw_ocb *ocb)
{
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;
    if (ocb->node == NULL || ocb->node->fd == NULL)
        return EBADF;

    uint64_t want = _IO_READ_GET_NBYTES(msg);
    if (want > DIR_BUF)
        want = DIR_BUF;
    if (want < pgw_dirent_reclen(1u))
        return EINVAL; /* the client's buffer cannot hold one entry */

    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    pgw_trace_req(PGW_OP_READDIR, 0);

    size_t count = 0;
    uint64_t next = 0;
    pgw_err e = pgw_rmfs_readdir(srv->fs, ocb->node->fd,
                                 (uint64_t)ocb->ocb.offset,
                                 g_dir.ents, DIR_ENTRIES, &count, &next);
    int status = EOK;
    size_t used = 0;
    if (e != PGW_OK) {
        status = pgw_qnx_status(e);
    } else {
        used = pack_entries(g_dir.ents, count, g_dir.raw, (size_t)want, &next);
        if (used > 0 && resmgr_msgwrite(ctp, g_dir.raw, used, 0) == -1)
            status = errno;
    }
    pgw_trace_rep(PGW_OP_READDIR, 0, status);
    pgw_prio_leave(&prio);
    if (status != EOK)
        return status;
    ocb->ocb.offset = (off_t)next;
    _IO_SET_READ_NBYTES(ctp, (long)used);
    return _RESMGR_NPARTS(0); /* the records already went to the client */
}

int pgw_io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *raw)
{
    int rc = iofunc_read_verify(ctp, msg, raw, NULL);
    if (rc != EOK)
        return rc;
    pgw_ocb *ocb = (pgw_ocb *)(void *)raw;
    uint32_t xtype = msg->i.xtype & _IO_XTYPE_MASK;
    if (ocb->ocb.attr != NULL && S_ISDIR(ocb->ocb.attr->mode)) {
        if (xtype != _IO_XTYPE_NONE && xtype != _IO_XTYPE_READDIR)
            return ENOSYS;
        return pgw_io_readdir(ctp, msg, ocb);
    }
    return pgw_io_read_file(ctp, msg, ocb, xtype);
}

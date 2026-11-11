/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fsfuse/src/trace_qnx.c - the virtiofs driver's view of the kernel
 * trace.
 *
 * drivers/common/src/trace_qnx.c owns the emission; this file owns what the
 * virtiofs driver puts in it: the operation a FUSE opcode belongs to, and the
 * on/off switch (a `-o debug` mount turns the trace on, so a measurement run
 * can turn it off without a rebuild).
 *
 * Covered requirements: QNX-09 (an event at submission and at reception of each
 * request), PERF-01 (the trace is where the round-trip counts are read from).
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"
#include "pgw/fsfuse.h"
#include "pgw/fsfuse_qnx.h"
#include "pgw/log.h"

#include <stdint.h>

void fsfuse_trace_init(const pgw_mount_opts *opts)
{
    pgw_trace_enable(opts != NULL && opts->debug);
    if (pgw_trace_enabled())
        PGW_INFO("fsfuse: kernel trace events enabled (QNX-09)");
}

/* The operation a FUSE opcode belongs to, or PGW_OP__MAX when it has none. */
enum pgw_op fsfuse_op_of(uint32_t opcode)
{
    switch (opcode) {
    case PGW_FUSE_LOOKUP:
        return PGW_OP_OPEN;
    case PGW_FUSE_FORGET:
    case PGW_FUSE_BATCH_FORGET:
        return PGW_OP_CLOSE;
    case PGW_FUSE_GETATTR:
        return PGW_OP_STAT;
    case PGW_FUSE_SETATTR:
        return PGW_OP_CHMOD;
    case PGW_FUSE_READ:
        return PGW_OP_READ;
    case PGW_FUSE_WRITE:
        return PGW_OP_WRITE;
    case PGW_FUSE_READDIR:
        return PGW_OP_READDIR;
    case PGW_FUSE_OPEN:
    case PGW_FUSE_CREATE:
        return PGW_OP_OPEN;
    case PGW_FUSE_RELEASE:
    case PGW_FUSE_RELEASEDIR:
        return PGW_OP_CLOSE;
    case PGW_FUSE_MKDIR:
        return PGW_OP_MKDIR;
    case PGW_FUSE_UNLINK:
    case PGW_FUSE_RMDIR:
        return PGW_OP_UNLINK;
    case PGW_FUSE_RENAME:
    case PGW_FUSE_RENAME2:
        return PGW_OP_RENAME;
    case PGW_FUSE_SYMLINK:
        return PGW_OP_SYMLINK;
    case PGW_FUSE_LINK:
        return PGW_OP_LINK;
    case PGW_FUSE_READLINK:
        return PGW_OP_READLINK;
    case PGW_FUSE_FSYNC:
    case PGW_FUSE_FSYNCDIR:
        return PGW_OP_SYNC;
    case PGW_FUSE_STATFS:
        return PGW_OP_DEVCTL;
    default:
        return PGW_OP__MAX;
    }
}

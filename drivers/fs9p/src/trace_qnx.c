/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fs9p/src/trace_qnx.c - the 9P driver's view of the kernel trace.
 *
 * drivers/common/src/trace_qnx.c owns the emission; this file owns what the 9P
 * driver puts in it: it turns a 9P2000.L message type into the operation the
 * event is labelled with, and switches tracing on for a `-o debug` mount so a
 * measurement run can pair a submission with its reply in `tracelogger`.
 *
 * Covered requirements: QNX-09 (an event at submission and at reception of each
 * request), QNX-03 (the events carry the caller's priority because the emitting
 * thread runs at it), PERF-01 (the trace is what the round-trip counts are read
 * from).
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"
#include "pgw/fs9p.h"
#include "pgw/fs9p_qnx.h"
#include "pgw/log.h"
#include "pgw/p9.h"

#include <stdint.h>

void fs9p_trace_init(const pgw_mount_opts *opts)
{
    /* Off by default: the trace ring is shared and a driver that fills it
     * continuously would hide everything else. `-o debug` turns it on. */
    pgw_trace_enable(opts != NULL && opts->debug);
    if (pgw_trace_enabled())
        PGW_INFO("fs9p: kernel trace events enabled (QNX-09)");
}

/* The operation a 9P message belongs to, or PGW_OP__MAX when it has none. Used
 * to label the trace event and the per-operation counter. */
enum pgw_op fs9p_op_of(uint8_t p9_type)
{
    switch (p9_type) {
    case PGW_P9_TLOPEN:
    case PGW_P9_TLCREATE:
        return PGW_OP_OPEN;
    case PGW_P9_TREAD:
        return PGW_OP_READ;
    case PGW_P9_TWRITE:
        return PGW_OP_WRITE;
    case PGW_P9_TGETATTR:
        return PGW_OP_STAT;
    case PGW_P9_TREADDIR:
        return PGW_OP_READDIR;
    case PGW_P9_TSETATTR:
        return PGW_OP_CHMOD;
    case PGW_P9_TFSYNC:
        return PGW_OP_SYNC;
    case PGW_P9_TSTATFS:
        return PGW_OP_DEVCTL;
    case PGW_P9_TUNLINKAT:
        return PGW_OP_UNLINK;
    case PGW_P9_TRENAMEAT:
        return PGW_OP_RENAME;
    case PGW_P9_TMKDIR:
        return PGW_OP_MKDIR;
    case PGW_P9_TSYMLINK:
        return PGW_OP_SYMLINK;
    case PGW_P9_TLINK:
        return PGW_OP_LINK;
    case PGW_P9_TREADLINK:
        return PGW_OP_READLINK;
    case PGW_P9_TCLUNK:
        return PGW_OP_CLOSE;
    default:
        return PGW_OP__MAX;
    }
}

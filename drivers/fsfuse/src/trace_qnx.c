/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fsfuse/src/trace_qnx.c - the virtiofs driver's view of the kernel
 * trace.
 *
 * drivers/common/src/trace_qnx.c owns the emission and the payload packing;
 * drivers/fsfuse/src/op.c owns the opcode table. Both are checked on the PC
 * bench. What this file owns is the one decision the driver makes: whether
 * tracing is on for this mount. `-o debug` turns it on, so a measurement run
 * produces a trace without a rebuild and a production mount pays nothing for it.
 *
 * Covered requirements: QNX-09, PERF-01.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"
#include "pgw/fsfuse.h"
#include "pgw/fsfuse_qnx.h"
#include "pgw/log.h"

void fsfuse_trace_init(const pgw_mount_opts *opts)
{
    pgw_trace_enable(opts != NULL && opts->debug);
    if (pgw_trace_enabled())
        PGW_INFO("fsfuse: kernel trace events enabled (QNX-09)");
}

/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fs9p/src/trace_qnx.c - the 9P driver's view of the kernel trace.
 *
 * drivers/common/src/trace_qnx.c owns the emission and the payload packing;
 * drivers/fs9p/src/op.c owns the message table. Both are checked on the PC bench.
 * What this file owns is the one decision the driver makes: whether tracing is
 * on for this mount. `-o debug` turns it on, so a measurement run can produce a
 * trace without a rebuild and a production mount pays nothing for it.
 *
 * Covered requirements: QNX-09 (an event at submission and at reception of each
 * request), PERF-01 (the trace is what the round-trip counts are read from).
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"
#include "pgw/fs9p.h"
#include "pgw/fs9p_qnx.h"
#include "pgw/log.h"

void fs9p_trace_init(const pgw_mount_opts *opts)
{
    /* Off by default: the trace ring is shared and a driver that fills it
     * continuously would hide everything else. `-o debug` turns it on. */
    pgw_trace_enable(opts != NULL && opts->debug);
    if (pgw_trace_enabled())
        PGW_INFO("fs9p: kernel trace events enabled (QNX-09)");
}

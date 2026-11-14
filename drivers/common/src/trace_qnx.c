/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/trace_qnx.c - kernel trace events for every request.
 *
 * One user event is inserted when a request is submitted to the transport and
 * one when its reply is received, so `tracelogger` shows, per request, the
 * caller's priority, the operation and the round trip. The event ids are chosen
 * by each driver inside _NTO_TRACE_USERFIRST.._NTO_TRACE_USERLAST; this file
 * only owns the emission and the on/off switch (`debug` mount option enables
 * it, so a measurement run can turn it off without a rebuild).
 *
 * Covered requirement: QNX-09 (a kernel trace event at submission and at
 * reception of each request).
 */
/* The project compiles with -std=c11 (no GNU extensions). QNX ignores these
 * feature macros: its own headers always expose S_IF*, strnlen and the POSIX
 * realtime interfaces. The host needs them declared to build this file against
 * the stubs in tests/qnx-stub, so both are spelled out here once. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "pgw/drvsrv.h"

#include <sys/neutrino.h>
#include <sys/trace.h>

static bool g_trace;

void pgw_trace_enable(bool on)
{
    g_trace = on;
}

bool pgw_trace_enabled(void)
{
    return g_trace;
}

/* Event ids inside the user range: one pair per operation, so a trace shows
 * "request sent" and "reply received" for the same operation as two events the
 * reader can pair on the tag. */
#define TRACE_EVENT_REQ(op) (int)(_NTO_TRACE_USERFIRST + 2 * (int)(op))
#define TRACE_EVENT_REP(op) (int)(_NTO_TRACE_USERFIRST + 2 * (int)(op) + 1)

void pgw_trace_req(enum pgw_op op, uint32_t tag)
{
    if (!g_trace || (unsigned)op >= (unsigned)PGW_OP__MAX)
        return;
    /* The first payload word carries the operation and the priority the serving
     * thread runs at, so the trace itself shows a request processed at the
     * caller's priority (QNX-03) rather than leaving it to be inferred. */
    pgw_trace_emit(TRACE_EVENT_REQ(op), pgw_trace_pack(op, pgw_trace_prio_now()),
                   (int)tag);
}

void pgw_trace_rep(enum pgw_op op, uint32_t tag, int32_t status)
{
    if (!g_trace || (unsigned)op >= (unsigned)PGW_OP__MAX)
        return;
    /* The status travels as the second payload word: 0 means the reply was
     * accepted, a QNX errno means it was refused. */
    (void)TraceEvent(_NTO_TRACE_INSERTSUSEREVENT, TRACE_EVENT_REP(op),
                     (int)tag, (int)status);
}

void pgw_trace_emit(int event, int data0, int data1)
{
    if (!g_trace)
        return;
    /* TraceEvent() never blocks and does not allocate: safe on the request
     * path. The return value only reports an unsupported command. */
    (void)TraceEvent(_NTO_TRACE_INSERTSUSEREVENT, event, data0, data1);
}

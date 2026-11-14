/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/traceop.c - the operation numbering and the event payload.
 *
 * Pure arithmetic and one thread call: no protocol, no kernel object, no file
 * descriptor. It is portable so that the PC bench can check the packing rule
 * and both drivers' message tables (drivers/tests/test_traceop.c), which is the
 * only place a wrong label can be caught before a guest runs.
 *
 * Covered requirements: QNX-03, QNX-09.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/traceop.h"

#include <pthread.h>

/* QNX numbers priorities 1 (most urgent) to 255 (least). Anything outside that
 * range is clamped rather than truncated: a truncated priority would silently
 * show the wrong thread in the trace, and a trace reader cannot tell the
 * difference. */
#define PGW_PRIO_MIN 0
#define PGW_PRIO_MAX 255

int pgw_trace_pack(enum pgw_op op, int prio)
{
    int o = (int)op;
    if (o < 0 || o > PGW_TRACE_PACK_OP_MASK)
        o = (int)PGW_OP_OTHER;
    if (prio < PGW_PRIO_MIN)
        prio = PGW_PRIO_MIN;
    if (prio > PGW_PRIO_MAX)
        prio = PGW_PRIO_MAX;
    return ((prio << PGW_TRACE_PACK_PRIO_SHIFT) & 0xFFFF) | (o & PGW_TRACE_PACK_OP_MASK);
}

enum pgw_op pgw_trace_unpack_op(int packed)
{
    int o = packed & PGW_TRACE_PACK_OP_MASK;
    return (o < (int)PGW_OP__MAX) ? (enum pgw_op)o : PGW_OP_OTHER;
}

int pgw_trace_unpack_prio(int packed)
{
    return (packed >> PGW_TRACE_PACK_PRIO_SHIFT) & PGW_TRACE_PACK_OP_MASK;
}

int pgw_trace_prio_now(void)
{
    int policy = 0;
    struct sched_param cur;
    if (pthread_getschedparam(pthread_self(), &policy, &cur) != 0)
        return PGW_PRIO_MAX; /* unknown: the least urgent, never the most */
    return cur.sched_priority;
}

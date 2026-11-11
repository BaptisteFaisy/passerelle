/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/prio_qnx.c - run a request at the caller's priority.
 *
 * QNX boosts the receiving thread to the sender's priority while the reply is
 * pending, so a short request already runs at the right priority. The boost is
 * lost as soon as the server thread blocks on something that is not the client
 * (a transport slot, a condition variable), which is exactly what happens while
 * a 9P or virtiofs request is in flight. This file re-applies the caller's
 * priority for the duration of the exchange so the whole round trip stays at
 * that priority, which is what the kernel trace shows (QNX-03).
 *
 * Covered requirements: QNX-03, QNX-04 (the priority is also what the submission
 * scheduler orders by), PERF-05.
 */
/* The project compiles with -std=c11 (no GNU extensions). QNX ignores these
 * feature macros: its own headers always expose S_IF*, strnlen and the POSIX
 * realtime interfaces. The host needs them declared to build this file against
 * the stubs in tests/qnx-stub, so both are spelled out here once. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "pgw/drvsrv.h"

#include "pgw/log.h"

#include <pthread.h>

void pgw_prio_enter(resmgr_context_t *ctp, pgw_prio_state *st)
{
    if (ctp == NULL || st == NULL)
        return;
    st->changed = false;
    int policy = 0;
    struct sched_param cur;
    if (pthread_getschedparam(pthread_self(), &policy, &cur) != 0)
        return;
    int want = (int)ctp->info.priority;
    if (want <= cur.sched_priority)
        return; /* already at least as high: the kernel boost is enough */

    struct sched_param next = cur;
    next.sched_priority = want;
    st->policy = policy;
    st->param = cur;
    if (pthread_setschedparam(pthread_self(), policy, &next) == 0) {
        st->changed = true;
    } else {
        /* Not fatal: the request still runs, at the inherited priority. */
        PGW_DBG("prio: cannot raise to %d", want);
    }
}

void pgw_prio_leave(pgw_prio_state *st)
{
    if (st == NULL || !st->changed)
        return;
    (void)pthread_setschedparam(pthread_self(), st->policy, &st->param);
    st->changed = false;
}

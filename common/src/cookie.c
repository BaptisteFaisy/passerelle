/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/cookie.c - concurrent request cookies shared by the drivers.
 *
 * The kernel trace of a 16-reader guest run caught the old non-atomic global
 * counter handing the same cookie to two threads (14 November 2026; the trace is
 * read back by tests/qemu/qnx8/trace_report.py). This is the one implementation
 * both transports draw from, and it is portable on purpose: the PC bench runs it
 * with real threads, which is the only way to check a counter that exists because
 * of concurrency.
 *
 * Covered requirements: QNX-05, ROB-05, VIO-02, QNX-09.
 */
#include "pgw/cookie.h"

#include <stddef.h>

uint32_t pgw_cookie_next(pgw_cookie *c)
{
    if (c == NULL)
        return 0u;

    uint32_t cur = atomic_load_explicit(&c->next, memory_order_relaxed);
    for (;;) {
        /* 0 is not a cookie (the handler edges use it for "no protocol tag"), and
         * the count wraps to 1 for the same reason. A struct somebody zeroed
         * draws 1 and continues at 2, so no value is ever handed out twice. */
        uint32_t nxt;
        if (cur == 0u)
            nxt = 2u;
        else if (cur == UINT32_MAX)
            nxt = 1u;
        else
            nxt = cur + 1u;

        if (atomic_compare_exchange_weak_explicit(
                    &c->next, &cur, nxt, memory_order_relaxed, memory_order_relaxed))
            return (cur == 0u) ? 1u : cur;
    }
}

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pgw/prio.h - priority convention and its single conversion point.
 *
 * Two conventions meet in this project and they point in opposite directions:
 *
 *   - QNX numbers thread priorities so that **1 is the most urgent** and 255
 *     the least (a real-time kernel: smaller is better);
 *   - the submission scheduler of layer 3 (pgw_prio_sched) orders pending
 *     requests by **descending urgency value**, because that keeps the
 *     insertion test trivial and the queue a plain array (QNX-04, PERF-05).
 *
 * Passing a QNX priority straight through would therefore let a background copy
 * (priority 250) overtake a control-loop read (priority 10) - the exact
 * behaviour PERF-05 forbids. The conversion happens here, once, and everywhere
 * else in the project `prio` means "bigger is more urgent".
 */
#ifndef PGW_PRIO_H
#define PGW_PRIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* QNX priority range (Neutrino: 1..255, 0 is not a valid thread priority). */
#define PGW_QNX_PRIO_MIN 1u
#define PGW_QNX_PRIO_MAX 255u

/* Convert a QNX thread priority into scheduler urgency. Unknown or out-of-range
 * values map to the least urgent class rather than to the most urgent one: a
 * mis-read priority must never let a request jump the queue. */
static inline uint32_t pgw_prio_from_qnx(uint32_t qnx_prio)
{
    if (qnx_prio < PGW_QNX_PRIO_MIN || qnx_prio > PGW_QNX_PRIO_MAX)
        return 1u; /* least urgent */
    return (PGW_QNX_PRIO_MAX + 1u) - qnx_prio;
}

/* Inverse conversion, for logging and for the kernel-trace events (QNX-09). */
static inline uint32_t pgw_prio_to_qnx(uint32_t urgency)
{
    if (urgency == 0u || urgency > PGW_QNX_PRIO_MAX)
        return PGW_QNX_PRIO_MAX;
    return (PGW_QNX_PRIO_MAX + 1u) - urgency;
}

#ifdef __cplusplus
}
#endif

#endif /* PGW_PRIO_H */

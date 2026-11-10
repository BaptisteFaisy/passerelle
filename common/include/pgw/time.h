/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pgw/time.h - monotonic time and deadlines.
 *
 * One clock source for the whole project, injectable in tests so that a
 * timeout can be exercised without sleeping (ROB-05, FS-13, PERF-06).
 * On QNX this maps to CLOCK_MONOTONIC through ClockTime/ClockCycles; on the PC
 * it maps to clock_gettime(CLOCK_MONOTONIC).
 */
#ifndef PGW_TIME_H
#define PGW_TIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Nanoseconds on the monotonic clock. Never goes backwards. */
uint64_t pgw_now_ns(void);

/* Test hook: replace the clock. `fn` returns nanoseconds; `user` is passed
 * through. NULL restores the real clock. */
typedef uint64_t (*pgw_clock_fn)(void *user);
void pgw_time_set_clock(pgw_clock_fn fn, void *user);

/* Advance the fake clock by `ns` when a fake clock is installed; no-op
 * otherwise. Returns false when no fake clock is installed, so a test that
 * meant to control time fails loudly instead of silently sleeping. */
bool pgw_time_advance(uint64_t ns);

/* --- deadlines ------------------------------------------------------------ */

typedef struct pgw_deadline {
    uint64_t due_ns; /* absolute, monotonic */
    bool forever;    /* true when no timeout was requested */
} pgw_deadline;

/* Build a deadline `timeout_ms` from now. timeout_ms == 0 means "no timeout". */
pgw_deadline pgw_deadline_from_ms(uint64_t timeout_ms);

bool pgw_deadline_expired(pgw_deadline d);           /* false when forever */
uint64_t pgw_deadline_left_ns(pgw_deadline d);       /* 0 when expired */

/* Milliseconds left, rounded up, capped at INT32_MAX; -1 when forever.
 * This is the value handed to a blocking wait. */
int32_t pgw_deadline_left_ms(pgw_deadline d);

/* --- small helpers -------------------------------------------------------- */

uint64_t pgw_ms_to_ns(uint64_t ms);
uint64_t pgw_sec_to_ns(uint64_t s);

/* Split nanoseconds into seconds and remaining nanoseconds, as the wire
 * formats of both 9P (qtime: sec u32 + nsec u32... in 9P2000.L it is u64 sec
 * plus u64 nsec) and FUSE (attr: sec u64 + nsec u32) expect. */
void pgw_ns_split(uint64_t ns, uint64_t *sec, uint32_t *nsec);
uint64_t pgw_ns_join(uint64_t sec, uint32_t nsec);

/* Elapsed-time measurement helper used by host/bench and by the guest
 * benchmarks: returns the difference in nanoseconds, saturating at 0. */
uint64_t pgw_elapsed_ns(uint64_t start_ns);

#ifdef __cplusplus
}
#endif

#endif /* PGW_TIME_H */

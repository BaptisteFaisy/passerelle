/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pgw/log.h - logging and per-operation counters.
 *
 * Two jobs, one header:
 *   - a levelled logger whose backend is swapped per build: syslog/slog2 on
 *     QNX (QNX-08), stderr on the PC test build;
 *   - a counter table indexed by operation, exported by the driver as a
 *     status file so a user can see where the round trips go (QNX-08,
 *     PERF-01).
 *
 * Logging must never allocate on the request path: every call formats into a
 * caller-owned or thread-local buffer and drops the message when the backend is
 * unavailable.
 */
#ifndef PGW_LOG_H
#define PGW_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pgw_level {
    PGW_LOG_ERROR = 0,
    PGW_LOG_WARN = 1,
    PGW_LOG_INFO = 2,
    PGW_LOG_DEBUG = 3,
    PGW_LOG_TRACE = 4
} pgw_level;

/* Backend signature: `msg` is already formatted and NUL-terminated. */
typedef void (*pgw_log_fn)(pgw_level lvl, const char *msg, void *user);

void pgw_log_set_level(pgw_level lvl);
pgw_level pgw_log_level(void);
void pgw_log_set_backend(pgw_log_fn fn, void *user); /* NULL restores default */
bool pgw_log_enabled(pgw_level lvl);

/* The macros below are the only logging interface the rest of the code uses.
 * They evaluate `lvl` first so that an argument with a side effect is not run
 * when the level is filtered out. */
void pgw_log_write(pgw_level lvl, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define PGW_ERR(...)  pgw_log_write(PGW_LOG_ERROR, __VA_ARGS__)
#define PGW_WARN(...) pgw_log_write(PGW_LOG_WARN, __VA_ARGS__)
#define PGW_INFO(...) pgw_log_write(PGW_LOG_INFO, __VA_ARGS__)
#define PGW_DBG(...)  pgw_log_write(PGW_LOG_DEBUG, __VA_ARGS__)
#define PGW_TRC(...)  pgw_log_write(PGW_LOG_TRACE, __VA_ARGS__)

/* Parse "error|warn|info|debug|trace" (case-insensitive). False if unknown. */
bool pgw_log_parse_level(const char *text, pgw_level *out);

/* --- counters ------------------------------------------------------------- */

typedef enum pgw_stat {
    PGW_STAT_REQ = 0,     /* requests submitted */
    PGW_STAT_RESP,        /* responses received */
    PGW_STAT_ERR,         /* responses carrying an error */
    PGW_STAT_TIMEOUT,     /* requests abandoned after the deadline (ROB-05) */
    PGW_STAT_FLUSH,       /* cancellations sent (P9-06) */
    PGW_STAT_RETRY,       /* requests retried after a transient failure */
    PGW_STAT_SPLIT,       /* operations split across messages (P9-08) */
    PGW_STAT_QUEUE_FULL,  /* submissions deferred, queue was full (QNX-04) */
    PGW_STAT_BAD_RESP,    /* responses rejected by validation (P9-09) */
    PGW_STAT_INODE_HIT,   /* attribute cache hit (FS-13) */
    PGW_STAT_INODE_MISS,  /* attribute cache miss */
    PGW_STAT_BYTES_READ,
    PGW_STAT_BYTES_WRITTEN,
    PGW_STAT__MAX
} pgw_stat;

/* Counters are 64-bit and updated with relaxed atomics: a driver thread and an
 * interrupt thread may bump the same one. */
void pgw_stat_add(pgw_stat which, uint64_t delta);
uint64_t pgw_stat_get(pgw_stat which);
void pgw_stat_reset(void);
const char *pgw_stat_name(pgw_stat which);

/* Render the whole table as `name=value` lines into `out` (NUL-terminated).
 * Returns the number of bytes written excluding the terminator, or the size
 * that would have been needed when `cap` is too small. */
size_t pgw_stat_format(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PGW_LOG_H */

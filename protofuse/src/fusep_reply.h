/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_reply.h - Tiny reply contexts shared by the operations.
 *
 * Implements requirements:
 *   - VFS-03: one completion callback per reply shape, so that the operation
 *     layer never repeats the "check unique then decode" logic
 *   - ROB-06: every completion goes through the validating decoders
 *
 * Internal to protofuse.
 */
#ifndef PGW_FUSEP_REPLY_H
#define PGW_FUSEP_REPLY_H

#include "fusep_session.h"

#include "pgw/fusep.h"

#include <stddef.h>
#include <stdint.h>

/* One reply destination. */
typedef struct pgw_fusep_rep1 {
    uint64_t unique;
    void *a;
} pgw_fusep_rep1;

/* Two reply destinations (CREATE returns an entry and a file handle). */
typedef struct pgw_fusep_rep2 {
    uint64_t unique;
    void *a;
    void *b;
} pgw_fusep_rep2;

/* READLINK needs a capacity, READDIR a capacity plus a count. */
typedef struct pgw_fusep_rep3 {
    uint64_t unique;
    void *a;
    void *b;
    size_t cap;
} pgw_fusep_rep3;

pgw_err pgw_fusep_done_entry(void *ctx, const void *rx, size_t len);
pgw_err pgw_fusep_done_attr(void *ctx, const void *rx, size_t len);
pgw_err pgw_fusep_done_statfs(void *ctx, const void *rx, size_t len);
pgw_err pgw_fusep_done_open(void *ctx, const void *rx, size_t len);
pgw_err pgw_fusep_done_create(void *ctx, const void *rx, size_t len);
pgw_err pgw_fusep_done_read(void *ctx, const void *rx, size_t len);
pgw_err pgw_fusep_done_write(void *ctx, const void *rx, size_t len);
pgw_err pgw_fusep_done_readlink(void *ctx, const void *rx, size_t len);
pgw_err pgw_fusep_done_empty(void *ctx, const void *rx, size_t len);

#endif /* PGW_FUSEP_REPLY_H */

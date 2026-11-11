/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_session.h - Internal view of a FUSE session.
 *
 * Implements requirements:
 *   - VFS-02: the pending queue has two lanes. FORGET and INTERRUPT are marked
 *     `hiprio` and are always submitted first, with `hiprio = true` reaching the
 *     transport callback so that layer 1 can pick the priority virtqueue.
 *   - VFS-04: the session owns the reference-count table so that closing it can
 *     return every outstanding reference.
 *
 * Internal to protofuse; tests include this header to drive the queue directly.
 */
#ifndef PGW_FUSEP_SESSION_H
#define PGW_FUSEP_SESSION_H

#include "pgw/buf.h"
#include "pgw/err.h"
#include "pgw/fusep.h"

#include "fusep_abi.h"
#include "fusep_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The feature flags this layer announces in INIT and then honours. Anything the
 * server answers that is not in this mask is dropped, so the client never
 * silently relies on a feature it does not implement. */
#define PGW_FUSEP_FLAGS_ANNOUNCED                                            \
    (PGW_FUSE_F_ASYNC_READ | PGW_FUSE_F_BIG_WRITES | PGW_FUSE_F_MAX_PAGES)

typedef struct pgw_fuse_ref {
    uint64_t nodeid;
    uint64_t nlookup;
} pgw_fuse_ref;

typedef struct pgw_fuse_refs {
    pgw_fuse_ref *v;
    size_t n;
    size_t cap;
} pgw_fuse_refs;

void pgw_fusep_refs_init(pgw_fuse_refs *t);
void pgw_fusep_refs_free(pgw_fuse_refs *t);
void pgw_fusep_refs_clear(pgw_fuse_refs *t);
pgw_err pgw_fusep_refs_add(pgw_fuse_refs *t, uint64_t nodeid, uint64_t delta);
pgw_err pgw_fusep_refs_take(pgw_fuse_refs *t, uint64_t nodeid, uint64_t *out);
uint64_t pgw_fusep_refs_get(const pgw_fuse_refs *t, uint64_t nodeid);
size_t pgw_fusep_refs_count(const pgw_fuse_refs *t);
const pgw_fuse_ref *pgw_fusep_refs_at(const pgw_fuse_refs *t, size_t i);

/* Completion callback: called with the reply buffer once the transport has
 * returned. `rx_len` is 0 for requests that expect no reply (FORGET). */
typedef pgw_err (*pgw_fusep_done_fn)(void *ctx, const void *rx, size_t rx_len);

typedef struct pgw_fuse_slot {
    const void *tx;
    size_t tx_len;
    bool hiprio;
    bool wants_reply;
    uint32_t prio;
    uint32_t timeout_ms;
    pgw_fusep_done_fn done;
    void *ctx;
    pgw_err rc;
} pgw_fuse_slot;

struct pgw_fuse_session {
    pgw_fuse_rpc_fn rpc;
    void *user;

    uint32_t minor;         /* negotiated minor version (7.minor) */
    uint64_t flags;         /* negotiated feature flags */
    uint32_t max_write;     /* negotiated maximum write payload */
    uint32_t max_readahead; /* announced and accepted */
    uint32_t max_pages;
    uint32_t timeout_ms;
    uint64_t next_unique;

    pgw_fuse_slot *slots; /* pending requests, in submission order */
    size_t n_slots;
    size_t cap_slots;

    uint8_t *tx; /* staging buffer for large requests (WRITE payloads) */
    size_t tx_cap;
    uint8_t *rx; /* reply buffer shared by all requests of this session */
    size_t rx_cap;

    pgw_fuse_refs refs;

    /* Directory listing cache: one reply chunk kept decoded so that a caller
     * with a smaller array than the chunk resumes on the next entry instead of
     * asking the server for the same offset again. */
    pgw_fuse_dirent rd_cache[PGW_FUSE_READDIR_MAX];
    size_t rd_count;
    size_t rd_pos;
    uint64_t rd_nodeid;
    uint64_t rd_fh;
    uint64_t rd_off_start;
    bool rd_valid;
    bool rd_eof;

    uint64_t stat_hiprio; /* requests submitted on the priority lane */
    uint64_t stat_normal; /* requests submitted on a request lane */
};

/* --- helpers shared by the operation layer -------------------------------- */

uint64_t pgw_fusep_next_unique(pgw_fuse_session *s);

/* Enqueue a request. `tx` must stay valid until the matching pump returns. */
pgw_err pgw_fusep_submit(pgw_fuse_session *s, const void *tx, size_t tx_len,
                         bool hiprio, bool wants_reply, uint32_t prio,
                         pgw_fusep_done_fn done, void *ctx);

/* Submit everything pending, priority lane first. Returns the first error. */
pgw_err pgw_fusep_pump(pgw_fuse_session *s);

/* Submit a request that has no reply at all (FORGET, INTERRUPT) and pump it. */
pgw_err pgw_fusep_call_noreply(pgw_fuse_session *s, const void *tx,
                               size_t tx_len, bool hiprio, uint32_t prio);

/* Submit one request and pump it immediately (the usual path). */
pgw_err pgw_fusep_call(pgw_fuse_session *s, const void *tx, size_t tx_len,
                       bool hiprio, uint32_t prio, pgw_fusep_done_fn done,
                       void *ctx);

#endif /* PGW_FUSEP_SESSION_H */

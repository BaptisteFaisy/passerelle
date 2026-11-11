/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_close.c - Closing a session and returning every
 * outstanding node reference.
 *
 * Implements requirements:
 *   - VFS-04: on close, every node the server still believes we hold is given
 *     back. BATCH_FORGET is used when the server announced 7.16 or later, one
 *     FORGET per node otherwise, and the table is empty afterwards.
 *   - VFS-02: FORGET messages travel on the priority lane.
 */
#include "fusep_session.h"

#include "pgw/buf.h"
#include "pgw/log.h"

#include <stdlib.h>

#define PGW_FUSEP_CLOSE_TX_MAX                                               \
    (PGW_FUSE_IN_HDR_SZ + PGW_FUSE_BATCH_FORGET_IN +                         \
     PGW_FUSE_BATCH_MAX * PGW_FUSE_BATCH_FORGET_ONE)

static int32_t enc_batch_forget(void *buf, size_t cap, uint64_t unique,
                                const pgw_fuse_ref *v, size_t n)
{
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, PGW_FUSE_BATCH_FORGET, unique, 0);
    pgw_put_u32(&w, (uint32_t)n);
    pgw_put_u32(&w, 0); /* dummy */
    for (size_t i = 0; i < n; i++) {
        pgw_put_u64(&w, v[i].nodeid);
        pgw_put_u64(&w, v[i].nlookup);
    }
    return pgw_fusep_enc_end(&w);
}

/* Give back up to `n` references taken from the head of the table. */
static pgw_err forget_batch(pgw_fuse_session *s, size_t n, uint8_t *tx,
                            size_t tx_cap)
{
    pgw_fuse_ref local[PGW_FUSE_BATCH_MAX];
    if (n > PGW_FUSE_BATCH_MAX)
        n = PGW_FUSE_BATCH_MAX;
    for (size_t i = 0; i < n; i++) {
        const pgw_fuse_ref *r = pgw_fusep_refs_at(&s->refs, i);
        if (r == NULL)
            return PGW_EINVAL;
        local[i] = *r;
    }

    pgw_err rc;
    if (s->minor >= PGW_FUSE_MINOR_BATCH_FORGET) {
        int32_t len = enc_batch_forget(tx, tx_cap, pgw_fusep_next_unique(s),
                                       local, n);
        if (len <= 0)
            return PGW_EINVAL;
        rc = pgw_fusep_call_noreply(s, tx, (size_t)len, true, 0);
    } else {
        rc = PGW_OK;
        for (size_t i = 0; i < n; i++) {
            int32_t len = pgw_fuse_enc_forget(tx, tx_cap,
                                              pgw_fusep_next_unique(s),
                                              local[i].nodeid, local[i].nlookup);
            if (len <= 0)
                return PGW_EINVAL;
            pgw_err one =
                pgw_fusep_call_noreply(s, tx, (size_t)len, true, 0);
            if (one != PGW_OK && rc == PGW_OK)
                rc = one;
        }
    }
    /* The references are gone from our side whatever the server answered:
     * keeping them would only make the next FORGET carry a stale count. */
    for (size_t i = 0; i < n; i++) {
        uint64_t ignored = 0;
        (void)pgw_fusep_refs_take(&s->refs, local[i].nodeid, &ignored);
    }
    return rc;
}

void pgw_fuse_session_close(pgw_fuse_session *s)
{
    if (s == NULL)
        return;
    uint8_t tx[PGW_FUSEP_CLOSE_TX_MAX];
    pgw_err rc = PGW_OK;
    size_t guard = 0;
    while (pgw_fusep_refs_count(&s->refs) > 0) {
        size_t n = pgw_fusep_refs_count(&s->refs);
        if (n > PGW_FUSE_BATCH_MAX)
            n = PGW_FUSE_BATCH_MAX;
        pgw_err one = forget_batch(s, n, tx, sizeof(tx));
        if (one != PGW_OK && rc == PGW_OK)
            rc = one;
        if (++guard > 100000u) { /* defensive: the loop must always progress */
            PGW_ERR("fuse: refusing to spin while forgetting nodes");
            break;
        }
    }
    pgw_fusep_refs_clear(&s->refs);
    if (rc != PGW_OK)
        PGW_WARN("fuse: session closed with %s while forgetting nodes",
                 pgw_err_name(rc));

    pgw_fusep_refs_free(&s->refs);
    free(s->slots);
    free(s->tx);
    free(s->rx);
    free(s);
}

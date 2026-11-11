/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_node.c - Node bookkeeping: FORGET, INTERRUPT and the
 * read-only views on the reference table.
 *
 * Implements requirements:
 *   - VFS-04: FORGET gives back exactly the `nlookup` count we accumulated, in
 *     one message, and the node leaves the table whether or not the transport
 *     accepted the message (a stale count would corrupt the server's own view)
 *   - VFS-02: both FORGET and INTERRUPT are submitted on the priority lane, so
 *     the transport callback receives `hiprio = true`
 */
#include "fusep_ops.h"

#include "pgw/buf.h"
#include "pgw/fusep.h"

uint64_t pgw_fuse_ref_get(const pgw_fuse_session *s, uint64_t nodeid)
{
    if (s == NULL)
        return 0;
    return pgw_fusep_refs_get(&s->refs, nodeid);
}

size_t pgw_fuse_ref_count_nodes(const pgw_fuse_session *s)
{
    if (s == NULL)
        return 0;
    return pgw_fusep_refs_count(&s->refs);
}

pgw_err pgw_fuse_forget_node(pgw_fuse_session *s, uint64_t nodeid)
{
    if (s == NULL)
        return PGW_EINVAL;
    uint64_t nlookup = pgw_fusep_refs_get(&s->refs, nodeid);
    if (nlookup == 0)
        return PGW_OK; /* never looked up, or already forgotten: idempotent */

    uint8_t tx[PGW_FUSE_TX_SMALL];
    uint64_t u = pgw_fusep_next_unique(s);
    int32_t n = pgw_fuse_enc_forget(tx, sizeof(tx), u, nodeid, nlookup);
    if (n <= 0)
        return PGW_EINVAL;
    /* FORGET has no reply, and it must not wait behind ordinary requests. */
    pgw_err rc = pgw_fusep_call_noreply(s, tx, (size_t)n, true, 0);

    uint64_t taken = 0;
    (void)pgw_fusep_refs_take(&s->refs, nodeid, &taken);
    return rc;
}

pgw_err pgw_fuse_interrupt(pgw_fuse_session *s, uint64_t target_unique)
{
    if (s == NULL || target_unique == 0)
        return PGW_EINVAL;
    uint8_t tx[PGW_FUSE_TX_SMALL];
    uint64_t u = pgw_fusep_next_unique(s);
    int32_t n = pgw_fuse_enc_interrupt(tx, sizeof(tx), u, target_unique);
    if (n <= 0)
        return PGW_EINVAL;
    /* The kernel does not wait for the INTERRUPT reply, so neither do we: it
     * goes on the priority lane and the reply, if any, is discarded. */
    return pgw_fusep_call_noreply(s, tx, (size_t)n, true, 0);
}

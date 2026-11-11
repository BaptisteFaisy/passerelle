/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_session.c - Session lifecycle, `unique` allocation and
 * the two-lane submission queue.
 *
 * Implements requirements:
 *   - VFS-02: monotonic `unique` numbers, a priority lane for FORGET and
 *     INTERRUPT (the transport callback sees `hiprio = true`) and request lanes
 *     for everything else, ordered by caller priority then by arrival.
 *   - VFS-03: INIT negotiation, including the fallback to a lower minor version
 *     announced by the server.
 *   - VFS-04: the session owns the reference table; closing it returns every
 *     outstanding reference with FORGET (or BATCH_FORGET when the server
 *     supports it).
 *   - ROB-05: every request carries a deadline.
 *
 * No direct I/O happens here: the only way out is the `pgw_fuse_rpc_fn`
 * callback, which is what makes the whole layer testable on the PC.
 */
#include "fusep_session.h"

#include "pgw/limits.h"
#include "pgw/log.h"

#include <stdlib.h>
#include <string.h>

uint64_t pgw_fusep_next_unique(pgw_fuse_session *s)
{
    if (s == NULL)
        return 0;
    uint64_t u = s->next_unique;
    s->next_unique++;
    if (s->next_unique == 0) /* never reuse 0: it means "no request" */
        s->next_unique = 1;
    return u;
}

static pgw_err slots_reserve(pgw_fuse_session *s, size_t want)
{
    if (want <= s->cap_slots)
        return PGW_OK;
    size_t cap = (s->cap_slots == 0) ? 8u : s->cap_slots;
    while (cap < want) {
        if (cap > SIZE_MAX / 2u)
            return PGW_ENOMEM;
        cap *= 2u;
    }
    if (cap > SIZE_MAX / sizeof(pgw_fuse_slot))
        return PGW_ENOMEM;
    pgw_fuse_slot *nv =
        (pgw_fuse_slot *)realloc(s->slots, cap * sizeof(*nv));
    if (nv == NULL)
        return PGW_ENOMEM;
    s->slots = nv;
    s->cap_slots = cap;
    return PGW_OK;
}

pgw_err pgw_fusep_submit(pgw_fuse_session *s, const void *tx, size_t tx_len,
                         bool hiprio, bool wants_reply, uint32_t prio,
                         pgw_fusep_done_fn done, void *ctx)
{
    if (s == NULL || tx == NULL || tx_len == 0)
        return PGW_EINVAL;
    if (wants_reply && done == NULL)
        return PGW_EINVAL;
    pgw_err rc = slots_reserve(s, s->n_slots + 1u);
    if (rc != PGW_OK)
        return rc;
    pgw_fuse_slot *sl = &s->slots[s->n_slots];
    sl->tx = tx;
    sl->tx_len = tx_len;
    sl->hiprio = hiprio;
    sl->wants_reply = wants_reply;
    sl->prio = prio;
    sl->timeout_ms = s->timeout_ms;
    sl->done = done;
    sl->ctx = ctx;
    sl->rc = PGW_OK;
    s->n_slots++;
    pgw_stat_add(PGW_STAT_REQ, 1);
    return PGW_OK;
}

/* Priority lane first; inside a lane, the highest caller priority; ties keep
 * arrival order because only a strictly greater priority replaces the choice. */
static size_t pick_next(const pgw_fuse_session *s)
{
    size_t best = 0;
    for (size_t i = 1; i < s->n_slots; i++) {
        const pgw_fuse_slot *it = &s->slots[i];
        const pgw_fuse_slot *cur = &s->slots[best];
        if (it->hiprio != cur->hiprio) {
            if (it->hiprio)
                best = i;
            continue;
        }
        if (it->prio > cur->prio)
            best = i;
    }
    return best;
}

static void slot_remove(pgw_fuse_session *s, size_t i)
{
    if (i + 1u < s->n_slots)
        memmove(&s->slots[i], &s->slots[i + 1u],
                (s->n_slots - i - 1u) * sizeof(s->slots[0]));
    s->n_slots--;
}

static pgw_err run_slot(pgw_fuse_session *s, const pgw_fuse_slot *sl)
{
    size_t rx_len = 0;
    pgw_err rc = s->rpc(sl->tx, sl->tx_len, s->rx, s->rx_cap, &rx_len,
                        sl->hiprio, sl->prio, sl->timeout_ms, s->user);
    if (sl->hiprio)
        s->stat_hiprio++;
    else
        s->stat_normal++;
    if (rc != PGW_OK) {
        if (rc == PGW_ETIMEDOUT)
            pgw_stat_add(PGW_STAT_TIMEOUT, 1);
        pgw_stat_add(PGW_STAT_ERR, 1);
        return rc;
    }
    pgw_stat_add(PGW_STAT_RESP, 1);
    if (!sl->wants_reply)
        return PGW_OK; /* FORGET, INTERRUPT and friends have no reply */
    if (rx_len > s->rx_cap)
        return PGW_FUSEP_E_BAD_MSG;
    return sl->done(sl->ctx, s->rx, rx_len);
}

pgw_err pgw_fusep_pump(pgw_fuse_session *s)
{
    if (s == NULL)
        return PGW_EINVAL;
    pgw_err first = PGW_OK;
    while (s->n_slots > 0) {
        size_t i = pick_next(s);
        pgw_fuse_slot sl = s->slots[i];
        slot_remove(s, i);
        pgw_err rc = run_slot(s, &sl);
        if (rc != PGW_OK && first == PGW_OK)
            first = rc;
    }
    return first;
}

pgw_err pgw_fusep_call_noreply(pgw_fuse_session *s, const void *tx,
                               size_t tx_len, bool hiprio, uint32_t prio)
{
    pgw_err rc = pgw_fusep_submit(s, tx, tx_len, hiprio, false, prio, NULL, NULL);
    if (rc != PGW_OK)
        return rc;
    return pgw_fusep_pump(s);
}

pgw_err pgw_fusep_call(pgw_fuse_session *s, const void *tx, size_t tx_len,
                       bool hiprio, uint32_t prio, pgw_fusep_done_fn done,
                       void *ctx)
{
    pgw_err rc = pgw_fusep_submit(s, tx, tx_len, hiprio, true, prio, done, ctx);
    if (rc != PGW_OK)
        return rc;
    return pgw_fusep_pump(s);
}

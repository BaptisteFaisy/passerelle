/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/src/p9_session.c - session lifecycle and the request plumbing.
 *
 * A session is the only place that talks to the outside world, and it does so
 * exclusively through the pgw_p9_rpc_fn callback handed to
 * pgw_p9_session_open(): no socket, no virtqueue, no file descriptor. That is
 * what lets the whole protocol layer be tested on the PC against an in-memory
 * server (P9-10) and reused unchanged on QNX.
 *
 * This file covers:
 *   - P9-01: Tversion negotiation, msize clamped to what we asked for;
 *   - P9-02: Tattach, giving the root fid;
 *   - P9-05: one tag per request, up to PGW_P9_SLOTS requests in flight;
 *   - P9-06: a request that times out or is abandoned is cancelled with Tflush;
 *   - P9-07: every fid and tag comes from a pool and goes back to it;
 *   - PERF-01: every round trip is counted in the shared pgw_stat_* table.
 */
/* nanosleep() is POSIX, not C11: the feature macro comes first. */
#define _POSIX_C_SOURCE 200809L

#include "p9_session_priv.h"

#include "pgw/log.h"
#include "pgw/limits.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Bounded wait for a free slot: a nested call needs one immediately, so we
 * spin a little instead of failing the whole operation at the first miss. When
 * the transport is saturated (more clients than slots), the wait continues -
 * sleeping between the spins - until the caller's own deadline: giving up
 * there turned a busy mount into a failed open. */
#define PGW_P9_SLOT_SPINS 1000000u
#define PGW_P9_SLOT_SLEEPS 10000u /* 10 s of 1 ms sleeps, then give up */

static void slot_release(pgw_p9_session *s, unsigned slot)
{
    atomic_fetch_and_explicit(&s->busy, ~(1u << slot), memory_order_acq_rel);
}

static pgw_err slot_acquire(pgw_p9_session *s, unsigned *slot)
{
    const uint32_t all = (uint32_t)((1u << PGW_P9_SLOTS) - 1u);
    for (uint32_t sleeps = 0; sleeps <= PGW_P9_SLOT_SLEEPS; sleeps++) {
    for (uint32_t tries = 0; tries < PGW_P9_SLOT_SPINS; tries++) {
        uint32_t busy = atomic_load_explicit(&s->busy, memory_order_acquire);
        if (busy == all)
            continue;
        for (unsigned i = 0; i < PGW_P9_SLOTS; i++) {
            uint32_t bit = 1u << i;
            if ((busy & bit) != 0)
                continue;
            if (atomic_compare_exchange_weak_explicit(
                    &s->busy, &busy, busy | bit, memory_order_acq_rel,
                    memory_order_relaxed)) {
                *slot = i;
                return PGW_OK;
            }
            break; /* the mask moved: reload it and retry */
        }
    }
    /* Saturated: give the holders a chance to finish before trying again. */
    {
        struct timespec ts = { 0, 1000000L }; /* 1 ms */
        (void)nanosleep(&ts, NULL);
    }
    }
    return PGW_EAGAIN;
}

pgw_err pgw_p9__fid_alloc(pgw_p9_session *s, uint32_t *fid)
{
    return pgw_p9_pool_alloc(s->fids, fid) ? PGW_OK : PGW_ENFILE;
}

void pgw_p9__fid_free(pgw_p9_session *s, uint32_t fid)
{
    pgw_p9_pool_free(s->fids, fid);
}

uint32_t pgw_p9__chunk(const pgw_p9_session *s)
{
    if (s == NULL || s->msize <= PGW_P9_IO_OVERHEAD)
        return 1;
    return s->msize - PGW_P9_IO_OVERHEAD;
}

pgw_err pgw_p9__begin(pgw_p9_session *s, unsigned *slot, uint32_t *tag)
{
    if (s == NULL || slot == NULL || tag == NULL)
        return PGW_EINVAL;
    pgw_err e = slot_acquire(s, slot);
    if (e != PGW_OK)
        return e;
    if (!pgw_p9_pool_alloc(s->tags, tag)) {
        slot_release(s, *slot);
        return PGW_ENFILE;
    }
    return PGW_OK;
}

void pgw_p9__finish(pgw_p9_session *s, unsigned slot, uint32_t tag)
{
    if (s == NULL)
        return;
    pgw_p9_pool_free(s->tags, tag);
    slot_release(s, slot);
}

static void flush_after_timeout(pgw_p9_session *s, uint16_t tag, uint32_t prio);

/* Lowest layer: one request, one reply, no interpretation of the reply. */
static pgw_err raw_call(pgw_p9_session *s, unsigned slot, size_t tx_len,
                        uint16_t tag, uint32_t prio, size_t *rx_len)
{
    struct pgw_p9_slot *sl = &s->slots[slot];
    if (tx_len > s->msize)
        return PGW_E2BIG;
    sl->rx_len = 0;
    pgw_stat_add(PGW_STAT_REQ, 1);
    pgw_err e = s->rpc(sl->tx, tx_len, sl->rx, s->msize, &sl->rx_len, prio,
                       s->timeout_ms, s->user);
    if (e != PGW_OK) {
        if (e == PGW_ETIMEDOUT) {
            pgw_stat_add(PGW_STAT_TIMEOUT, 1);
            /* P9-06: tell the server to forget the request. Done here so that
             * no caller can forget it, and guarded so a flush that times out
             * does not start flushing itself. */
            flush_after_timeout(s, tag, prio);
        }
        return e;
    }
    pgw_stat_add(PGW_STAT_RESP, 1);
    if (sl->rx_len == 0 || sl->rx_len > s->msize) {
        pgw_stat_add(PGW_STAT_BAD_RESP, 1);
        return PGW_EINVAL;
    }
    *rx_len = sl->rx_len;
    return PGW_OK;
}

pgw_err pgw_p9__call(pgw_p9_session *s, unsigned slot, int32_t enc, uint32_t tag,
                     uint8_t rtype, uint32_t prio, size_t *msg_len)
{
    if (enc < 0)
        return enc == -(int32_t)PGW_EMSGSIZE ? PGW_E2BIG : PGW_EINVAL;
    size_t rlen = 0;
    pgw_err e = raw_call(s, slot, (size_t)enc, (uint16_t)tag, prio, &rlen);
    if (e != PGW_OK)
        return e;
    const uint8_t *rx = s->slots[slot].rx;
    pgw_p9_hdr h;
    e = pgw_p9_dec_hdr(rx, rlen, &h);
    if (e != PGW_OK || h.tag != (uint16_t)tag) {
        pgw_stat_add(PGW_STAT_BAD_RESP, 1);
        return e != PGW_OK ? e : PGW_EINVAL;
    }
    if (h.type == PGW_P9_RLERROR) {
        uint32_t ecode = 0;
        e = pgw_p9_dec_rlerror(rx, rlen, h.tag, &ecode);
        if (e != PGW_OK) {
            pgw_stat_add(PGW_STAT_BAD_RESP, 1);
            return e;
        }
        pgw_stat_add(PGW_STAT_ERR, 1);
        return pgw_p9__err_from_wire(ecode);
    }
    if (h.type != rtype) {
        pgw_stat_add(PGW_STAT_BAD_RESP, 1);
        return PGW_EINVAL;
    }
    if (msg_len != NULL)
        *msg_len = h.size;
    return PGW_OK;
}

/* Send Tflush for `tag`; best effort, never recursive, never allocates. */
static void flush_after_timeout(pgw_p9_session *s, uint16_t tag, uint32_t prio)
{
    if (s == NULL || s->flushing)
        return;
    s->flushing = true;
    unsigned slot = 0;
    if (slot_acquire(s, &slot) == PGW_OK) {
        uint32_t ftag = 0;
        if (pgw_p9_pool_alloc(s->tags, &ftag)) {
            int32_t enc = pgw_p9_enc_tflush(s->slots[slot].tx, s->msize,
                                            (uint16_t)ftag, tag);
            if (enc > 0) {
                size_t rlen = 0;
                (void)raw_call(s, slot, (size_t)enc, (uint16_t)ftag, prio, &rlen);
            }
            pgw_p9_pool_free(s->tags, ftag);
        }
        slot_release(s, slot);
    }
    s->flushing = false;
    pgw_stat_add(PGW_STAT_FLUSH, 1);
}

/* --- version and attach (P9-01, P9-02) ------------------------------------ */

static pgw_err do_version(pgw_p9_session *s, uint32_t want)
{
    unsigned slot = 0;
    pgw_err e = slot_acquire(s, &slot);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_tversion(s->slots[slot].tx, s->msize, PGW_TAG_NONE,
                                      want, PGW_P9_VERSION);
    size_t mlen = 0;
    e = pgw_p9__call(s, slot, enc, PGW_TAG_NONE, PGW_P9_RVERSION, 0, &mlen);
    if (e == PGW_OK) {
        uint32_t msize = 0;
        char ver[16];
        e = pgw_p9_dec_rversion(s->slots[slot].rx, mlen, PGW_TAG_NONE, &msize,
                                ver, sizeof ver);
        if (e == PGW_OK) {
            if (strcmp(ver, PGW_P9_VERSION) != 0)
                e = PGW_EOPNOTSUPP; /* the server does not speak .L */
            else if (msize > want || msize < PGW_P9_MSIZE_FLOOR)
                e = PGW_EINVAL; /* a server may lower msize, never raise it */
            else
                s->msize = msize;
        }
    }
    slot_release(s, slot);
    return e;
}

static pgw_err do_attach(pgw_p9_session *s, const char *uname, const char *aname,
                         uint32_t uid)
{
    unsigned slot = 0;
    uint32_t tag = 0;
    pgw_err e = pgw_p9__begin(s, &slot, &tag);
    if (e != PGW_OK)
        return e;
    uint32_t fid = 0;
    e = pgw_p9__fid_alloc(s, &fid);
    if (e != PGW_OK) {
        pgw_p9__finish(s, slot, tag);
        return e;
    }
    int32_t enc = pgw_p9_enc_tattach(s->slots[slot].tx, s->msize, (uint16_t)tag,
                                     fid, PGW_FID_NONE, uname, aname, uid);
    size_t mlen = 0;
    e = pgw_p9__call(s, slot, enc, tag, PGW_P9_RATTACH, 0, &mlen);
    if (e == PGW_OK) {
        pgw_p9_qid qid;
        e = pgw_p9_dec_rattach(s->slots[slot].rx, mlen, (uint16_t)tag, &qid);
        if (e == PGW_OK)
            s->root_fid = fid;
    }
    if (e != PGW_OK)
        pgw_p9__fid_free(s, fid);
    pgw_p9__finish(s, slot, tag);
    return e;
}

/* The allocator the session uses for its buffer block (VIO-07). */
static pgw_p9_alloc_fn g_alloc = NULL;
static pgw_p9_free_fn g_free = NULL;
static void *g_alloc_user = NULL;

void pgw_p9_session_set_memory(pgw_p9_alloc_fn alloc, pgw_p9_free_fn free_fn,
                               void *user)
{
    g_alloc = alloc;
    g_free = free_fn;
    g_alloc_user = user;
}

static void *session_alloc(size_t bytes)
{
    if (g_alloc != NULL)
        return g_alloc(bytes, g_alloc_user);
    return calloc(1, bytes);
}

static void session_release(void *ptr, size_t bytes)
{
    if (ptr == NULL)
        return;
    if (g_free != NULL)
        g_free(ptr, bytes, g_alloc_user);
    else
        free(ptr);
}

static void session_free(pgw_p9_session *s)
{
    if (s == NULL)
        return;
    if (s->slots[0].tx != NULL)
        session_release(s->slots[0].tx, s->block_bytes); /* one block */
    pgw_p9_pool_destroy(s->tags);
    pgw_p9_pool_destroy(s->fids);
    free(s);
}

pgw_p9_session *pgw_p9_session_open(pgw_p9_rpc_fn rpc, void *user,
                                    uint32_t msize_req, const char *uname,
                                    const char *aname, uint32_t uid)
{
    if (rpc == NULL)
        return NULL;
    uint32_t msize = msize_req;
    if (msize < PGW_P9_MSIZE_FLOOR)
        msize = PGW_P9_MSIZE_FLOOR;
    if (msize > PGW_MSIZE_MAX)
        msize = PGW_MSIZE_MAX;
    pgw_p9_session *s = calloc(1, sizeof *s);
    if (s == NULL)
        return NULL;
    s->rpc = rpc;
    s->user = user;
    s->msize = msize;
    s->timeout_ms = PGW_TIMEOUT_MS_DEFAULT;
    s->root_fid = PGW_FID_NONE;
    atomic_store_explicit(&s->busy, 0u, memory_order_relaxed);
    size_t block = (size_t)PGW_P9_SLOTS * 2u * (size_t)msize;
    uint8_t *bufs = session_alloc(block);
    s->block_bytes = block;
    if (bufs == NULL) {
        session_free(s);
        return NULL;
    }
    for (unsigned i = 0; i < PGW_P9_SLOTS; i++) {
        s->slots[i].tx = bufs + (size_t)i * 2u * (size_t)msize;
        s->slots[i].rx = s->slots[i].tx + msize;
        s->slots[i].rx_len = 0;
    }
    s->tags = pgw_p9_pool_create(0, PGW_P9_MAX_TAGS);
    s->fids = pgw_p9_pool_create(0, PGW_P9_MAX_FIDS);
    if (s->tags == NULL || s->fids == NULL) {
        session_free(s);
        return NULL;
    }
    const char *u = uname != NULL ? uname : "";
    const char *a = aname != NULL ? aname : "";
    pgw_err e = do_version(s, msize);
    if (e == PGW_OK)
        e = do_attach(s, u, a, uid);
    if (e != PGW_OK) {
        PGW_WARN("p9: session open failed: %s", pgw_err_name(e));
        session_free(s);
        return NULL;
    }
    return s;
}

void pgw_p9_session_close(pgw_p9_session *s)
{
    if (s == NULL)
        return;
    if (s->root_fid != PGW_FID_NONE) {
        (void)pgw_p9_clunk(s, s->root_fid, 0);
        s->root_fid = PGW_FID_NONE;
    }
    session_free(s);
}

uint32_t pgw_p9_session_msize(const pgw_p9_session *s)
{
    return s != NULL ? s->msize : 0;
}

uint32_t pgw_p9_session_root_fid(const pgw_p9_session *s)
{
    return s != NULL ? s->root_fid : PGW_FID_NONE;
}

uint32_t pgw_p9_session_open_fids(const pgw_p9_session *s)
{
    return s != NULL ? pgw_p9_pool_in_use(s->fids) : 0;
}

void pgw_p9_session_set_timeout(pgw_p9_session *s, uint32_t timeout_ms)
{
    if (s == NULL)
        return;
    if (timeout_ms > PGW_TIMEOUT_MS_MAX)
        timeout_ms = PGW_TIMEOUT_MS_MAX;
    s->timeout_ms = timeout_ms;
}

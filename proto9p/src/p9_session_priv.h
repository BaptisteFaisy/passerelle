/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/src/p9_session_priv.h - internals shared by the session files.
 *
 * The public contract (pgw/p9.h) exposes pgw_p9_session as an opaque handle.
 * Inside proto9p/ the three session translation units need the same structure
 * and the same request plumbing, so it lives here, once: session.c owns it,
 * session_io.c and session_ns.c only call it.
 *
 * Concurrency (P9-05): a session owns PGW_P9_SLOTS request/reply buffer pairs.
 * Every operation takes a free slot and its own tag, so a request can be sent
 * while another is still in flight, and a call made from inside the RPC
 * callback (a nested lookup during a readdir, say) never overwrites the bytes
 * of the request it is nested in.
 */
#ifndef PGW_P9_SESSION_PRIV_H
#define PGW_P9_SESSION_PRIV_H

#include "p9_priv.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

struct pgw_p9_slot {
    uint8_t *tx;   /* request buffer, msize bytes */
    uint8_t *rx;   /* reply buffer, msize bytes */
    size_t rx_len; /* bytes of the reply in `rx` */
};

struct pgw_p9_session {
    pgw_p9_rpc_fn rpc;
    void *user;
    uint32_t msize;     /* negotiated, never above what we asked for */
    uint32_t timeout_ms;/* handed to the transport (ROB-05) */
    uint32_t root_fid;  /* fid returned by Tattach */
    pgw_p9_idpool *tags;
    pgw_p9_idpool *fids;
    bool flushing; /* a Tflush is in progress: never flush a flush (P9-06) */
    size_t block_bytes; /* size of the buffer block, for the free hook */
    _Atomic uint32_t busy; /* one bit per slot, 1 = in use */
    struct pgw_p9_slot slots[PGW_P9_SLOTS];
};

/* Take a free slot and a free tag. */
pgw_err pgw_p9__begin(pgw_p9_session *s, unsigned *slot, uint32_t *tag);
void pgw_p9__finish(pgw_p9_session *s, unsigned slot, uint32_t tag);

/* Send what was encoded into s->slots[slot].tx and validate the envelope:
 * header, tag, Rlerror (turned into a pgw_err) or the expected reply type.
 * On PGW_OK the reply is at s->slots[slot].rx with length .rx_len. */
pgw_err pgw_p9__call(pgw_p9_session *s, unsigned slot, int32_t enc, uint32_t tag,
                     uint8_t rtype, uint32_t prio, size_t *msg_len);

/* Largest payload that fits in one message, both directions (P9-08). */
uint32_t pgw_p9__chunk(const pgw_p9_session *s);

pgw_err pgw_p9__fid_alloc(pgw_p9_session *s, uint32_t *fid);
void pgw_p9__fid_free(pgw_p9_session *s, uint32_t fid);

/* One in-flight request: a slot (buffers) plus a tag. The helpers below are
 * the only way the operation files touch the slot buffers, so the "encode,
 * send, decode, release" sequence is written exactly once. */
struct pgw_p9_call {
    pgw_p9_session *s;
    unsigned slot;
    uint32_t tag;
};

static inline uint8_t *pgw_p9__call_tx(const struct pgw_p9_call *c)
{
    return c->s->slots[c->slot].tx;
}

static inline const uint8_t *pgw_p9__call_rx(const struct pgw_p9_call *c)
{
    return c->s->slots[c->slot].rx;
}

static inline pgw_err pgw_p9__call_begin(pgw_p9_session *s,
                                         struct pgw_p9_call *c)
{
    if (s == NULL || c == NULL)
        return PGW_EINVAL;
    c->s = s;
    return pgw_p9__begin(s, &c->slot, &c->tag);
}

static inline pgw_err pgw_p9__call_send(struct pgw_p9_call *c, int32_t enc,
                                        uint8_t rtype, uint32_t prio,
                                        size_t *mlen)
{
    return pgw_p9__call(c->s, c->slot, enc, c->tag, rtype, prio, mlen);
}

static inline void pgw_p9__call_end(struct pgw_p9_call *c)
{
    pgw_p9__finish(c->s, c->slot, c->tag);
}

#endif /* PGW_P9_SESSION_PRIV_H */

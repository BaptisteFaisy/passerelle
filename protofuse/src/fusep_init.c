/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_init.c - Opening a session and negotiating the protocol.
 *
 * Implements requirements:
 *   - VFS-03: we announce FUSE 7.36 with our feature flags and max_readahead,
 *     then adopt the server's answer. When the server announces a lower minor
 *     version we fall back to it (and stop using the features that appeared
 *     after it) instead of failing: a virtiofsd older than our header must
 *     still work.
 *   - ROB-05: a session is never opened without a deadline on INIT.
 */
#include "fusep_session.h"

#include "pgw/buf.h"
#include "pgw/limits.h"
#include "pgw/log.h"

#include <stdlib.h>
#include <string.h>

/* Smallest write the layer is willing to work with: below one page the
 * protocol is unusable in practice. */
#define PGW_FUSEP_MIN_MAX_WRITE PGW_FUSE_PAGE_SIZE

/* Decode the INIT reply into `info`; `unique` travels through the context. */
typedef struct init_ctx {
    uint64_t unique;
    pgw_fuse_init_info info;
} init_ctx;

static pgw_err init_done_full(void *ctx, const void *rx, size_t rx_len)
{
    init_ctx *c = (init_ctx *)ctx;
    return pgw_fuse_dec_init(rx, rx_len, c->unique, &c->info);
}

static pgw_err negotiate(pgw_fuse_session *s, uint32_t want_write)
{
    uint8_t tx[PGW_FUSE_IN_HDR_SZ + PGW_FUSE_INIT_IN_SIZE];
    init_ctx c;
    c.unique = pgw_fusep_next_unique(s);
    int32_t n = pgw_fuse_enc_init(tx, sizeof(tx), c.unique,
                                  PGW_FUSE_MAX_READAHEAD,
                                  PGW_FUSEP_FLAGS_ANNOUNCED);
    if (n <= 0)
        return PGW_EINVAL;
    pgw_err rc = pgw_fusep_call(s, tx, (size_t)n, false, 0, init_done_full, &c);
    if (rc != PGW_OK)
        return rc;

    const pgw_fuse_init_info *in = &c.info;
    if (in->major != PGW_FUSE_MAJOR) {
        PGW_ERR("fuse: server speaks protocol major %u, we only speak %u",
                (unsigned)in->major, (unsigned)PGW_FUSE_MAJOR);
        return PGW_ENOSYS;
    }
    if (in->minor < PGW_FUSE_MINOR_FLOOR) {
        PGW_ERR("fuse: server minor %u is below the supported floor %u",
                (unsigned)in->minor, (unsigned)PGW_FUSE_MINOR_FLOOR);
        return PGW_ENOSYS;
    }

    s->minor = (in->minor < PGW_FUSE_MINOR) ? in->minor : PGW_FUSE_MINOR;
    s->flags = in->flags & PGW_FUSEP_FLAGS_ANNOUNCED;
    s->max_readahead = (in->max_readahead < PGW_FUSE_MAX_READAHEAD)
                           ? in->max_readahead
                           : PGW_FUSE_MAX_READAHEAD;

    uint32_t mw = (in->max_write == 0) ? want_write : in->max_write;
    if (mw > want_write)
        mw = want_write; /* never ask for more than our buffers hold */
    if (mw < PGW_FUSEP_MIN_MAX_WRITE)
        return PGW_ENOSYS;
    s->max_write = mw;

    s->max_pages = 1;
    if (s->minor >= PGW_FUSE_MINOR_MAX_PAGES && in->max_pages > 0)
        s->max_pages = (in->max_pages < PGW_FUSE_MAX_PAGES_CAP)
                           ? in->max_pages
                           : PGW_FUSE_MAX_PAGES_CAP;

    if (s->minor < PGW_FUSE_MINOR)
        PGW_INFO("fuse: server announced 7.%u, running at 7.%u",
                 (unsigned)in->minor, (unsigned)s->minor);
    return PGW_OK;
}

static void session_destroy(pgw_fuse_session *s)
{
    if (s == NULL)
        return;
    pgw_fusep_refs_free(&s->refs);
    free(s->slots);
    free(s->tx);
    free(s->rx);
    free(s);
}

pgw_fuse_session *pgw_fuse_session_open(pgw_fuse_rpc_fn rpc, void *user,
                                        uint32_t max_write)
{
    if (rpc == NULL)
        return NULL;
    uint32_t want = (max_write == 0) ? PGW_MSIZE_DEFAULT : max_write;
    if (want < PGW_MSIZE_MIN)
        want = PGW_MSIZE_MIN;
    if (want > PGW_MSIZE_MAX)
        want = PGW_MSIZE_MAX;

    pgw_fuse_session *s = (pgw_fuse_session *)calloc(1, sizeof(*s));
    if (s == NULL)
        return NULL;
    s->rpc = rpc;
    s->user = user;
    s->next_unique = 1;
    s->timeout_ms = PGW_TIMEOUT_MS_DEFAULT;
    pgw_fusep_refs_init(&s->refs);

    /* The reply buffer must hold a full READ answer: header plus max_write. */
    s->rx_cap = (size_t)PGW_FUSE_OUT_HDR_SZ + (size_t)want + 4096u;
    s->rx = (uint8_t *)malloc(s->rx_cap);
    /* The staging buffer holds a WRITE: header plus max_write payload. */
    s->tx_cap = (size_t)PGW_FUSE_IN_HDR_SZ + (size_t)want + 256u;
    s->tx = (uint8_t *)malloc(s->tx_cap);
    if (s->rx == NULL || s->tx == NULL) {
        session_destroy(s);
        return NULL;
    }

    if (negotiate(s, want) != PGW_OK) {
        session_destroy(s);
        return NULL;
    }
    return s;
}

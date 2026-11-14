/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fsfuse/src/dispatch_qnx.c - the seam between the QNX resource
 * manager, the FUSE session and the virtqueues.
 *
 * The virtiofs device exposes two classes of queues (VFS-01):
 *
 *   queue 0        the *hiprio* queue, which carries only the messages that
 *                  must not wait behind a bulk transfer: FORGET and INTERRUPT
 *                  (VFS-02);
 *   queues 1..n    the request queues, which carry everything else.
 *
 * The RPC callback of layer 2 asks for the hiprio class with its `hiprio`
 * argument; this file is what turns that into a queue choice, so the priority
 * lane is a property of the transport and not of the protocol code.
 *
 * The wait is bounded by the mount's timeout (ROB-05) and no allocation happens
 * on the request path.
 *
 * Covered requirements: VFS-01, VFS-02, QNX-03, ROB-05, VIO-07.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"
#include "pgw/cookie.h"
#include "pgw/fsfuse.h"
#include "pgw/fsfuse_qnx.h"
#include "pgw/fsfuse_op.h"
#include "pgw/fusep.h"
#include "pgw/log.h"
#include "pgw/time.h"
#include "pgw/virtio.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The transport state of one mount. */
typedef struct fsfuse_xport {
    fsfuse_dev *dev;
    volatile unsigned irqs;
    uint32_t timeout_ms;
    uint32_t next_request; /* round-robin over the request queues */
} fsfuse_xport;

static fsfuse_xport g_xport;
static pgw_fuse_session *g_session;
/* The virtqueue is shared by all clients of this mount. A cookie is therefore
 * an atomic draw, not a local increment in the thread that submits. */
static pgw_cookie g_unique_next = PGW_COOKIE_INITIALIZER;

void fsfuse_xport_irq(void *user)
{
    fsfuse_xport *x = user;
    if (x != NULL)
        x->irqs++;
}

/* The queue a message goes to: the hiprio lane for FORGET and INTERRUPT, a
 * request queue for everything else (VFS-02). */
static pgw_virtq *pick_queue(fsfuse_xport *x, bool hiprio)
{
    if (hiprio)
        return x->dev->hiprio;
    if (x->dev->num_requests == 0u)
        return NULL;
    uint16_t idx = (uint16_t)(x->next_request % x->dev->num_requests);
    x->next_request = (x->next_request + 1u) % x->dev->num_requests;
    return x->dev->requests[idx];
}

/* Wait until the used ring of `vq` has an entry for `cookie`. */
static pgw_err wait_completion(fsfuse_xport *x, pgw_virtq *vq, void *cookie,
                               uint32_t *out_len, uint32_t timeout_ms)
{
    pgw_deadline dl = pgw_deadline_from_ms(timeout_ms);
    unsigned seen = x->irqs;
    for (;;) {
        /* A reply popped by another waiter is kept for its owner. */
        uint32_t kept = 0;
        if (pgw_reqtab_take(cookie, &kept)) {
            *out_len = kept;
            return PGW_OK;
        }
        void *got = NULL;
        uint32_t len = 0;
        pgw_err e = pgw_vq_pop(vq, &got, &len);
        if (e == PGW_OK) {
            if (got == cookie) {
                *out_len = len;
                return PGW_OK;
            }
            pgw_reqtab_keep(got, len); /* it belongs to another request */
        }
        if (e != PGW_OK && e != PGW_EAGAIN)
            return e;
        if (pgw_deadline_expired(dl)) {
            pgw_stat_add(PGW_STAT_TIMEOUT, 1); /* ROB-05 */
            return PGW_ETIMEDOUT;
        }
        if (x->irqs == seen)
            pgw_relax_ms(1);
        seen = x->irqs;
    }
}

/* The RPC callback of layer 2: one request out, one reply in. */
static pgw_err fsfuse_rpc(const void *tx, size_t tx_len, void *rx, size_t rx_cap,
                          size_t *rx_len, bool hiprio, uint32_t prio,
                          uint32_t timeout_ms, void *user)
{
    fsfuse_xport *x = user;
    /* Nothing to do with `prio`: the handler thread was already raised to the
     * caller's priority by pgw_prio_enter(), and the submission event reads it
     * back off the thread, so QNX-03 is shown in the trace (pgw_trace_prio_now
     *()). */
    (void)prio;
    if (x == NULL || tx == NULL || rx == NULL || rx_len == NULL)
        return PGW_EINVAL;
    if (tx_len > UINT32_MAX || rx_cap > UINT32_MAX)
        return PGW_EINVAL;

    pgw_virtq *vq = pick_queue(x, hiprio);
    if (vq == NULL)
        return PGW_ENODEV;

    /* The label of the request is the opcode it carries, and the tag that pairs
     * a submission with its reply is the FUSE unique id (QNX-09). */
    pgw_fuse_in_hdr hdr;
    enum pgw_op op = PGW_OP_OTHER;
    uint32_t tag = 0;
    if (pgw_fuse_dec_in_hdr(tx, tx_len, &hdr) == PGW_OK) {
        op = fsfuse_op_of(hdr.opcode);
        tag = (uint32_t)hdr.unique;
    }
    pgw_trace_req(op, tag);

    /* The device addresses physical memory: the descriptors carry the physical
     * address of the caller's buffers, not their virtual one (VIO-07). */
    uint64_t tx_pa = 0;
    uint64_t rx_pa = 0;
    pgw_err e = pgw_qnx_phys((void *)(uintptr_t)tx, tx_len, &tx_pa);
    if (e == PGW_OK)
        e = pgw_qnx_phys(rx, rx_cap, &rx_pa);
    if (e != PGW_OK)
        return e;

    pgw_vio_sg sg[2];
    sg[0].addr = (void *)(uintptr_t)tx;
    sg[0].phys = tx_pa;
    sg[0].len = (uint32_t)tx_len;
    sg[0].write = false;
    sg[1].addr = rx;
    sg[1].phys = rx_pa;
    sg[1].len = (uint32_t)rx_cap;
    sg[1].write = true;

    uint32_t unique = pgw_cookie_next(&g_unique_next);
    void *cookie = (void *)(uintptr_t)unique;

    pgw_stat_add(PGW_STAT_REQ, 1);
    e = pgw_vq_push(vq, sg, 2u, cookie);
    if (e != PGW_OK)
        return e;
    (void)pgw_vq_kick(vq);

    uint32_t got = 0;
    uint32_t wait_ms = timeout_ms != 0u ? timeout_ms : x->timeout_ms;
    e = wait_completion(x, vq, cookie, &got, wait_ms);
    pgw_trace_rep(op, tag, e == PGW_OK ? 0 : pgw_qnx_status(e));
    if (e != PGW_OK) {
        pgw_stat_add(PGW_STAT_ERR, 1);
        return e;
    }
    pgw_stat_add(PGW_STAT_RESP, 1);
    *rx_len = got;
    return PGW_OK;
}

pgw_err fsfuse_dispatch_open(fsfuse_dev *dev)
{
    if (dev == NULL || dev->hiprio == NULL || dev->num_requests == 0u)
        return PGW_EINVAL;
    memset(&g_xport, 0, sizeof g_xport);
    g_xport.dev = dev;
    g_xport.timeout_ms = dev->opts.timeout_ms;

    g_session = pgw_fuse_session_open(fsfuse_rpc, &g_xport, dev->opts.msize);
    if (g_session == NULL)
        return PGW_ENOMEM;
    PGW_INFO("fsfuse: FUSE session open, %u request queues", dev->num_requests);
    return PGW_OK;
}

void fsfuse_dispatch_close(void)
{
    if (g_session != NULL) {
        pgw_fuse_session_close(g_session); /* returns every reference (VFS-04) */
        g_session = NULL;
    }
}

pgw_fuse_session *fsfuse_session(void)
{
    return g_session;
}

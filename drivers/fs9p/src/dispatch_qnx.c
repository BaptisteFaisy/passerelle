/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fs9p/src/dispatch_qnx.c - the seam between the QNX resource manager,
 * the 9P session and the virtqueue.
 *
 * Two jobs:
 *   - own the one virtqueue the 9P device exposes and answer the session's RPC
 *     callback by pushing the caller's buffers straight onto it (no copy
 *     between the protocol layer and the transport, VIO-07) and waiting for the
 *     used ring to come back;
 *   - expose the session the resource-manager handlers use, so the io/connect
 *     handlers written once in drivers/common reach 9P without knowing about
 *     virtio at all (QNX-01).
 *
 * The wait is bounded by the mount's timeout, so a silent server produces an
 * error and not a hung thread (ROB-05). No allocation happens here.
 *
 * Covered requirements: QNX-01, QNX-03, QNX-05, ROB-05, VIO-07, VIO-08, P9-05.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"
#include "pgw/fs9p.h"
#include "pgw/fs9p_qnx.h"
#include "pgw/log.h"
#include "pgw/p9.h"
#include "pgw/time.h"
#include "pgw/virtio.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/neutrino.h>

/* The transport state of one mount: one queue, one interrupt counter. */
typedef struct fs9p_xport {
    pgw_virtq *vq;
    volatile unsigned irqs; /* bumped by the interrupt service thread */
    uint32_t timeout_ms;
} fs9p_xport;

static fs9p_xport g_xport;
static pgw_p9_session *g_session;
static uint32_t g_tag_next = 1u;

void fs9p_xport_irq(void *user)
{
    fs9p_xport *x = user;
    if (x != NULL)
        x->irqs++;
}

/* Wait until the used ring has an entry for `cookie`, or the deadline passes.
 * The interrupt service thread only bumps a counter; the ring is drained here,
 * on the thread that owns the request, so no lock is needed. */
static pgw_err wait_completion(fs9p_xport *x, void *cookie, uint32_t *out_len,
                               uint32_t timeout_ms)
{
    pgw_deadline dl = pgw_deadline_from_ms(timeout_ms);
    unsigned seen = x->irqs;
    for (;;) {
        void *got = NULL;
        uint32_t len = 0;
        pgw_err e = pgw_vq_pop(x->vq, &got, &len);
        if (e == PGW_OK && got == cookie) {
            *out_len = len;
            return PGW_OK;
        }
        if (e != PGW_OK && e != PGW_EAGAIN)
            return e;
        if (pgw_deadline_expired(dl)) {
            pgw_stat_add(PGW_STAT_TIMEOUT, 1); /* ROB-05 */
            return PGW_ETIMEDOUT;
        }
        if (x->irqs == seen)
            pgw_relax_ms(1); /* nothing arrived yet: yield the CPU */
        seen = x->irqs;
    }
}

/* The RPC callback of layer 2: one request out, one reply in. */
static pgw_err fs9p_rpc(const void *tx, size_t tx_len, void *rx, size_t rx_cap,
                        size_t *rx_len, uint32_t prio, uint32_t timeout_ms,
                        void *user)
{
    fs9p_xport *x = user;
    (void)prio; /* the caller already runs at its own priority (QNX-03) */
    if (x == NULL || x->vq == NULL || tx == NULL || rx == NULL || rx_len == NULL)
        return PGW_EINVAL;
    if (tx_len > UINT32_MAX || rx_cap > UINT32_MAX)
        return PGW_EINVAL;

    pgw_trace_req(PGW_OP_OPEN, g_tag_next); /* tag pairs the two events (QNX-09) */
    /* Device-readable request, device-writable reply: the caller's buffers are
     * handed over as they are, by their physical address (VIO-07). */
    uint64_t tx_pa = 0;
    uint64_t rx_pa = 0;
    pgw_err e = pgw_qnx_phys((void *)(uintptr_t)tx, tx_len, &tx_pa);
    if (e == PGW_OK)
        e = pgw_qnx_phys(rx, rx_cap, &rx_pa);
    if (e != PGW_OK)
        return e;
    pgw_vio_sg sg[2];
    sg[0].addr = (void *)(uintptr_t)tx; /* the device only reads this buffer */
    sg[0].phys = tx_pa;
    sg[0].len = (uint32_t)tx_len;
    sg[0].write = false;
    sg[1].addr = rx;
    sg[1].phys = rx_pa;
    sg[1].len = (uint32_t)rx_cap;
    sg[1].write = true;

    void *cookie = (void *)(uintptr_t)g_tag_next;
    g_tag_next = g_tag_next == UINT32_MAX ? 1u : g_tag_next + 1u;

    pgw_stat_add(PGW_STAT_REQ, 1);
    e = pgw_vq_push(x->vq, sg, 2u, cookie);
    if (e != PGW_OK)
        return e;
    (void)pgw_vq_kick(x->vq); /* suppressed when the device asked us not to */

    uint32_t got = 0;
    uint32_t wait_ms = timeout_ms != 0u ? timeout_ms : x->timeout_ms;
    e = wait_completion(x, cookie, &got, wait_ms);
    pgw_trace_rep(PGW_OP_OPEN, (uint32_t)(uintptr_t)cookie,
                  e == PGW_OK ? EOK : pgw_qnx_status(e));
    if (e != PGW_OK) {
        pgw_stat_add(PGW_STAT_ERR, 1);
        return e;
    }
    pgw_stat_add(PGW_STAT_RESP, 1);
    *rx_len = got;
    return PGW_OK;
}

pgw_err fs9p_dispatch_open(pgw_vio_dev *dev, pgw_virtq *vq, uint32_t timeout_ms)
{
    if (dev == NULL || vq == NULL)
        return PGW_EINVAL;
    (void)dev;
    g_xport.vq = vq;
    g_xport.irqs = 0;
    g_xport.timeout_ms = timeout_ms;
    /* The device reads and writes the session buffers, so they must be
     * physically contiguous: the session's allocator is the DMA pool (VIO-07). */
    pgw_p9_session_set_memory(pgw_qnx_alloc_dma, pgw_qnx_free_dma, NULL);
    g_session = pgw_p9_session_open(fs9p_rpc, &g_xport, PGW_MSIZE_DEFAULT, "nobody",
                                    "/", 0u);
    if (g_session == NULL)
        return PGW_ENOMEM;
    PGW_INFO("fs9p: 9P session open, msize %u", pgw_p9_session_msize(g_session));
    return PGW_OK;
}

void fs9p_dispatch_close(void)
{
    if (g_session != NULL) {
        pgw_p9_session_close(g_session); /* clunks every fid (P9-07) */
        g_session = NULL;
    }
    g_xport.vq = NULL;
}

pgw_p9_session *fs9p_session(void)
{
    return g_session;
}

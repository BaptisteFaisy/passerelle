/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/src/virtq_dev.c - split virtqueue, device side.
 *
 * This is the half of the virtqueue a *device* runs: consume an available
 * chain, turn it into a readable/writable scatter-gather request, publish the
 * completion in the used ring. A real device does that in QEMU; here the mock
 * transport does it in memory, which is what makes the whole stack testable on
 * the PC (VIO-10, P9-10).
 *
 * Covers:
 *   VIO-07 - the request handed to the device callback points straight at the
 *            caller's buffers, in chain order, readable entries first;
 *   VIO-08 - an indirect chain is expanded from its table, so the device side
 *            sees the same request shape as a direct chain;
 *   ROB-06 - a chain that is malformed (indirect length not a multiple of 16,
 *            too many entries, descriptor index out of range, cyclic NEXT
 *            chain) is refused instead of walked.
 */
#include "virtio_priv.h"

#include "pgw/log.h"

#include <stdatomic.h>

#define PGW_DESC_BYTES 16u /* sizeof(pgw_vring_desc), as the wire sees it */

static void sg_fill(pgw_vio_sg *e, uint64_t addr, uint32_t len, uint16_t flags)
{
    e->addr = (void *)(uintptr_t)addr;
    e->phys = addr;
    e->len = len;
    e->write = (flags & PGW_VRING_DESC_F_WRITE) != 0;
}

static pgw_err sg_add(pgw_vq_req *req, uint64_t addr, uint32_t len,
                      uint16_t flags)
{
    if ((flags & PGW_VRING_DESC_F_WRITE) != 0) {
        if (req->n_out >= (size_t)PGW_SG_MAX)
            return PGW_E2BIG;
        sg_fill(&req->out[req->n_out], addr, len, flags);
        req->n_out++;
    } else {
        if (req->n_in >= (size_t)PGW_SG_MAX)
            return PGW_E2BIG;
        sg_fill(&req->in[req->n_in], addr, len, flags);
        req->n_in++;
    }
    return PGW_OK;
}

static pgw_err resolve_indirect(pgw_virtq *vq, const pgw_vring_desc *head,
                                pgw_vq_req *req)
{
    if (head->len == 0 || (head->len % PGW_DESC_BYTES) != 0) {
        PGW_ERR("virtio: queue %u: indirect length %u is not a multiple of %u",
                (unsigned)vq->qidx, (unsigned)head->len, (unsigned)PGW_DESC_BYTES);
        return PGW_EIO;
    }
    size_t n = (size_t)head->len / (size_t)PGW_DESC_BYTES;
    if (n > (size_t)PGW_SG_MAX) {
        PGW_ERR("virtio: queue %u: indirect table of %zu entries", (unsigned)vq->qidx,
                n);
        return PGW_EIO;
    }
    const pgw_vring_desc *t = (const pgw_vring_desc *)(uintptr_t)head->addr;
    for (size_t i = 0; i < n; i++) {
        pgw_err e = sg_add(req, t[i].addr, t[i].len, t[i].flags);
        if (e != PGW_OK)
            return e;
    }
    return PGW_OK;
}

static pgw_err resolve_direct(pgw_virtq *vq, uint16_t head, pgw_vq_req *req)
{
    uint16_t cur = head;
    for (size_t guard = 0; guard <= (size_t)vq->size; guard++) {
        if (cur >= vq->size) {
            PGW_ERR("virtio: queue %u: chain leaves the table at %u",
                    (unsigned)vq->qidx, (unsigned)cur);
            return PGW_EIO;
        }
        const pgw_vring_desc *d = &vq->desc[cur];
        pgw_err e = sg_add(req, d->addr, d->len, d->flags);
        if (e != PGW_OK)
            return e;
        if ((d->flags & PGW_VRING_DESC_F_NEXT) == 0)
            return PGW_OK;
        cur = d->next;
    }
    PGW_ERR("virtio: queue %u: cyclic descriptor chain", (unsigned)vq->qidx);
    return PGW_EIO;
}

static pgw_err resolve_chain(pgw_virtq *vq, uint16_t head, pgw_vq_req *req)
{
    req->head = head;
    req->n_in = 0;
    req->n_out = 0;
    const pgw_vring_desc *d = &vq->desc[head];
    if ((d->flags & PGW_VRING_DESC_F_INDIRECT) != 0)
        return resolve_indirect(vq, d, req);
    return resolve_direct(vq, head, req);
}

pgw_err pgw_vq_take_avail(pgw_virtq *vq, pgw_vq_req *req, bool *got)
{
    if (vq == NULL || req == NULL || got == NULL)
        return PGW_EINVAL;
    *got = false;

    atomic_thread_fence(memory_order_acquire);
    if (*pgw_vq_avail_idx(vq) == vq->dev_last_avail)
        return PGW_OK;

    uint16_t head =
        pgw_vq_avail_ring(vq)[(size_t)vq->dev_last_avail % (size_t)vq->size];
    vq->dev_last_avail = (uint16_t)(vq->dev_last_avail + 1u);
    if (head >= vq->size) {
        PGW_ERR("virtio: queue %u: available ring names descriptor %u",
                (unsigned)vq->qidx, (unsigned)head);
        return PGW_EIO;
    }

    pgw_err e = resolve_chain(vq, head, req);
    if (e != PGW_OK)
        return e;
    *got = true;
    return PGW_OK;
}

pgw_err pgw_vq_complete(pgw_virtq *vq, uint16_t head, uint32_t written)
{
    if (vq == NULL || head >= vq->size)
        return PGW_EINVAL;

    uint16_t idx = *pgw_vq_used_idx(vq);
    pgw_vring_used_elem *ring = pgw_vq_used_ring(vq);
    ring[(size_t)idx % (size_t)vq->size].id = head;
    ring[(size_t)idx % (size_t)vq->size].len = written;
    atomic_thread_fence(memory_order_release);
    *pgw_vq_used_idx(vq) = (uint16_t)(idx + 1u);
    return PGW_OK;
}

size_t pgw_vq_mock_process(pgw_virtq *vq, pgw_vio_mock_fn fn, void *user)
{
    if (vq == NULL || fn == NULL)
        return 0;

    size_t done = 0;
    for (;;) {
        pgw_vq_req req;
        bool got = false;
        if (pgw_vq_take_avail(vq, &req, &got) != PGW_OK || !got)
            break;

        uint32_t written = fn(req.in, req.n_in, req.out, req.n_out, user);
        uint32_t room = 0;
        for (size_t i = 0; i < req.n_out; i++)
            room += req.out[i].len;
        if (written > room)
            written = room; /* a device cannot write more than it was given */
        if (pgw_vq_complete(vq, req.head, written) != PGW_OK)
            break;
        done++;
    }
    return done;
}

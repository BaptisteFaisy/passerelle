/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/src/virtq.c - split virtqueue, driver side.
 *
 * Implements the driver half of virtio 1.2 §2.6: descriptor table, available
 * ring, used ring, descriptor chains, indirect descriptor tables and the two
 * notification suppression mechanisms.
 *
 * Covers:
 *   VIO-07 - a chain is built straight from the caller's scatter-gather list;
 *            no byte is copied and the caller's cookie travels with the head
 *            descriptor, so the completion path can find its request without a
 *            lookup table;
 *   VIO-08 - indirect descriptors are used when the feature was negotiated and
 *            the request needs more than PGW_INDIRECT_THRESHOLD entries, and
 *            both EVENT_IDX (used_event / avail_event) and
 *            VRING_AVAIL_F_NO_INTERRUPT are honoured;
 *   QNX-04 - a full queue returns PGW_EAGAIN and leaves the ring untouched.
 *
 * Ring memory and queue lifetime live in virtq_alloc.c; this file only builds
 * and consumes chains. Every buffer was allocated once, when the queue was
 * created, so the submission and completion paths never allocate and are safe
 * from an interrupt thread.
 */
#include "virtio_priv.h"

#include "pgw/limits.h"
#include "pgw/log.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* --- descriptor and table pools ------------------------------------------- */

static uint16_t desc_alloc(pgw_virtq *vq)
{
    uint16_t i = vq->free_head;
    if (i == PGW_VQ_NO_DESC || vq->num_free == 0)
        return PGW_VQ_NO_DESC;
    vq->free_head = vq->free_next[i];
    vq->num_free--;
    return i;
}

static void desc_free(pgw_virtq *vq, uint16_t i)
{
    vq->free_next[i] = vq->free_head;
    vq->free_head = i;
    vq->num_free++;
}

static uint16_t table_take(pgw_virtq *vq)
{
    if (vq->tables == NULL)
        return PGW_VQ_NO_DESC;
    for (uint16_t i = 0; i < (uint16_t)PGW_VQ_INDIRECT_TABLES; i++) {
        uint32_t bit = 1u << i;
        if ((vq->table_mask & bit) == 0) {
            vq->table_mask |= bit;
            return i;
        }
    }
    return PGW_VQ_NO_DESC;
}

static void table_put(pgw_virtq *vq, uint16_t table)
{
    vq->table_mask &= ~(1u << table);
}

/* --- chain construction (VIO-07, VIO-08) ---------------------------------- */

/* Descriptors address caller memory directly. A pre-translated DMA address wins
 * when the caller provided one; otherwise the buffer address is used as is,
 * which is what the mock and MMIO transports expect. */
static uint64_t sg_addr(const pgw_vio_sg *sg)
{
    return sg->phys != 0 ? sg->phys : (uint64_t)(uintptr_t)sg->addr;
}

static uint16_t chain_build_direct(pgw_virtq *vq, const pgw_vio_sg *sg,
                                   size_t count)
{
    uint16_t idx[PGW_SG_MAX];
    for (size_t i = 0; i < count; i++) {
        idx[i] = desc_alloc(vq);
        if (idx[i] == PGW_VQ_NO_DESC) {
            for (size_t j = 0; j < i; j++)
                desc_free(vq, idx[j]);
            return PGW_VQ_NO_DESC;
        }
    }
    for (size_t i = 0; i < count; i++) {
        pgw_vring_desc *d = &vq->desc[idx[i]];
        d->addr = sg_addr(&sg[i]);
        d->len = sg[i].len;
        d->flags = sg[i].write ? PGW_VRING_DESC_F_WRITE : 0u;
        d->next = 0;
        if (i + 1u < count) {
            d->flags |= PGW_VRING_DESC_F_NEXT;
            d->next = idx[i + 1u];
        }
    }
    return idx[0];
}

static uint16_t chain_build_indirect(pgw_virtq *vq, uint16_t table,
                                     const pgw_vio_sg *sg, size_t count)
{
    uint16_t head = desc_alloc(vq);
    if (head == PGW_VQ_NO_DESC)
        return PGW_VQ_NO_DESC;

    pgw_vring_desc *t = &vq->tables[(size_t)table * (size_t)PGW_SG_MAX];
    for (size_t i = 0; i < count; i++) {
        t[i].addr = sg_addr(&sg[i]);
        t[i].len = sg[i].len;
        t[i].flags = sg[i].write ? PGW_VRING_DESC_F_WRITE : 0u;
        t[i].next = 0;
    }

    pgw_vring_desc *d = &vq->desc[head];
    d->addr = (uint64_t)(uintptr_t)t;
    d->len = (uint32_t)(count * sizeof(pgw_vring_desc));
    d->flags = PGW_VRING_DESC_F_INDIRECT;
    d->next = 0;
    return head;
}

static void avail_publish(pgw_virtq *vq, uint16_t head)
{
    uint16_t idx = *pgw_vq_avail_idx(vq);
    pgw_vq_avail_ring(vq)[(size_t)idx % (size_t)vq->size] = head;
    atomic_thread_fence(memory_order_release);
    *pgw_vq_avail_idx(vq) = (uint16_t)(idx + 1u);
}

pgw_err pgw_vq_push(pgw_virtq *vq, const pgw_vio_sg *sg, size_t count,
                    void *cookie)
{
    if (vq == NULL || sg == NULL || count == 0)
        return PGW_EINVAL;
    if (count > (size_t)PGW_SG_MAX)
        return PGW_E2BIG;

    bool indirect = vq->indirect && count > (size_t)PGW_INDIRECT_THRESHOLD;
    uint16_t table = PGW_VQ_NO_DESC;
    uint16_t needed = (uint16_t)count;
    if (indirect) {
        table = table_take(vq);
        if (table != PGW_VQ_NO_DESC)
            needed = 1u; /* one descriptor pointing at the table */
        else
            indirect = false; /* no table left: fall back to a direct chain */
    }

    if (vq->num_free < needed) {
        if (table != PGW_VQ_NO_DESC)
            table_put(vq, table);
        pgw_stat_add(PGW_STAT_QUEUE_FULL, 1);
        return PGW_EAGAIN; /* ring untouched: nothing to unwind */
    }

    uint16_t head = indirect ? chain_build_indirect(vq, table, sg, count)
                             : chain_build_direct(vq, sg, count);
    if (head == PGW_VQ_NO_DESC) {
        if (table != PGW_VQ_NO_DESC)
            table_put(vq, table);
        return PGW_EAGAIN;
    }

    vq->cookies[head] = cookie;
    vq->table_owner[head] = table;
    avail_publish(vq, head);
    return PGW_OK;
}

/* --- notification suppression (VIO-08) ------------------------------------ */

/* vring_need_event() of virtio 1.2 §2.6.7: 16-bit wraparound arithmetic. */
static bool need_event(uint16_t event, uint16_t new_idx, uint16_t old)
{
    return (uint16_t)(new_idx - event - 1u) < (uint16_t)(new_idx - old);
}

bool pgw_vq_kick(pgw_virtq *vq)
{
    if (vq == NULL)
        return false;
    uint16_t new_idx = *pgw_vq_avail_idx(vq);
    bool needed;
    if (vq->event_idx) {
        needed = need_event(*pgw_vq_avail_event(vq), new_idx, vq->last_kick);
    } else {
        needed = (*pgw_vq_used_flags(vq) & PGW_VRING_USED_F_NO_NOTIFY) == 0;
    }
    if (!needed) {
        PGW_TRC("virtio: queue %u: notification suppressed", (unsigned)vq->qidx);
        return false;
    }
    vq->last_kick = new_idx;
    if (vq->dev->ops->notify_queue != NULL)
        vq->dev->ops->notify_queue(vq->dev, vq->qidx);
    return true;
}

/* While chains are still waiting in the used ring the driver asks not to be
 * interrupted; once it has drained them it re-arms the interrupt. */
static void intr_policy(pgw_virtq *vq, bool more_pending)
{
    if (vq->event_idx) {
        uint16_t next = (uint16_t)(vq->last_used + (more_pending ? 1u : 0u));
        *pgw_vq_used_event(vq) = next;
    } else if (more_pending) {
        *pgw_vq_avail_flags(vq) |= PGW_VRING_AVAIL_F_NO_INTERRUPT;
    } else {
        *pgw_vq_avail_flags(vq) &=
            (uint16_t)~PGW_VRING_AVAIL_F_NO_INTERRUPT;
    }
}

/* --- completion (VIO-07) -------------------------------------------------- */

static void chain_release(pgw_virtq *vq, uint16_t head)
{
    pgw_vring_desc *d = &vq->desc[head];
    if ((d->flags & PGW_VRING_DESC_F_INDIRECT) != 0) {
        uint16_t table = vq->table_owner[head];
        if (table != PGW_VQ_NO_DESC)
            table_put(vq, table);
        vq->table_owner[head] = PGW_VQ_NO_DESC;
        desc_free(vq, head);
        return;
    }
    uint16_t cur = head;
    for (size_t guard = 0; guard <= (size_t)vq->size; guard++) {
        /* A chain longer than the table means the device handed back a cycle:
         * stop before a descriptor enters the free list twice. */
        if (cur == PGW_VQ_NO_DESC || cur >= vq->size ||
            vq->num_free >= vq->size)
            return;
        uint16_t next = (vq->desc[cur].flags & PGW_VRING_DESC_F_NEXT) != 0
                            ? vq->desc[cur].next
                            : PGW_VQ_NO_DESC;
        desc_free(vq, cur);
        cur = next;
    }
}

pgw_err pgw_vq_pop(pgw_virtq *vq, void **out_cookie, uint32_t *out_len)
{
    if (vq == NULL)
        return PGW_EINVAL;

    atomic_thread_fence(memory_order_acquire);
    if (*pgw_vq_used_idx(vq) == vq->last_used) {
        intr_policy(vq, false); /* ring empty: an interrupt must wake us */
        return PGW_EAGAIN;
    }

    pgw_vring_used_elem *ring = pgw_vq_used_ring(vq);
    pgw_vring_used_elem elem = ring[(size_t)vq->last_used % (size_t)vq->size];
    if (elem.id >= (uint32_t)vq->size) {
        PGW_ERR("virtio: queue %u: used ring returns descriptor %u",
                (unsigned)vq->qidx, (unsigned)elem.id);
        return PGW_EIO;
    }

    uint16_t head = (uint16_t)elem.id;
    vq->last_used = (uint16_t)(vq->last_used + 1u);
    void *cookie = vq->cookies[head];
    vq->cookies[head] = NULL;
    chain_release(vq, head);

    if (out_cookie != NULL)
        *out_cookie = cookie;
    if (out_len != NULL)
        *out_len = elem.len;

    intr_policy(vq, *pgw_vq_used_idx(vq) != vq->last_used);
    return PGW_OK;
}

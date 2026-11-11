/* SPDX-License-Identifier: Apache-2.0 */
/*
 * clab_vring.c - chapter 5's exercise, implementation.
 *
 * Read this together with clab_vring.h. Three subtleties the exercise is built
 * around:
 *
 *  1. the free list lives *inside* the descriptor table: a free descriptor's
 *     `next` field points at the next free one, which is why a descriptor can
 *     be recycled with one assignment;
 *  2. publishing is two steps, never one: write the ring entry, then bump
 *     avail.idx. The device trusts idx, so anything written after that bump may
 *     not be seen;
 *  3. completion is decided from used.idx, never from a wakeup: a spurious
 *     interrupt must cost nothing but one loop.
 */
#include "clab_vring.h"

#include <stdio.h>
#include <string.h>

#define NO_DESC 0xFFFFu

void clab_vring_init(clab_vring *vq, uint8_t *arena, uint32_t arena_len,
                     bool indirect_enabled)
{
    memset(vq, 0, sizeof *vq);
    vq->arena = arena;
    vq->arena_len = arena_len;
    vq->indirect_enabled = indirect_enabled;
    for (uint16_t i = 0; i < CLAB_VRING_SIZE; i++) {
        vq->desc[i].next = (uint16_t)(i + 1u);
        vq->desc[i].flags = 0;
        vq->desc[i].len = 0;
        vq->desc[i].addr = 0;
    }
    vq->desc[CLAB_VRING_SIZE - 1u].next = NO_DESC;
    vq->free_head = 0;
    vq->num_free = CLAB_VRING_SIZE;
    vq->pending_head = NO_DESC;
}

static uint16_t take_desc(clab_vring *vq)
{
    uint16_t idx = vq->free_head;

    if (idx == NO_DESC)
        return NO_DESC;
    vq->free_head = vq->desc[idx].next;
    vq->desc[idx].next = NO_DESC;
    vq->num_free--;
    return idx;
}

/* Fill one descriptor: `has_next` sets NEXT, `writable` sets WRITE. */
static void fill(clab_vring_desc *d, const clab_vring_sg *sg, bool has_next)
{
    d->addr = sg->off;
    d->len = sg->len;
    d->flags = (uint16_t)((sg->writable ? CLAB_VRING_DESC_F_WRITE : 0u) |
                          (has_next ? CLAB_VRING_DESC_F_NEXT : 0u));
    if (!has_next)
        d->next = 0;
}

/* Chain `count` buffers through `count` free descriptors. */
static uint16_t add_chain(clab_vring *vq, const clab_vring_sg *sg, size_t count)
{
    uint16_t head = NO_DESC;
    uint16_t prev = NO_DESC;

    for (size_t i = 0; i < count; i++) {
        uint16_t idx = take_desc(vq);
        if (idx == NO_DESC)
            return NO_DESC;
        fill(&vq->desc[idx], &sg[i], i + 1u < count);
        if (prev == NO_DESC)
            head = idx;
        else
            vq->desc[prev].next = idx;
        prev = idx;
    }
    return head;
}

/* The indirect form: one descriptor in the table, pointing at a table of
 * descriptors outside it. One descriptor instead of `count`, but the device
 * must have been told it may see them (VIO-08). */
static uint16_t add_indirect(clab_vring *vq, const clab_vring_sg *sg, size_t count)
{
    uint16_t idx = take_desc(vq);

    if (idx == NO_DESC)
        return NO_DESC;
    for (size_t i = 0; i < count; i++)
        fill(&vq->indirect[i], &sg[i], i + 1u < count);
    /* Here the table is a known array rather than a DMA address, so addr stays
     * 0; a real driver puts the table's physical address there. */
    vq->desc[idx].addr = 0;
    vq->desc[idx].len = (uint32_t)(count * sizeof(clab_vring_desc));
    vq->desc[idx].flags = CLAB_VRING_DESC_F_INDIRECT;
    vq->desc[idx].next = 0;
    return idx;
}

int clab_vring_add(clab_vring *vq, const clab_vring_sg *sg, size_t count)
{
    uint16_t head;

    if (count == 0 || count > CLAB_VRING_SG_MAX || vq->pending_head != NO_DESC)
        return -1;
    if (vq->indirect_enabled && count > 1u && vq->num_free >= 1u)
        head = add_indirect(vq, sg, count);
    else
        head = add_chain(vq, sg, count);
    if (head == NO_DESC)
        return -1;
    vq->pending_head = head;
    return (int)head;
}

void clab_vring_publish(clab_vring *vq)
{
    if (vq->pending_head == NO_DESC)
        return;
    /* Step 1: the ring entry. Step 2: the index. Never the other way round. */
    vq->avail.ring[vq->avail.idx % CLAB_VRING_SIZE] = vq->pending_head;
    vq->avail.idx = (uint16_t)(vq->avail.idx + 1u);
    vq->pending_head = NO_DESC;
}

int clab_vring_pop(clab_vring *vq, uint16_t *head, uint32_t *len)
{
    if (vq->last_used == vq->used.idx)
        return 0; /* nothing the device has finished */
    const clab_vring_used_elem *e = &vq->used.ring[vq->last_used % CLAB_VRING_SIZE];
    *head = (uint16_t)e->id;
    *len = e->len;
    vq->last_used = (uint16_t)(vq->last_used + 1u);
    return 1;
}

void clab_vring_recycle(clab_vring *vq, uint16_t head)
{
    uint16_t idx = head;

    for (;;) {
        const bool has_next = (vq->desc[idx].flags & CLAB_VRING_DESC_F_NEXT) != 0u;
        const uint16_t next = vq->desc[idx].next;
        vq->desc[idx].flags = 0;
        vq->desc[idx].len = 0;
        vq->desc[idx].addr = 0;
        vq->desc[idx].next = vq->free_head;
        vq->free_head = idx;
        vq->num_free++;
        if (!has_next)
            break;
        idx = next;
    }
}

/* Copy the chain's *readable* buffers into `out`; returns the bytes copied.
 * The device stops reading at the first device-writable buffer: everything
 * before it is the request, everything from it on is where the reply goes. */
static uint32_t collect(clab_vring *vq, uint16_t head, uint8_t *out, uint32_t cap)
{
    uint16_t idx = head;
    uint32_t used = 0;

    for (;;) {
        const clab_vring_desc *d = &vq->desc[idx];
        if ((d->flags & CLAB_VRING_DESC_F_WRITE) != 0u)
            break; /* the reply buffer: not part of the request */
        const uint32_t room = cap - used;
        const uint32_t n = d->len < room ? d->len : room;
        if (n > 0u)
            memcpy(out + used, vq->arena + d->addr, n);
        used += n;
        if ((d->flags & CLAB_VRING_DESC_F_NEXT) == 0u)
            break;
        idx = d->next;
    }
    return used;
}

/* Write `len` bytes into the chain's first device-writable buffer. */
static uint32_t scatter_reply(clab_vring *vq, uint16_t head, const uint8_t *data,
                              uint32_t len)
{
    uint16_t idx = head;

    for (;;) {
        clab_vring_desc *d = &vq->desc[idx];
        if ((d->flags & CLAB_VRING_DESC_F_WRITE) != 0u) {
            const uint32_t n = d->len < len ? d->len : len;
            memcpy(vq->arena + d->addr, data, n);
            return n;
        }
        if ((d->flags & CLAB_VRING_DESC_F_NEXT) == 0u)
            break;
        idx = d->next;
    }
    return 0;
}

void clab_vring_complete(clab_vring *vq, uint16_t head, uint32_t written)
{
    /* The device writes the entry first, then the index: the driver trusts the
     * index, so an entry written after it may never be looked at. */
    clab_vring_used_elem *e = &vq->used.ring[vq->used.idx % CLAB_VRING_SIZE];
    e->id = head;
    e->len = written;
    vq->used.idx = (uint16_t)(vq->used.idx + 1u);
}

uint32_t clab_vring_fake_device(clab_vring *vq, uint16_t head)
{
    uint8_t request[CLAB_VRING_BUF_MAX];
    uint8_t reply[CLAB_VRING_BUF_MAX];
    const uint32_t req_len = collect(vq, head, request, sizeof request);
    uint32_t wrote;

    for (uint32_t i = 0; i < req_len; i++)
        reply[i] = (request[i] >= 'a' && request[i] <= 'z')
                       ? (uint8_t)(request[i] - 32u)
                       : request[i];
    wrote = scatter_reply(vq, head, reply, req_len);
    clab_vring_complete(vq, head, wrote);
    return wrote;
}

void clab_vring_dump(const clab_vring *vq)
{
    printf("  desc  addr   len  flags  next\n");
    for (uint16_t i = 0; i < CLAB_VRING_SIZE; i++) {
        printf("  [%u]   %4llu   %3u   0x%02x   %u%s\n", i,
               (unsigned long long)vq->desc[i].addr, vq->desc[i].len,
               vq->desc[i].flags, vq->desc[i].next,
               (vq->desc[i].flags & CLAB_VRING_DESC_F_WRITE) ? "  (w)" : "");
    }
    printf("  avail: idx=%u  ring[%u]=%u\n", vq->avail.idx,
           (unsigned)((vq->avail.idx + CLAB_VRING_SIZE - 1u) % CLAB_VRING_SIZE),
           vq->avail.ring[(vq->avail.idx + CLAB_VRING_SIZE - 1u) % CLAB_VRING_SIZE]);
    printf("  used:  idx=%u  last_used=%u  free=%u\n", vq->used.idx, vq->last_used,
           vq->num_free);
}

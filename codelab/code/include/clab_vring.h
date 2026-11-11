/* SPDX-License-Identifier: Apache-2.0 */
/*
 * clab_vring.h - chapter 5's exercise: a split virtqueue, in plain RAM.
 *
 * The virtqueue is the whole of virtio's data path (virtio 1.2, 2.7). It is
 * three structures in shared memory:
 *
 *   descriptor table   16 bytes per entry: addr, len, flags, next
 *   available ring     what the driver offers the device: flags, idx, ring[]
 *   used ring          what the device has finished: flags, idx, ring[]
 *
 * The driver writes descriptors, puts the head index in the available ring,
 * bumps available.idx and notifies the device. The device reads the chain,
 * writes its reply into the device-writable buffers, puts the head index and
 * the number of bytes written into the used ring, bumps used.idx and raises an
 * interrupt. The driver reads used.idx, takes the entry, and recycles the
 * descriptors.
 *
 * In this exercise the "device" is a function you call yourself, and the rings
 * live in ordinary memory, so the whole dance can be printed and inspected.
 * The real queue (virtio/src/virtq.c) is the same logic with device memory and
 * an interrupt thread behind it.
 */
#ifndef CLAB_VRING_H
#define CLAB_VRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLAB_VRING_DESC_F_NEXT     0x0001u
#define CLAB_VRING_DESC_F_WRITE    0x0002u
#define CLAB_VRING_DESC_F_INDIRECT 0x0004u

#define CLAB_VRING_SIZE 8u /* descriptors; must be a power of two */
#define CLAB_VRING_SG_MAX 4u
#define CLAB_VRING_BUF_MAX 64u

typedef struct {
    uint64_t addr; /* here: an offset into the arena, not a physical address */
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} clab_vring_desc;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[CLAB_VRING_SIZE];
    uint16_t used_event;
} clab_vring_avail;

typedef struct {
    uint32_t id;
    uint32_t len;
} clab_vring_used_elem;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    clab_vring_used_elem ring[CLAB_VRING_SIZE];
    uint16_t avail_event;
} clab_vring_used;

/* A buffer the driver offers: a slice of the arena, and its direction. */
typedef struct {
    uint32_t off;   /* offset into the arena */
    uint32_t len;
    bool writable;  /* true: the device writes into it (reply) */
} clab_vring_sg;

typedef struct {
    clab_vring_desc desc[CLAB_VRING_SIZE];
    clab_vring_avail avail;
    clab_vring_used used;
    /* One indirect table, used when a chain is longer than the threshold. */
    clab_vring_desc indirect[CLAB_VRING_SG_MAX];
    bool indirect_enabled;
    uint16_t free_head;   /* first free descriptor, 0xFFFF when none */
    uint16_t num_free;
    uint16_t last_used;   /* how many used entries we have consumed */
    uint16_t pending_head; /* a chain built by add(), not published yet */
    uint8_t *arena;       /* the memory all the sg offsets point into */
    uint32_t arena_len;
} clab_vring;

/* Set up the ring over `arena`, chaining the descriptors into a free list. */
void clab_vring_init(clab_vring *vq, uint8_t *arena, uint32_t arena_len,
                     bool indirect_enabled);

/* Offer a chain of `count` buffers. Returns the head descriptor index, or
 * -1 when the ring is full. */
int clab_vring_add(clab_vring *vq, const clab_vring_sg *sg, size_t count);

/* What the driver does after add(): make the entry visible to the device. */
void clab_vring_publish(clab_vring *vq);

/* Pop one completed chain. Returns 1 and fills head/len, or 0 when the used
 * ring is empty. `head` is the index the device reported. */
int clab_vring_pop(clab_vring *vq, uint16_t *head, uint32_t *len);

/* Give a completed chain's descriptors back to the free list. */
void clab_vring_recycle(clab_vring *vq, uint16_t head);

/* What the device does when it is done with a chain: record the completion in
 * the used ring and bump used.idx. Called after it has written its reply into
 * the device-writable buffers. */
void clab_vring_complete(clab_vring *vq, uint16_t head, uint32_t written);

/* A toy device for the first exercise: upper-cases the request into the reply
 * buffer, then completes. Returns the number of bytes it wrote. */
uint32_t clab_vring_fake_device(clab_vring *vq, uint16_t head);

/* Print the ring state, for the exercise's output. */
void clab_vring_dump(const clab_vring *vq);

#endif /* CLAB_VRING_H */

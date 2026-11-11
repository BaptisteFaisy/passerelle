/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ex04_vring.c - chapter 5's exercise: put a two-descriptor request on a split
 * virtqueue, run a toy device on it, and take the reply off the used ring.
 *
 * The rings live in ordinary memory and the "device" is a function call, so
 * every intermediate state can be printed. That is the point: on a real device
 * the same sequence happens in device memory with a hypervisor on the other
 * side, and when it goes wrong you cannot printf the middle of it.
 *
 * Run it once with indirect descriptors off and once with them on: the
 * descriptor table looks different, the used ring looks the same. That is what
 * "the chain is the unit, not the descriptor" means.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "clab_check.h"
#include "clab_vring.h"

/* The arena: a request buffer at offset 0 and a reply buffer after it. */
#define REQ_OFF 0u
#define RPL_OFF 32u
#define ARENA_LEN 96u

/* Offer one request, and print the ring before and after. */
static void offer(clab_vring *vq, uint8_t *arena, int *out_added)
{
    clab_vring_sg sg[2];
    const char request[] = "Tversion msize=8192"; /* not a real 9P message */

    memset(arena, 0, ARENA_LEN);
    memcpy(arena + REQ_OFF, request, sizeof request - 1u);

    /* A request is two buffers: the device reads the first, writes the second.
     * The WRITE flag is what tells it which is which. */
    sg[0].off = REQ_OFF;
    sg[0].len = (uint32_t)(sizeof request - 1u);
    sg[0].writable = false;
    sg[1].off = RPL_OFF;
    sg[1].len = 32u;
    sg[1].writable = true;

    const int added = clab_vring_add(vq, sg, 2);
    CLAB_CHECK(added >= 0);
    *out_added = added;
    printf("after add(), head = %d, %u descriptors left:\n", added, vq->num_free);
    clab_vring_dump(vq);

    clab_vring_publish(vq);
    printf("after publish(), avail.idx = %u (this is what the device sees):\n",
           vq->avail.idx);
}

/* Run the toy device, take the completion, and recycle the chain. */
static void complete(clab_vring *vq, uint8_t *arena, int added)
{
    uint16_t head = 0;
    uint32_t len = 0;
    const uint32_t wrote = clab_vring_fake_device(vq, (uint16_t)added);

    printf("device wrote %u bytes, used.idx = %u\n", wrote, vq->used.idx);
    clab_vring_dump(vq);

    /* The driver takes the completion and checks it is the chain it sent. */
    CLAB_CHECK_EQ(clab_vring_pop(vq, &head, &len), 1);
    CLAB_CHECK_EQ(head, (uint16_t)added);
    CLAB_CHECK_EQ(len, wrote);
    printf("reply: \"%.*s\"\n", (int)len, (const char *)(arena + RPL_OFF));
    CLAB_CHECK_MEM(arena + RPL_OFF, "TVERSION MSIZE=8192", wrote);

    /* Recycling is what makes the ring reusable; forgetting it leaks a
     * descriptor per request, and the ring fills up after SIZE/2 requests. */
    clab_vring_recycle(vq, head);
    printf("after recycle(), free = %u\n", vq->num_free);
    CLAB_CHECK_EQ(vq->num_free, CLAB_VRING_SIZE);
    CLAB_CHECK_EQ(clab_vring_pop(vq, &head, &len), 0);
}

static void run(int indirect)
{
    uint8_t arena[ARENA_LEN];
    clab_vring vq;
    int added = 0;

    clab_vring_init(&vq, arena, ARENA_LEN, indirect != 0);

    printf("\n%s\n", indirect ? "indirect descriptors enabled" : "direct descriptors");
    printf("empty ring:\n");
    clab_vring_dump(&vq);
    CLAB_CHECK_EQ(vq.num_free, CLAB_VRING_SIZE);

    offer(&vq, arena, &added);
    complete(&vq, arena, added);
}

int main(void)
{
    printf("chapter 5 - the split virtqueue\n");
    run(0);
    run(1);
    return clab_result();
}

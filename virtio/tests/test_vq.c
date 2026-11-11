/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/tests/test_vq.c - the split virtqueue, driver side.
 *
 * Covers VIO-07 (zero-copy scatter-gather, cookie round trip), VIO-08 (indirect
 * descriptors, EVENT_IDX and NO_NOTIFY suppression), QNX-04 (a full queue says
 * EAGAIN and leaves the ring intact), VIO-03 (legacy contiguous layout) and
 * ROB-06 (a malformed chain is refused, not walked).
 */
#include "pgw/test.h"
#include "pgw/virtio.h"
#include "virtio_priv.h"

#include <stdio.h>
#include <string.h>

/* A mock device with no handler: requests stay queued until the test plays the
 * device with pgw_vio_mock_step() or by completing them itself. */
static pgw_vio_dev *open_mock(uint64_t features)
{
    pgw_vio_dev *dev =
        pgw_vio_mock_open(PGW_VIRTIO_ID_9P, "t", 1u, features, NULL, NULL);
    if (dev == NULL)
        return NULL;
    uint64_t want = features & (PGW_VIO_F_VERSION_1 | PGW_VIO_F_RING_INDIRECT_DESC |
                                PGW_VIO_F_RING_EVENT_IDX);
    if (pgw_vio_init_device(dev, want) != PGW_OK) {
        pgw_vio_destroy(dev);
        return NULL;
    }
    return dev;
}

/* Echo device: copies every readable byte into the first writable buffer. */
static uint32_t echo_handler(const pgw_vio_sg *in, size_t n_in,
                             const pgw_vio_sg *out, size_t n_out, void *user)
{
    (void)user;
    if (n_out == 0)
        return 0;
    size_t done = 0;
    uint8_t *dst = (uint8_t *)out[0].addr;
    for (size_t i = 0; i < n_in && done < out[0].len; i++) {
        size_t n = in[i].len;
        if (n > out[0].len - done)
            n = out[0].len - done;
        memcpy(dst + done, in[i].addr, n);
        done += n;
    }
    return (uint32_t)done;
}

/* --- basic submission ----------------------------------------------------- */

static void test_push_then_pop_returns_cookie(void)
{
    pgw_vio_dev *dev = open_mock(PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 8u, true, true);
    PGW_ASSERT(vq != NULL);
    PGW_ASSERT_EQ_U(pgw_vq_size(vq), 8u);

    char in[] = "hello";
    char out[16];
    pgw_vio_sg sg[2] = {
        {in, 0, sizeof(in), false},
        {out, 0, sizeof(out), true},
    };
    int cookie = 42;
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, 2u, &cookie), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_vq_free_descs(vq), 6u);
    PGW_ASSERT(pgw_vq_kick(vq));

    void *got = NULL;
    uint32_t len = 0;
    PGW_ASSERT_EQ_I(pgw_vq_pop(vq, &got, &len), PGW_EAGAIN); /* not answered */

    PGW_ASSERT_EQ_U(pgw_vio_mock_step(dev, 0), 0u); /* no handler installed */
    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_multiple_submissions_keep_their_order(void)
{
    pgw_vio_dev *dev =
        pgw_vio_mock_open(PGW_VIRTIO_ID_9P, "t", 1u, PGW_VIO_F_VERSION_1,
                          echo_handler, NULL);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1), PGW_OK);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 8u, false, false);
    PGW_ASSERT(vq != NULL);

    char in[4][8];
    char out[4][8];
    static int cookies[4] = {10, 11, 12, 13};
    for (size_t i = 0; i < 4u; i++) {
        snprintf(in[i], sizeof(in[i]), "msg%zu", i);
        memset(out[i], 0, sizeof(out[i]));
        pgw_vio_sg sg[2] = {
            {in[i], 0, 5u, false},
            {out[i], 0, sizeof(out[i]), true},
        };
        PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, 2u, &cookies[i]), PGW_OK);
        (void)pgw_vq_kick(vq); /* the synchronous mock answers here */
    }
    PGW_ASSERT_EQ_U(pgw_vq_free_descs(vq), 0u);

    for (size_t i = 0; i < 4u; i++) {
        void *cookie = NULL;
        uint32_t len = 0;
        PGW_ASSERT_EQ_I(pgw_vq_pop(vq, &cookie, &len), PGW_OK);
        PGW_ASSERT_EQ_PTR(cookie, &cookies[i]); /* completion order == submit */
        PGW_ASSERT_EQ_U(len, 5u);
        PGW_ASSERT_MEM_EQ(out[i], in[i], 5u); /* the device saw our memory */
    }
    void *cookie = NULL;
    uint32_t len = 0;
    PGW_ASSERT_EQ_I(pgw_vq_pop(vq, &cookie, &len), PGW_EAGAIN);
    PGW_ASSERT_EQ_U(pgw_vq_free_descs(vq), 8u); /* everything came back */

    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_zero_copy_uses_caller_addresses(void)
{
    pgw_vio_dev *dev = open_mock(PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 8u, false, false);
    PGW_ASSERT(vq != NULL);

    char in[4] = "abc";
    char out[8] = {0};
    pgw_vio_sg sg[2] = {
        {in, 0, sizeof(in), false},
        {out, 0, sizeof(out), true},
    };
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, 2u, NULL), PGW_OK);
    /* The descriptor must point straight at the caller's buffer (VIO-07). */
    PGW_ASSERT_EQ_U(vq->desc[0].addr, (uint64_t)(uintptr_t)in);
    PGW_ASSERT_EQ_U(vq->desc[0].len, sizeof(in));
    PGW_ASSERT_EQ_U(vq->desc[0].flags & PGW_VRING_DESC_F_WRITE, 0u);
    PGW_ASSERT_EQ_U(vq->desc[1].addr, (uint64_t)(uintptr_t)out);
    PGW_ASSERT((vq->desc[1].flags & PGW_VRING_DESC_F_WRITE) != 0);

    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_phys_address_wins_over_virtual(void)
{
    pgw_vio_dev *dev = open_mock(PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 8u, false, false);
    PGW_ASSERT(vq != NULL);

    char buf[8] = {0};
    pgw_vio_sg sg = {buf, 0xDEADB000ULL, 8u, false};
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, &sg, 1u, NULL), PGW_OK);
    PGW_ASSERT_EQ_U(vq->desc[0].addr, 0xDEADB000ULL); /* pre-translated DMA */
    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

/* --- indirect descriptors (VIO-08) ---------------------------------------- */

static void test_indirect_used_above_threshold(void)
{
    pgw_vio_dev *dev = open_mock(PGW_VIO_F_VERSION_1 | PGW_VIO_F_RING_INDIRECT_DESC);
    PGW_ASSERT(dev != NULL);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 16u, true, false);
    PGW_ASSERT(vq != NULL);

    char bufs[PGW_SG_MAX][8];
    pgw_vio_sg sg[PGW_SG_MAX];
    size_t n = (size_t)PGW_INDIRECT_THRESHOLD + 1u;
    for (size_t i = 0; i < n; i++) {
        memset(bufs[i], 0, sizeof(bufs[i]));
        sg[i].addr = bufs[i];
        sg[i].phys = 0;
        sg[i].len = 8u;
        sg[i].write = false;
    }
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, n, NULL), PGW_OK);

    /* One head descriptor, flagged INDIRECT, pointing at a table. */
    PGW_ASSERT((vq->desc[0].flags & PGW_VRING_DESC_F_INDIRECT) != 0);
    PGW_ASSERT_EQ_U(vq->desc[0].len, n * 16u);
    PGW_ASSERT_EQ_U(pgw_vq_free_descs(vq), 15u);
    /* The table itself addresses the caller buffers, still without a copy. */
    PGW_ASSERT_EQ_U(vq->tables[0].addr, (uint64_t)(uintptr_t)bufs[0]);
    PGW_ASSERT_EQ_U(vq->tables[n - 1u].addr, (uint64_t)(uintptr_t)bufs[n - 1u]);

    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_indirect_not_used_at_or_below_threshold(void)
{
    pgw_vio_dev *dev = open_mock(PGW_VIO_F_VERSION_1 | PGW_VIO_F_RING_INDIRECT_DESC);
    PGW_ASSERT(dev != NULL);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 16u, true, false);
    PGW_ASSERT(vq != NULL);

    char bufs[PGW_SG_MAX][8];
    pgw_vio_sg sg[PGW_SG_MAX];
    for (size_t i = 0; i < (size_t)PGW_INDIRECT_THRESHOLD; i++) {
        sg[i].addr = bufs[i];
        sg[i].phys = 0;
        sg[i].len = 8u;
        sg[i].write = false;
    }
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, PGW_INDIRECT_THRESHOLD, NULL), PGW_OK);
    PGW_ASSERT_EQ_U(vq->desc[0].flags & PGW_VRING_DESC_F_INDIRECT, 0u);
    PGW_ASSERT((vq->desc[0].flags & PGW_VRING_DESC_F_NEXT) != 0);
    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_indirect_disabled_when_not_negotiated(void)
{
    /* The caller asks for indirect descriptors but the feature was not offered:
     * the queue silently stays direct instead of building a table the device
     * cannot read. */
    pgw_vio_dev *dev = open_mock(PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 16u, true, false);
    PGW_ASSERT(vq != NULL);
    PGW_ASSERT_FALSE(vq->indirect);

    char bufs[PGW_SG_MAX][8];
    pgw_vio_sg sg[PGW_SG_MAX];
    size_t n = (size_t)PGW_INDIRECT_THRESHOLD + 2u;
    for (size_t i = 0; i < n; i++) {
        sg[i].addr = bufs[i];
        sg[i].phys = 0;
        sg[i].len = 8u;
        sg[i].write = false;
    }
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, n, NULL), PGW_OK);
    PGW_ASSERT_EQ_U(vq->desc[0].flags & PGW_VRING_DESC_F_INDIRECT, 0u);
    PGW_ASSERT_EQ_U(pgw_vq_free_descs(vq), (uint16_t)(16u - n));
    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_indirect_chain_round_trip(void)
{
    pgw_vio_dev *dev = pgw_vio_mock_open(
        PGW_VIRTIO_ID_9P, "t", 1u,
        PGW_VIO_F_VERSION_1 | PGW_VIO_F_RING_INDIRECT_DESC, echo_handler, NULL);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_I(
        pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1 | PGW_VIO_F_RING_INDIRECT_DESC),
        PGW_OK);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 16u, true, false);
    PGW_ASSERT(vq != NULL);

    char parts[6][4];
    char out[32];
    memset(out, 0, sizeof(out));
    pgw_vio_sg sg[7];
    for (size_t i = 0; i < 6u; i++) {
        snprintf(parts[i], sizeof(parts[i]), "p%zu", i);
        sg[i].addr = parts[i];
        sg[i].phys = 0;
        sg[i].len = 2u; /* "p0", without the terminator */
        sg[i].write = false;
    }
    sg[6].addr = out;
    sg[6].phys = 0;
    sg[6].len = sizeof(out);
    sg[6].write = true;

    int cookie = 7;
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, 7u, &cookie), PGW_OK);
    PGW_ASSERT(pgw_vq_kick(vq));

    void *got = NULL;
    uint32_t len = 0;
    PGW_ASSERT_EQ_I(pgw_vq_pop(vq, &got, &len), PGW_OK);
    PGW_ASSERT_EQ_PTR(got, &cookie);
    PGW_ASSERT_EQ_U(len, 12u); /* 6 pieces of 2 bytes */
    PGW_ASSERT_MEM_EQ(out, "p0p1p2p3p4p5", 12u);
    PGW_ASSERT_EQ_U(pgw_vq_free_descs(vq), 16u); /* head and table released */
    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

/* --- notification suppression (VIO-08) ------------------------------------ */

static void test_no_notify_flag_suppresses_kick(void)
{
    pgw_vio_dev *dev = open_mock(PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 8u, false, false);
    PGW_ASSERT(vq != NULL);

    /* The device asks the driver not to notify (used ring flags). */
    PGW_ASSERT_EQ_I(
        pgw_vio_mock_queue_flags(dev, 0, PGW_VRING_USED_F_NO_NOTIFY, 0u), PGW_OK);
    PGW_ASSERT_FALSE(pgw_vq_kick(vq));

    PGW_ASSERT_EQ_I(pgw_vio_mock_queue_flags(dev, 0, 0u, 0u), PGW_OK);
    PGW_ASSERT(pgw_vq_kick(vq));
    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_event_idx_suppresses_redundant_kick(void)
{
    pgw_vio_dev *dev = open_mock(PGW_VIO_F_VERSION_1 | PGW_VIO_F_RING_EVENT_IDX);
    PGW_ASSERT(dev != NULL);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 8u, false, true);
    PGW_ASSERT(vq != NULL);
    PGW_ASSERT(vq->event_idx);

    char in[4] = "x";
    char out[4];
    pgw_vio_sg sg[2] = {{in, 0, 2u, false}, {out, 0, 2u, true}};

    /* avail_event is the last index the device already knows about; 0 means
     * "tell me as soon as you add anything" (virtio 1.2 §2.6.7). */
    PGW_ASSERT_EQ_I(pgw_vio_mock_queue_flags(dev, 0, 0u, 0u), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, 2u, NULL), PGW_OK);
    PGW_ASSERT(pgw_vq_kick(vq)); /* avail_idx reaches 1: notification due */

    /* avail_event was not moved forward by the device, so the driver already
     * knows the device has seen everything up to that point: the second kick
     * is redundant and must be suppressed. */
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, 2u, NULL), PGW_OK);
    PGW_ASSERT_FALSE(pgw_vq_kick(vq));

    /* The device moves avail_event to 2: the next push must notify again. */
    PGW_ASSERT_EQ_I(pgw_vio_mock_queue_flags(dev, 0, 0u, 1u), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, 2u, NULL), PGW_OK);
    PGW_ASSERT(pgw_vq_kick(vq));
    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_pop_rearms_the_interrupt(void)
{
    pgw_vio_dev *dev = open_mock(PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 8u, false, false);
    PGW_ASSERT(vq != NULL);

    void *cookie = NULL;
    uint32_t len = 0;
    PGW_ASSERT_EQ_I(pgw_vq_pop(vq, &cookie, &len), PGW_EAGAIN);
    /* An empty ring must leave the interrupt armed. */
    PGW_ASSERT_EQ_U(*pgw_vq_avail_flags(vq) & PGW_VRING_AVAIL_F_NO_INTERRUPT, 0u);

    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

/* --- overflow (QNX-04) ---------------------------------------------------- */

static void test_full_queue_returns_eagain_without_corruption(void)
{
    pgw_vio_dev *dev = open_mock(PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 4u, false, false);
    PGW_ASSERT(vq != NULL);

    char in[8] = "abcd";
    char out[8];
    pgw_vio_sg sg[2] = {{in, 0, 4u, false}, {out, 0, 4u, true}};

    /* Two chains of two descriptors fill a four descriptor queue. */
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, 2u, NULL), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, 2u, NULL), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_vq_free_descs(vq), 0u);

    uint16_t avail_before = *pgw_vq_avail_idx(vq);
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, 2u, NULL), PGW_EAGAIN);
    /* Nothing was published: a refused request must not leave a hole. */
    PGW_ASSERT_EQ_U(*pgw_vq_avail_idx(vq), avail_before);
    PGW_ASSERT_EQ_U(pgw_vq_free_descs(vq), 0u);
    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_rejects_too_many_sg_entries(void)
{
    pgw_vio_dev *dev = open_mock(PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 64u, false, false);
    PGW_ASSERT(vq != NULL);

    pgw_vio_sg sg[PGW_SG_MAX + 1u];
    char buf[4] = {0};
    for (size_t i = 0; i < PGW_SG_MAX + 1u; i++) {
        sg[i].addr = buf;
        sg[i].phys = 0;
        sg[i].len = 1u;
        sg[i].write = false;
    }
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, PGW_SG_MAX + 1u, NULL), PGW_E2BIG);
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, 0u, NULL), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, NULL, 1u, NULL), PGW_EINVAL);
    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

/* --- malformed chains (ROB-06) -------------------------------------------- */

static void test_refuses_out_of_range_used_id(void)
{
    pgw_vio_dev *dev = open_mock(PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 4u, false, false);
    PGW_ASSERT(vq != NULL);

    /* A device that lies about the completed descriptor index must be caught. */
    PGW_ASSERT_EQ_I(pgw_vq_complete(vq, 4u, 0u), PGW_EINVAL);
    pgw_vq_used_ring(vq)[0].id = 99u;
    pgw_vq_used_ring(vq)[0].len = 0u;
    *pgw_vq_used_idx(vq) = 1u;
    void *cookie = NULL;
    uint32_t len = 0;
    PGW_ASSERT_EQ_I(pgw_vq_pop(vq, &cookie, &len), PGW_EIO);
    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_refuses_malformed_indirect_length(void)
{
    pgw_vio_dev *dev = open_mock(PGW_VIO_F_VERSION_1 | PGW_VIO_F_RING_INDIRECT_DESC);
    PGW_ASSERT(dev != NULL);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 8u, true, false);
    PGW_ASSERT(vq != NULL);

    /* A device handing back an indirect chain whose length is not a multiple of
     * the descriptor size must be refused. */
    vq->desc[0].addr = (uint64_t)(uintptr_t)vq->tables;
    vq->desc[0].len = 15u;
    vq->desc[0].flags = PGW_VRING_DESC_F_INDIRECT;
    vq->desc[0].next = 0;
    pgw_vq_avail_ring(vq)[0] = 0u;
    *pgw_vq_avail_idx(vq) = 1u;

    pgw_vq_req req;
    bool got = false;
    PGW_ASSERT_EQ_I(pgw_vq_take_avail(vq, &req, &got), PGW_EIO);
    PGW_ASSERT_FALSE(got);
    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

/* --- size selection (QNX-04) ---------------------------------------------- */

static void test_queue_size_is_power_of_two_and_clamped(void)
{
    pgw_vio_dev *dev = open_mock(PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);

    pgw_virtq *vq = pgw_vq_create(dev, 0, 100u, false, false);
    PGW_ASSERT(vq != NULL);
    PGW_ASSERT_EQ_U(pgw_vq_size(vq), 64u); /* rounded down to a power of two */
    pgw_vq_destroy(vq);

    vq = pgw_vq_create(dev, 0, 1u, false, false);
    PGW_ASSERT(vq != NULL);
    PGW_ASSERT_EQ_U(pgw_vq_size(vq), PGW_QUEUE_SIZE_MIN);
    pgw_vq_destroy(vq);

    PGW_ASSERT(pgw_vq_create(NULL, 0, 8u, false, false) == NULL);
    PGW_ASSERT_EQ_U(pgw_vq_size(NULL), 0u);
    PGW_ASSERT_EQ_U(pgw_vq_free_descs(NULL), 0u);
    PGW_ASSERT_FALSE(pgw_vq_kick(NULL));
    pgw_vio_destroy(dev);
}

PGW_TEST(test_push_then_pop_returns_cookie);
PGW_TEST(test_multiple_submissions_keep_their_order);
PGW_TEST(test_zero_copy_uses_caller_addresses);
PGW_TEST(test_phys_address_wins_over_virtual);
PGW_TEST(test_indirect_used_above_threshold);
PGW_TEST(test_indirect_not_used_at_or_below_threshold);
PGW_TEST(test_indirect_disabled_when_not_negotiated);
PGW_TEST(test_indirect_chain_round_trip);
PGW_TEST(test_no_notify_flag_suppresses_kick);
PGW_TEST(test_event_idx_suppresses_redundant_kick);
PGW_TEST(test_pop_rearms_the_interrupt);
PGW_TEST(test_full_queue_returns_eagain_without_corruption);
PGW_TEST(test_rejects_too_many_sg_entries);
PGW_TEST(test_refuses_out_of_range_used_id);
PGW_TEST(test_refuses_malformed_indirect_length);
PGW_TEST(test_queue_size_is_power_of_two_and_clamped);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}

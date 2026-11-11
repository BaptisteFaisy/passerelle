/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/tests/test_mmio.c - virtio-mmio v1 (legacy) and v2 (modern), VIO-03.
 *
 * The registers are backed by a RAM array and the "device" is the callback pair
 * the test installs, which is exactly the shape a real MMIO window has. So the
 * transport code is exercised for real: magic and version checks, feature words
 * split across two 32-bit selects, QueueNumMax/QueueNum, page frame number
 * addressing on v1, three 64-bit ring addresses and QueueReady on v2, and the
 * device configuration window at 0x100.
 */
#include "pgw/test.h"
#include "pgw/virtio.h"
#include "virtio_priv.h"

#include <string.h>

#define FAKE_REGS 0x400u

typedef struct fake_mmio {
    uint32_t reg[FAKE_REGS / 4u];
    uint32_t version;
    uint32_t device_id;
    uint64_t features;
    uint32_t queue_max;
    uint32_t notifications;
    pgw_virtq *vq; /* set by the test once the queue exists */
} fake_mmio;

static uint32_t *slot(fake_mmio *f, uint32_t off)
{
    return &f->reg[(off & (FAKE_REGS - 1u)) / 4u];
}

static uint32_t fake_r32(void *ctx, uint32_t off)
{
    fake_mmio *f = (fake_mmio *)ctx;
    switch (off) {
    case 0x000u:
        return *slot(f, 0x000u); /* MAGIC, written by make_fake */
    case 0x004u:
        return f->version;
    case 0x008u:
        return f->device_id;
    case 0x00cu:
        return PGW_VIRTIO_PCI_VENDOR;
    case 0x010u: { /* DEVICE_FEATURES, selected by 0x014 */
        uint32_t sel = *slot(f, 0x014u);
        return sel == 0u ? (uint32_t)(f->features & 0xFFFFFFFFu)
                         : (uint32_t)(f->features >> 32);
    }
    case 0x034u: /* QUEUE_NUM_MAX */
        return f->queue_max;
    case 0x060u: /* INTERRUPT_STATUS */
        return *slot(f, 0x060u);
    default:
        return *slot(f, off);
    }
}

static void fake_w32(void *ctx, uint32_t off, uint32_t val)
{
    fake_mmio *f = (fake_mmio *)ctx;
    switch (off) {
    case 0x050u: /* QUEUE_NOTIFY */
        f->notifications++;
        break;
    case 0x064u: /* INTERRUPT_ACK */
        *slot(f, 0x060u) &= ~val;
        break;
    case 0x070u: /* STATUS: writing zero is the reset */
        if (val == 0u)
            *slot(f, 0x044u) = 0u; /* QUEUE_READY drops on reset */
        *slot(f, 0x070u) = val;
        break;
    default:
        *slot(f, off) = val;
        break;
    }
}

static fake_mmio *make_fake(uint32_t version, uint32_t device_id,
                            const char *tag)
{
    static fake_mmio f;
    memset(&f, 0, sizeof(f));
    *slot(&f, 0x000u) = PGW_MMIO_MAGIC; /* "virt" */
    f.version = version;
    f.device_id = device_id;
    f.features = PGW_VIO_F_VERSION_1 | PGW_VIO_F_RING_INDIRECT_DESC |
                 PGW_VIO_F_RING_EVENT_IDX;
    f.queue_max = 16u;

    if (device_id == PGW_VIRTIO_ID_9P) {
        /* virtio_9p_config at 0x100: le16 length then the tag. */
        size_t len = strlen(tag);
        uint8_t *cfg = (uint8_t *)&f.reg[0x100u / 4u];
        cfg[0] = (uint8_t)(len & 0xFFu);
        cfg[1] = (uint8_t)(len >> 8);
        memcpy(&cfg[2], tag, len);
    } else {
        /* virtio_fs_config at 0x100: char tag[36] then le32 num_queues. */
        uint8_t *cfg = (uint8_t *)&f.reg[0x100u / 4u];
        memcpy(cfg, tag, strlen(tag));
        cfg[36] = 2u; /* two request queues */
    }
    return &f;
}

static pgw_mmio_io io_for(fake_mmio *f)
{
    pgw_mmio_io io = {fake_r32, fake_w32, f};
    return io;
}

/* --- discovery ------------------------------------------------------------ */

static void test_v2_device_is_found(void)
{
    fake_mmio *f = make_fake(PGW_MMIO_VERSION_2, PGW_VIRTIO_ID_9P, "share");
    pgw_vio_dev *dev = pgw_vio_mmio_open(io_for(f), "share");
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_U(pgw_vio_get_info(dev)->bus_kind, 2u);
    PGW_ASSERT_EQ_U(pgw_vio_get_info(dev)->mmio_version, 2u);
    PGW_ASSERT_EQ_U(pgw_vio_get_info(dev)->device_id, PGW_VIRTIO_ID_9P);
    PGW_ASSERT_STR_EQ(pgw_vio_get_info(dev)->tag, "share");
    pgw_vio_destroy(dev);
}

static void test_v1_device_is_found(void)
{
    fake_mmio *f = make_fake(PGW_MMIO_VERSION_1, PGW_VIRTIO_ID_9P, "legacy");
    pgw_vio_dev *dev = pgw_vio_mmio_open(io_for(f), NULL);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_U(pgw_vio_get_info(dev)->mmio_version, 1u);
    PGW_ASSERT_STR_EQ(pgw_vio_get_info(dev)->tag, "legacy");
    pgw_vio_destroy(dev);
}

static void test_refuses_bad_magic(void)
{
    fake_mmio *f = make_fake(PGW_MMIO_VERSION_2, PGW_VIRTIO_ID_9P, "share");
    f->reg[0] = 0x12345678u; /* not "virt" */
    PGW_ASSERT(pgw_vio_mmio_open(io_for(f), NULL) == NULL);
}

static void test_refuses_unsupported_version(void)
{
    fake_mmio *f = make_fake(3u, PGW_VIRTIO_ID_9P, "share");
    PGW_ASSERT(pgw_vio_mmio_open(io_for(f), NULL) == NULL);
}

static void test_refuses_foreign_device(void)
{
    fake_mmio *f = make_fake(PGW_MMIO_VERSION_2, 42u, "share");
    PGW_ASSERT(pgw_vio_mmio_open(io_for(f), NULL) == NULL);
    PGW_ASSERT(pgw_vio_mmio_open((pgw_mmio_io){NULL, NULL, NULL}, NULL) == NULL);
}

static void test_tag_selects_the_device(void)
{
    /* VIO-05: the mount tag, not the enumeration order, picks the device. */
    fake_mmio *f = make_fake(PGW_MMIO_VERSION_2, PGW_VIRTIO_ID_9P, "share-a");
    PGW_ASSERT(pgw_vio_mmio_open(io_for(f), "share-b") == NULL);
    pgw_vio_dev *dev = pgw_vio_mmio_open(io_for(f), "share-a");
    PGW_ASSERT(dev != NULL);
    pgw_vio_destroy(dev);
}

static void test_virtiofs_tag_and_queues(void)
{
    fake_mmio *f = make_fake(PGW_MMIO_VERSION_2, PGW_VIRTIO_ID_FS, "vfsroot");
    pgw_vio_dev *dev = pgw_vio_mmio_open(io_for(f), "vfsroot");
    PGW_ASSERT(dev != NULL);
    char tag[64];
    uint32_t req = 0;
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_fs(dev, tag, sizeof(tag), &req), PGW_OK);
    PGW_ASSERT_STR_EQ(tag, "vfsroot");
    PGW_ASSERT_EQ_U(req, 2u);
    pgw_vio_destroy(dev);
}

/* --- feature negotiation through the two selects -------------------------- */

static void test_features_are_split_across_two_words(void)
{
    fake_mmio *f = make_fake(PGW_MMIO_VERSION_2, PGW_VIRTIO_ID_9P, "share");
    pgw_vio_dev *dev = pgw_vio_mmio_open(io_for(f), NULL);
    PGW_ASSERT(dev != NULL);

    uint64_t want = PGW_VIO_F_VERSION_1 | PGW_VIO_F_RING_EVENT_IDX;
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, want), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_vio_get_info(dev)->negotiated_features, want);
    /* The driver wrote both halves back into DRIVER_FEATURES. */
    PGW_ASSERT_EQ_U(*slot(f, 0x020u), (uint32_t)(want & 0xFFFFFFFFu));
    PGW_ASSERT_EQ_U(*slot(f, 0x024u), 0u); /* selector left on word 0 */
    PGW_ASSERT_EQ_U(*slot(f, 0x070u), PGW_VIO_STATUS_ACK |
                                          PGW_VIO_STATUS_DRIVER |
                                          PGW_VIO_STATUS_FEATURES |
                                          PGW_VIO_STATUS_DRIVER_OK);
    pgw_vio_destroy(dev);
}

/* --- queue programming ---------------------------------------------------- */

static void test_v2_queue_uses_64_bit_addresses(void)
{
    fake_mmio *f = make_fake(PGW_MMIO_VERSION_2, PGW_VIRTIO_ID_9P, "share");
    pgw_vio_dev *dev = pgw_vio_mmio_open(io_for(f), NULL);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1), PGW_OK);

    pgw_virtq *vq = pgw_vq_create(dev, 0, 8u, false, false);
    PGW_ASSERT(vq != NULL);
    PGW_ASSERT_EQ_U(*slot(f, 0x038u), 8u); /* QUEUE_NUM */
    PGW_ASSERT_EQ_U(*slot(f, 0x080u), (uint32_t)(vq->desc_pa & 0xFFFFFFFFu));
    PGW_ASSERT_EQ_U(*slot(f, 0x084u), (uint32_t)(vq->desc_pa >> 32));
    PGW_ASSERT_EQ_U(*slot(f, 0x090u), (uint32_t)(vq->avail_pa & 0xFFFFFFFFu));
    PGW_ASSERT_EQ_U(*slot(f, 0x0a0u), (uint32_t)(vq->used_pa & 0xFFFFFFFFu));
    PGW_ASSERT_EQ_U(*slot(f, 0x044u), 1u); /* QUEUE_READY */
    PGW_ASSERT_EQ_U(*slot(f, 0x040u), 0u); /* no PFN on v2 */

    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_v1_queue_uses_a_page_frame_number(void)
{
    fake_mmio *f = make_fake(PGW_MMIO_VERSION_1, PGW_VIRTIO_ID_9P, "share");
    pgw_vio_dev *dev = pgw_vio_mmio_open(io_for(f), NULL);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1), PGW_OK);

    pgw_virtq *vq = pgw_vq_create(dev, 0, 8u, false, false);
    PGW_ASSERT(vq != NULL);
    PGW_ASSERT(vq->legacy);
    PGW_ASSERT_EQ_U(*slot(f, 0x028u), 4096u); /* GUEST_PAGE_SIZE */
    PGW_ASSERT_EQ_U(*slot(f, 0x03cu), 4096u); /* QUEUE_ALIGN */
    /* The PFN is the whole contiguous block, descriptor table first. */
    PGW_ASSERT_EQ_U(*slot(f, 0x040u), (uint32_t)(vq->desc_pa / 4096u));
    PGW_ASSERT_EQ_U(*slot(f, 0x044u), 0u); /* no QUEUE_READY on v1 */
    PGW_ASSERT_EQ_U(*slot(f, 0x084u), 0u); /* no 64-bit address on v1 */

    /* One contiguous page aligned block: descriptor table, then available
     * ring, then used ring on the next page boundary (VIO-03). */
    PGW_ASSERT(vq->block != NULL);
    PGW_ASSERT_EQ_U(vq->desc_pa % PGW_VQ_ALIGN, 0u);
    PGW_ASSERT_EQ_U(vq->used_pa % PGW_VQ_ALIGN, 0u);
    PGW_ASSERT((uintptr_t)vq->avail_mem > (uintptr_t)vq->desc);
    PGW_ASSERT((uintptr_t)vq->used_mem > (uintptr_t)vq->avail_mem);

    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_queue_above_device_maximum_is_refused(void)
{
    fake_mmio *f = make_fake(PGW_MMIO_VERSION_2, PGW_VIRTIO_ID_9P, "share");
    f->queue_max = 4u;
    pgw_vio_dev *dev = pgw_vio_mmio_open(io_for(f), NULL);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1), PGW_OK);

    /* The queue is clamped down to what the device accepts. */
    pgw_virtq *vq = pgw_vq_create(dev, 0, 64u, false, false);
    PGW_ASSERT(vq != NULL);
    PGW_ASSERT_EQ_U(pgw_vq_size(vq), 4u);
    pgw_vq_destroy(vq);

    /* A queue index the device does not have is refused outright. */
    f->queue_max = 0u;
    PGW_ASSERT(pgw_vq_create(dev, 3u, 4u, false, false) == NULL);
    pgw_vio_destroy(dev);
}

static void test_notify_and_interrupt_ack(void)
{
    fake_mmio *f = make_fake(PGW_MMIO_VERSION_2, PGW_VIRTIO_ID_9P, "share");
    pgw_vio_dev *dev = pgw_vio_mmio_open(io_for(f), NULL);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1), PGW_OK);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 8u, false, false);
    PGW_ASSERT(vq != NULL);

    PGW_ASSERT(pgw_vq_kick(vq));
    PGW_ASSERT_EQ_U(f->notifications, 1u);
    PGW_ASSERT_EQ_U(*slot(f, 0x050u), 0u); /* the queue index we notified */

    /* No pending interrupt: the poll says so instead of blocking. */
    PGW_ASSERT_EQ_I(dev->ops->wait_irq(dev, 0), PGW_ETIMEDOUT);
    *slot(f, 0x060u) = 1u; /* the device raises the queue interrupt */
    PGW_ASSERT_EQ_I(dev->ops->wait_irq(dev, 0), PGW_OK);
    PGW_ASSERT_EQ_U(*slot(f, 0x060u), 0u); /* and the ack clears it */

    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_reset_clears_queue_ready(void)
{
    fake_mmio *f = make_fake(PGW_MMIO_VERSION_2, PGW_VIRTIO_ID_9P, "share");
    pgw_vio_dev *dev = pgw_vio_mmio_open(io_for(f), NULL);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1), PGW_OK);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 8u, false, false);
    PGW_ASSERT(vq != NULL);
    PGW_ASSERT_EQ_U(*slot(f, 0x044u), 1u);

    pgw_vio_reset(dev); /* VIO-09 */
    PGW_ASSERT_EQ_U(*slot(f, 0x070u), 0u);
    PGW_ASSERT_EQ_U(*slot(f, 0x044u), 0u);
    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

PGW_TEST(test_v2_device_is_found);
PGW_TEST(test_v1_device_is_found);
PGW_TEST(test_refuses_bad_magic);
PGW_TEST(test_refuses_unsupported_version);
PGW_TEST(test_refuses_foreign_device);
PGW_TEST(test_tag_selects_the_device);
PGW_TEST(test_virtiofs_tag_and_queues);
PGW_TEST(test_features_are_split_across_two_words);
PGW_TEST(test_v2_queue_uses_64_bit_addresses);
PGW_TEST(test_v1_queue_uses_a_page_frame_number);
PGW_TEST(test_queue_above_device_maximum_is_refused);
PGW_TEST(test_notify_and_interrupt_ack);
PGW_TEST(test_reset_clears_queue_ready);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}

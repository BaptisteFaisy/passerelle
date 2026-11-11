/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/src/mmio.c - virtio over MMIO, versions 1 (legacy) and 2 (modern).
 *
 * Covers VIO-03: the same register map as virtio 1.2 §4.2, driven through an
 * abstract 32-bit register accessor (`pgw_mmio_io`) so that the whole transport
 * can be unit-tested with registers backed by RAM. The QNX binding that maps a
 * real physical window lives in mmio_qnx.c; nothing in this file knows about an
 * OS.
 *
 * Version differences that matter here:
 *   - v1 addresses a queue with a page frame number (QueuePFN) and requires the
 *     three rings in one contiguous, page aligned block, with GuestPageSize and
 *     QueueAlign written first;
 *   - v2 gives the descriptor table, driver (available) ring and device (used)
 *     ring three 64-bit addresses and a QueueReady flag.
 */
#include "virtio_priv.h"

#include "pgw/limits.h"
#include "pgw/log.h"

#include <stdlib.h>
#include <string.h>

/* Register map (virtio 1.2 §4.2.2). */
#define PGW_MMIO_R_MAGIC        0x000u
#define PGW_MMIO_R_VERSION      0x004u
#define PGW_MMIO_R_DEVICE_ID    0x008u
#define PGW_MMIO_R_VENDOR_ID    0x00cu
#define PGW_MMIO_R_DEV_FEAT     0x010u
#define PGW_MMIO_R_DEV_FEAT_SEL 0x014u
#define PGW_MMIO_R_DRV_FEAT     0x020u
#define PGW_MMIO_R_DRV_FEAT_SEL 0x024u
#define PGW_MMIO_R_GUEST_PAGE   0x028u
#define PGW_MMIO_R_QUEUE_SEL    0x030u
#define PGW_MMIO_R_QUEUE_MAX    0x034u
#define PGW_MMIO_R_QUEUE_NUM    0x038u
#define PGW_MMIO_R_QUEUE_ALIGN  0x03cu
#define PGW_MMIO_R_QUEUE_PFN    0x040u
#define PGW_MMIO_R_QUEUE_READY  0x044u
#define PGW_MMIO_R_QUEUE_NOTIFY 0x050u
#define PGW_MMIO_R_INT_STATUS   0x060u
#define PGW_MMIO_R_INT_ACK      0x064u
#define PGW_MMIO_R_STATUS       0x070u
#define PGW_MMIO_R_DESC_LOW     0x080u
#define PGW_MMIO_R_DESC_HIGH    0x084u
#define PGW_MMIO_R_AVAIL_LOW    0x090u
#define PGW_MMIO_R_AVAIL_HIGH   0x094u
#define PGW_MMIO_R_USED_LOW     0x0a0u
#define PGW_MMIO_R_USED_HIGH    0x0a4u
#define PGW_MMIO_R_CONFIG_GEN   0x0fcu
#define PGW_MMIO_R_CONFIG       0x100u

#define PGW_MMIO_CFG_MAX  0x400u
#define PGW_MMIO_PAGE     4096u
#define PGW_MMIO_INT_QUEUE 0x1u

typedef struct mmio_state {
    pgw_mmio_io io;
    uint16_t version;
    uint32_t teardown_count;
    uint32_t notify_count;
} mmio_state;

static mmio_state *state_of(pgw_vio_dev *dev)
{
    if (dev == NULL || dev->info.bus_kind != 2u)
        return NULL;
    return (mmio_state *)dev->backend;
}

static uint32_t reg_r(pgw_vio_dev *dev, uint32_t off)
{
    mmio_state *st = state_of(dev);
    return st == NULL ? 0u : st->io.r32(st->io.ctx, off);
}

static void reg_w(pgw_vio_dev *dev, uint32_t off, uint32_t val)
{
    mmio_state *st = state_of(dev);
    if (st != NULL)
        st->io.w32(st->io.ctx, off, val);
}

/* --- bus operations ------------------------------------------------------- */

static void mmio_destroy(pgw_vio_dev *dev)
{
    free(state_of(dev));
}

static void mmio_reset(pgw_vio_dev *dev)
{
    reg_w(dev, PGW_MMIO_R_STATUS, 0u);
    reg_w(dev, PGW_MMIO_R_INT_ACK, 0xFFFFFFFFu); /* drop any pending bit */
}

static uint8_t mmio_get_status(pgw_vio_dev *dev)
{
    return (uint8_t)(reg_r(dev, PGW_MMIO_R_STATUS) & 0xFFu);
}

static void mmio_set_status(pgw_vio_dev *dev, uint8_t status)
{
    reg_w(dev, PGW_MMIO_R_STATUS, status);
}

static uint64_t mmio_get_features(pgw_vio_dev *dev)
{
    reg_w(dev, PGW_MMIO_R_DEV_FEAT_SEL, 1u);
    uint64_t hi = reg_r(dev, PGW_MMIO_R_DEV_FEAT);
    reg_w(dev, PGW_MMIO_R_DEV_FEAT_SEL, 0u);
    uint64_t lo = reg_r(dev, PGW_MMIO_R_DEV_FEAT);
    return (hi << 32) | lo;
}

static void mmio_set_features(pgw_vio_dev *dev, uint64_t features)
{
    reg_w(dev, PGW_MMIO_R_DRV_FEAT_SEL, 1u);
    reg_w(dev, PGW_MMIO_R_DRV_FEAT, (uint32_t)(features >> 32));
    reg_w(dev, PGW_MMIO_R_DRV_FEAT_SEL, 0u);
    reg_w(dev, PGW_MMIO_R_DRV_FEAT, (uint32_t)(features & 0xFFFFFFFFu));
}

static pgw_err mmio_read_config(pgw_vio_dev *dev, uint32_t offset, void *out,
                                size_t len)
{
    if (out == NULL)
        return PGW_EINVAL;
    if (offset > PGW_MMIO_CFG_MAX || len > (size_t)(PGW_MMIO_CFG_MAX - offset))
        return PGW_EINVAL;
    uint8_t *p = (uint8_t *)out;
    for (size_t i = 0; i < len; i++) {
        uint32_t abs = PGW_MMIO_R_CONFIG + offset + (uint32_t)i;
        uint32_t word = reg_r(dev, abs & ~3u);
        p[i] = (uint8_t)((word >> (8u * (abs & 3u))) & 0xFFu);
    }
    return PGW_OK;
}

static pgw_err mmio_program_queue_v1(pgw_vio_dev *dev, const pgw_virtq *vq)
{
    if ((vq->desc_pa % PGW_MMIO_PAGE) != 0u) {
        PGW_ERR("virtio-mmio v1: queue %u is not page aligned",
                (unsigned)vq->qidx);
        return PGW_EINVAL;
    }
    reg_w(dev, PGW_MMIO_R_GUEST_PAGE, PGW_MMIO_PAGE);
    reg_w(dev, PGW_MMIO_R_QUEUE_ALIGN, PGW_MMIO_PAGE);
    /* QueuePFN covers the whole contiguous block, descriptor table first. */
    reg_w(dev, PGW_MMIO_R_QUEUE_PFN, (uint32_t)(vq->desc_pa / PGW_MMIO_PAGE));
    return PGW_OK;
}

static void mmio_program_queue_v2(pgw_vio_dev *dev, const pgw_virtq *vq)
{
    reg_w(dev, PGW_MMIO_R_DESC_LOW, (uint32_t)(vq->desc_pa & 0xFFFFFFFFu));
    reg_w(dev, PGW_MMIO_R_DESC_HIGH, (uint32_t)(vq->desc_pa >> 32));
    reg_w(dev, PGW_MMIO_R_AVAIL_LOW, (uint32_t)(vq->avail_pa & 0xFFFFFFFFu));
    reg_w(dev, PGW_MMIO_R_AVAIL_HIGH, (uint32_t)(vq->avail_pa >> 32));
    reg_w(dev, PGW_MMIO_R_USED_LOW, (uint32_t)(vq->used_pa & 0xFFFFFFFFu));
    reg_w(dev, PGW_MMIO_R_USED_HIGH, (uint32_t)(vq->used_pa >> 32));
    reg_w(dev, PGW_MMIO_R_QUEUE_READY, 1u);
}

static pgw_err mmio_setup_queue(pgw_vio_dev *dev, uint16_t qidx, pgw_virtq *vq)
{
    mmio_state *st = state_of(dev);
    if (st == NULL || vq == NULL)
        return PGW_EINVAL;

    reg_w(dev, PGW_MMIO_R_QUEUE_SEL, qidx);
    uint32_t max = reg_r(dev, PGW_MMIO_R_QUEUE_MAX);
    if (max == 0u) {
        PGW_ERR("virtio-mmio: queue %u does not exist", (unsigned)qidx);
        return PGW_EINVAL;
    }
    if ((uint32_t)vq->size > max) {
        PGW_ERR("virtio-mmio: queue %u asks %u descriptors, device max %u",
                (unsigned)qidx, (unsigned)vq->size, (unsigned)max);
        return PGW_EINVAL;
    }

    reg_w(dev, PGW_MMIO_R_QUEUE_NUM, vq->size);
    if (st->version == 1u)
        return mmio_program_queue_v1(dev, vq);
    mmio_program_queue_v2(dev, vq);
    return PGW_OK;
}

static void mmio_notify_queue(pgw_vio_dev *dev, uint16_t qidx)
{
    mmio_state *st = state_of(dev);
    if (st != NULL)
        st->notify_count++;
    reg_w(dev, PGW_MMIO_R_QUEUE_NOTIFY, qidx);
}

/* A single non-blocking poll: the blocking wait belongs to the interrupt
 * thread of the QNX binding (mmio_qnx.c), not to a portable library. */
static pgw_err mmio_wait_irq(pgw_vio_dev *dev, int32_t timeout_ms)
{
    (void)timeout_ms;
    uint32_t pending = reg_r(dev, PGW_MMIO_R_INT_STATUS);
    if (pending == 0u)
        return PGW_ETIMEDOUT;
    reg_w(dev, PGW_MMIO_R_INT_ACK, pending & PGW_MMIO_INT_QUEUE);
    return PGW_OK;
}

static void mmio_teardown_irq(pgw_vio_dev *dev)
{
    mmio_state *st = state_of(dev);
    if (st != NULL)
        st->teardown_count++;
}

static const pgw_vio_bus_ops g_mmio_ops = {
    .destroy = mmio_destroy,
    .reset = mmio_reset,
    .get_status = mmio_get_status,
    .set_status = mmio_set_status,
    .get_features = mmio_get_features,
    .set_features = mmio_set_features,
    .read_config = mmio_read_config,
    .setup_queue = mmio_setup_queue,
    .notify_queue = mmio_notify_queue,
    .wait_irq = mmio_wait_irq,
    .teardown_irq = mmio_teardown_irq,
};

/* --- opening -------------------------------------------------------------- */

uint16_t pgw_vio_mmio_queue_max(pgw_vio_dev *dev, uint16_t qidx)
{
    if (dev == NULL || dev->info.bus_kind != 2u)
        return 0;
    reg_w(dev, PGW_MMIO_R_QUEUE_SEL, qidx);
    return (uint16_t)(reg_r(dev, PGW_MMIO_R_QUEUE_MAX) & 0xFFFFu);
}

static pgw_err match_tag(pgw_vio_dev *dev, const char *expected)
{
    char tag[64];
    pgw_err e;
    if (dev->info.device_id == (uint16_t)PGW_VIRTIO_ID_9P) {
        e = pgw_vio_read_tag_9p(dev, tag, sizeof(tag));
    } else {
        uint32_t req = 0;
        e = pgw_vio_read_tag_fs(dev, tag, sizeof(tag), &req);
    }
    if (e != PGW_OK)
        return e;
    if (expected == NULL)
        return PGW_OK;
    if (strcmp(tag, expected) != 0) {
        PGW_INFO("virtio-mmio: device tag '%s' is not '%s'", tag, expected);
        return PGW_ENOENT;
    }
    return PGW_OK;
}

pgw_vio_dev *pgw_vio_mmio_open(pgw_mmio_io io, const char *expected_tag)
{
    if (io.r32 == NULL || io.w32 == NULL) {
        PGW_ERR("virtio-mmio: register accessor is incomplete");
        return NULL;
    }

    uint32_t magic = io.r32(io.ctx, PGW_MMIO_R_MAGIC);
    if (magic != PGW_MMIO_MAGIC) {
        PGW_ERR("virtio-mmio: bad magic 0x%08x (expected \"virt\")",
                (unsigned)magic);
        return NULL;
    }
    uint32_t version = io.r32(io.ctx, PGW_MMIO_R_VERSION);
    if (version != PGW_MMIO_VERSION_1 && version != PGW_MMIO_VERSION_2) {
        PGW_ERR("virtio-mmio: unsupported version %u", (unsigned)version);
        return NULL;
    }
    uint32_t did = io.r32(io.ctx, PGW_MMIO_R_DEVICE_ID);
    if (did != PGW_VIRTIO_ID_9P && did != PGW_VIRTIO_ID_FS) {
        PGW_ERR("virtio-mmio: unsupported device id %u", (unsigned)did);
        return NULL;
    }

    mmio_state *st = calloc(1u, sizeof(*st));
    if (st == NULL)
        return NULL;
    st->io = io;
    st->version = (uint16_t)version;

    pgw_vio_dev *dev = pgw_vio_create(&g_mmio_ops, st, (uint16_t)did, 2u);
    if (dev == NULL) {
        free(st);
        return NULL;
    }
    dev->info.mmio_version = version;
    dev->info.vendor_id =
        (uint16_t)(io.r32(io.ctx, PGW_MMIO_R_VENDOR_ID) & 0xFFFFu);

    /* Read the mount tag even when the caller does not filter on it: the
     * identity of a share is its tag, not its enumeration order (VIO-05). */
    if (match_tag(dev, expected_tag) != PGW_OK) {
        pgw_vio_destroy(dev);
        return NULL;
    }
    PGW_INFO("virtio-mmio: device %u found, version %u", (unsigned)did,
             (unsigned)version);
    return dev;
}

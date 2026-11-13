/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/src/pci_modern.c - the modern PCI transport (virtio 1.2 §4.1).
 *
 * Covers VIO-01, VIO-02, VIO-04 and VIO-05:
 *   - the device is identified by vendor 0x1AF4 and one of 0x1009 (9P
 *     transitional), 0x1049 (9P modern) or 0x105A (virtiofs);
 *   - every register access goes through the COMMON_CFG, NOTIFY, ISR and
 *     DEVICE capabilities found by pci_caps.c, including the
 *     notify_off_multiplier of the notification area;
 *   - MSI-X is programmed from the capability table when the caller allows it,
 *     with the classic INTx (ISR capability) path kept as the fallback;
 *   - the mount tag is read from the device configuration capability.
 *
 * The file never talks to a bus: configuration space and BARs are two function
 * pointer tables. That is what lets the PC test drive a complete modern device
 * out of two RAM arrays, and it is also why the QNX glue in pci_qnx.c is only
 * an adapter.
 */
#include "virtio_priv.h"

#include "pgw/limits.h"
#include "pgw/log.h"

#include <stdlib.h>
#include <string.h>

/* COMMON_CFG layout (virtio 1.2 §4.1.4.3). */
#define PGW_CC_DEV_FEAT_SEL   0x00u
#define PGW_CC_DEV_FEAT       0x04u
#define PGW_CC_DRV_FEAT_SEL   0x08u
#define PGW_CC_DRV_FEAT       0x0cu
#define PGW_CC_MSIX_CONFIG    0x10u
#define PGW_CC_NUM_QUEUES     0x12u
#define PGW_CC_DEVICE_STATUS  0x14u
#define PGW_CC_QUEUE_SELECT   0x16u
#define PGW_CC_QUEUE_SIZE     0x18u
#define PGW_CC_QUEUE_MSIX_VEC 0x1au
#define PGW_CC_QUEUE_ENABLE   0x1cu
#define PGW_CC_QUEUE_NOTIFY   0x1eu
#define PGW_CC_QUEUE_DESC     0x20u
#define PGW_CC_QUEUE_DRIVER   0x28u
#define PGW_CC_QUEUE_DEVICE   0x30u

#define PGW_MSIX_CTRL_ENABLE  0x8000u
#define PGW_MSIX_MSG_ADDR     0xFEE00000ULL /* x86 local APIC, one delivery */
#define PGW_MSIX_MSG_DATA     0u

typedef struct pci_state {
    pgw_pci_cfg_io cfg;
    pgw_pci_bar_io bars;
    pgw_pci_caps caps;
    uint32_t notify_count;
    uint32_t teardown_count;
    uint32_t reset_count;
    bool msix_enabled;
} pci_state;

static pci_state *state_of(pgw_vio_dev *dev)
{
    if (dev == NULL || dev->info.bus_kind != 1u)
        return NULL;
    return (pci_state *)dev->backend;
}

/* --- raw accessors -------------------------------------------------------- */

static uint32_t bar_r32(pci_state *st, uint8_t bar, uint32_t off)
{
    return st->bars.r32(st->bars.ctx, bar, off);
}

static void bar_w32(pci_state *st, uint8_t bar, uint32_t off, uint32_t val)
{
    st->bars.w32(st->bars.ctx, bar, off, val);
}

static uint16_t bar_r16(pci_state *st, uint8_t bar, uint32_t off)
{
    uint32_t word = bar_r32(st, bar, off & ~3u);
    return (uint16_t)((word >> (8u * (off & 3u))) & 0xFFFFu);
}

static void bar_w16(pci_state *st, uint8_t bar, uint32_t off, uint16_t val)
{
    uint32_t base = off & ~3u;
    uint32_t shift = 8u * (off & 3u);
    uint32_t word = bar_r32(st, bar, base);
    word = (word & ~(0xFFFFu << shift)) | ((uint32_t)val << shift);
    bar_w32(st, bar, base, word);
}

static uint8_t bar_r8(pci_state *st, uint8_t bar, uint32_t off)
{
    if (st->bars.r8 != NULL)
        return st->bars.r8(st->bars.ctx, bar, off);
    uint32_t word = bar_r32(st, bar, off & ~3u);
    return (uint8_t)((word >> (8u * (off & 3u))) & 0xFFu);
}

static void bar_w8(pci_state *st, uint8_t bar, uint32_t off, uint8_t val)
{
    if (st->bars.w8 != NULL) {
        st->bars.w8(st->bars.ctx, bar, off, val);
        return;
    }
    uint32_t base = off & ~3u;
    uint32_t shift = 8u * (off & 3u);
    uint32_t word = bar_r32(st, bar, base);
    word = (word & ~(0xFFu << shift)) | ((uint32_t)val << shift);
    bar_w32(st, bar, base, word);
}

static uint16_t cfg_r16(pci_state *st, uint16_t off)
{
    uint8_t b[2] = {0, 0};
    if (st->cfg.read(st->cfg.ctx, off, b, sizeof(b)) != PGW_OK)
        return 0;
    return (uint16_t)((uint16_t)b[0] | (uint16_t)((uint16_t)b[1] << 8));
}

static void cfg_w16(pci_state *st, uint16_t off, uint16_t val)
{
    uint8_t b[2];
    b[0] = (uint8_t)(val & 0xFFu);
    b[1] = (uint8_t)((val >> 8) & 0xFFu);
    if (st->cfg.write != NULL)
        (void)st->cfg.write(st->cfg.ctx, off, b, sizeof(b));
}

static uint32_t cfg_r32(pci_state *st, uint16_t off)
{
    uint8_t b[4] = {0, 0, 0, 0};
    if (st->cfg.read(st->cfg.ctx, off, b, sizeof(b)) != PGW_OK)
        return 0;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static uint32_t common_r32(pci_state *st, uint32_t off)
{
    return bar_r32(st, st->caps.common.bar, st->caps.common.offset + off);
}

static void common_w32(pci_state *st, uint32_t off, uint32_t val)
{
    bar_w32(st, st->caps.common.bar, st->caps.common.offset + off, val);
}

static uint16_t common_r16(pci_state *st, uint32_t off)
{
    return bar_r16(st, st->caps.common.bar, st->caps.common.offset + off);
}

static void common_w16(pci_state *st, uint32_t off, uint16_t val)
{
    bar_w16(st, st->caps.common.bar, st->caps.common.offset + off, val);
}

static void common_w8(pci_state *st, uint32_t off, uint8_t val)
{
    bar_w8(st, st->caps.common.bar, st->caps.common.offset + off, val);
}

static void common_w64(pci_state *st, uint32_t off, uint64_t val)
{
    common_w32(st, off, (uint32_t)(val & 0xFFFFFFFFu));
    common_w32(st, off + 4u, (uint32_t)(val >> 32));
}

/* --- bus operations ------------------------------------------------------- */

static void pci_destroy(pgw_vio_dev *dev)
{
    free(state_of(dev));
}

static void pci_reset(pgw_vio_dev *dev)
{
    pci_state *st = state_of(dev);
    if (st == NULL)
        return;
    common_w8(st, PGW_CC_DEVICE_STATUS, 0u);
    st->reset_count++;
}

static uint8_t pci_get_status(pgw_vio_dev *dev)
{
    pci_state *st = state_of(dev);
    if (st == NULL)
        return 0;
    return bar_r8(st, st->caps.common.bar, st->caps.common.offset +
                                              PGW_CC_DEVICE_STATUS);
}

static void pci_set_status(pgw_vio_dev *dev, uint8_t status)
{
    pci_state *st = state_of(dev);
    if (st != NULL)
        common_w8(st, PGW_CC_DEVICE_STATUS, status);
}

static uint64_t pci_get_features(pgw_vio_dev *dev)
{
    pci_state *st = state_of(dev);
    if (st == NULL)
        return 0;
    common_w32(st, PGW_CC_DEV_FEAT_SEL, 1u);
    uint64_t hi = common_r32(st, PGW_CC_DEV_FEAT);
    common_w32(st, PGW_CC_DEV_FEAT_SEL, 0u);
    uint64_t lo = common_r32(st, PGW_CC_DEV_FEAT);
    return (hi << 32) | lo;
}

static void pci_set_features(pgw_vio_dev *dev, uint64_t features)
{
    pci_state *st = state_of(dev);
    if (st == NULL)
        return;
    common_w32(st, PGW_CC_DRV_FEAT_SEL, 1u);
    common_w32(st, PGW_CC_DRV_FEAT, (uint32_t)(features >> 32));
    common_w32(st, PGW_CC_DRV_FEAT_SEL, 0u);
    common_w32(st, PGW_CC_DRV_FEAT, (uint32_t)(features & 0xFFFFFFFFu));
}

static pgw_err pci_read_config(pgw_vio_dev *dev, uint32_t offset, void *out,
                               size_t len)
{
    pci_state *st = state_of(dev);
    if (st == NULL || out == NULL)
        return PGW_EINVAL;
    if (!st->caps.device.present)
        return PGW_ENOSYS;
    if ((uint64_t)offset + (uint64_t)len > (uint64_t)st->caps.device.length)
        return PGW_EINVAL;

    /* Byte at a time: the device-specific region is usually shorter than the
     * capability length, and a wider read that crosses the real end is
     * answered with 0xff (see the note on pgw_pci_bar_io). The virtio
     * specification makes the device configuration byte-addressable, so this
     * is also the conforming access size. */
    uint8_t *p = (uint8_t *)out;
    uint32_t base = st->caps.device.offset + offset;
    for (size_t i = 0; i < len; i++)
        p[i] = bar_r8(st, st->caps.device.bar, base + (uint32_t)i);
    return PGW_OK;
}

static pgw_err pci_setup_queue(pgw_vio_dev *dev, uint16_t qidx, pgw_virtq *vq)
{
    pci_state *st = state_of(dev);
    if (st == NULL || vq == NULL)
        return PGW_EINVAL;

    common_w16(st, PGW_CC_QUEUE_SELECT, qidx);
    uint16_t max = common_r16(st, PGW_CC_QUEUE_SIZE);
    if (max == 0u) {
        PGW_ERR("virtio-pci: queue %u does not exist", (unsigned)qidx);
        return PGW_EINVAL;
    }
    if (vq->size > max) {
        PGW_ERR("virtio-pci: queue %u asks %u descriptors, device max %u",
                (unsigned)qidx, (unsigned)vq->size, (unsigned)max);
        return PGW_EINVAL;
    }

    common_w16(st, PGW_CC_QUEUE_SIZE, vq->size);
    if (st->msix_enabled)
        common_w16(st, PGW_CC_QUEUE_MSIX_VEC, 0u);
    common_w64(st, PGW_CC_QUEUE_DESC, vq->desc_pa);
    common_w64(st, PGW_CC_QUEUE_DRIVER, vq->avail_pa);
    common_w64(st, PGW_CC_QUEUE_DEVICE, vq->used_pa);
    common_w16(st, PGW_CC_QUEUE_ENABLE, 1u);
    PGW_DBG("virtio-pci: queue %u desc=0x%llx avail=0x%llx used=0x%llx "
            "notify bar=%u off=0x%x mult=%u",
            (unsigned)qidx, (unsigned long long)vq->desc_pa,
            (unsigned long long)vq->avail_pa, (unsigned long long)vq->used_pa,
            (unsigned)st->caps.notify.bar, (unsigned)st->caps.notify.offset,
            (unsigned)st->caps.notify.multiplier);
    return PGW_OK;
}

static void pci_notify_queue(pgw_vio_dev *dev, uint16_t qidx)
{
    pci_state *st = state_of(dev);
    if (st == NULL)
        return;
    st->notify_count++;
    if (!st->caps.notify.present)
        return;
    common_w16(st, PGW_CC_QUEUE_SELECT, qidx);
    uint16_t off = common_r16(st, PGW_CC_QUEUE_NOTIFY);
    /* notify address = area base + queue_notify_off * notify_off_multiplier */
    uint32_t addr = st->caps.notify.offset +
                    (uint32_t)off * st->caps.notify.multiplier;
    bar_w16(st, st->caps.notify.bar, addr, qidx);
}

/* Reading the ISR byte acknowledges the INTx line: one poll, like MMIO. */
static pgw_err pci_wait_irq(pgw_vio_dev *dev, int32_t timeout_ms)
{
    (void)timeout_ms;
    pci_state *st = state_of(dev);
    if (st == NULL)
        return PGW_EINVAL;
    if (st->msix_enabled)
        return PGW_OK; /* MSI-X delivers through the vector, nothing to poll */
    if (!st->caps.isr.present)
        return PGW_ENOSYS;
    uint8_t pending = bar_r8(st, st->caps.isr.bar, st->caps.isr.offset);
    return pending != 0u ? PGW_OK : PGW_ETIMEDOUT;
}

static void pci_teardown_irq(pgw_vio_dev *dev)
{
    pci_state *st = state_of(dev);
    if (st == NULL)
        return;
    st->teardown_count++;
    if (!st->msix_enabled)
        return;
    uint16_t ctrl = cfg_r16(st, (uint16_t)(st->caps.msix_off + 2u));
    cfg_w16(st, (uint16_t)(st->caps.msix_off + 2u),
            (uint16_t)(ctrl & (uint16_t)~PGW_MSIX_CTRL_ENABLE));
    st->msix_enabled = false;
}

static const pgw_vio_bus_ops g_pci_ops = {
    .destroy = pci_destroy,
    .reset = pci_reset,
    .get_status = pci_get_status,
    .set_status = pci_set_status,
    .get_features = pci_get_features,
    .set_features = pci_set_features,
    .read_config = pci_read_config,
    .setup_queue = pci_setup_queue,
    .notify_queue = pci_notify_queue,
    .wait_irq = pci_wait_irq,
    .teardown_irq = pci_teardown_irq,
};

/* --- MSI-X (VIO-04) ------------------------------------------------------- */

/* Program vector 0 and unmask it, then enable MSI-X in the capability
 * (virtio 1.2 §4.1.5, PCI Local Bus 3.0 §6.8.2). */
static pgw_err pci_msix_program(pci_state *st, uint64_t msg_addr,
                                uint32_t msg_data)
{
    if (st->caps.msix_off == 0u)
        return PGW_ENOENT;

    uint16_t ctrl = cfg_r16(st, (uint16_t)(st->caps.msix_off + 2u));
    uint16_t vectors = (uint16_t)((ctrl & 0x07FFu) + 1u);
    if (vectors == 0u) {
        PGW_ERR("virtio-pci: MSI-X table is empty");
        return PGW_EIO;
    }
    uint32_t table = cfg_r32(st, (uint16_t)(st->caps.msix_off + 4u));
    uint8_t bar = (uint8_t)(table & 0x07u);
    uint32_t off = table & ~0x07u;
    if (bar > 5u) {
        PGW_ERR("virtio-pci: MSI-X table sits in BAR %u", (unsigned)bar);
        return PGW_EIO;
    }

    bar_w32(st, bar, off + 0u, (uint32_t)(msg_addr & 0xFFFFFFFFu));
    bar_w32(st, bar, off + 4u, (uint32_t)(msg_addr >> 32));
    bar_w32(st, bar, off + 8u, msg_data);
    bar_w32(st, bar, off + 12u, 0u); /* vector control: unmasked, not pending */

    cfg_w16(st, (uint16_t)(st->caps.msix_off + 2u),
            (uint16_t)(ctrl | PGW_MSIX_CTRL_ENABLE));
    uint16_t after = cfg_r16(st, (uint16_t)(st->caps.msix_off + 2u));
    if ((after & PGW_MSIX_CTRL_ENABLE) == 0u) {
        PGW_WARN("virtio-pci: MSI-X refused to enable, falling back to INTx");
        return PGW_EOPNOTSUPP;
    }
    st->msix_enabled = true;
    common_w16(st, PGW_CC_MSIX_CONFIG, 0u); /* config changes: vector 0 */
    return PGW_OK;
}

/* --- opening -------------------------------------------------------------- */

pgw_err pgw_vio_pci_sim_msix(pgw_vio_dev *dev, uint64_t msg_addr,
                             uint32_t msg_data)
{
    pci_state *st = state_of(dev);
    if (st == NULL)
        return PGW_EINVAL;
    pgw_err e = pci_msix_program(st, msg_addr, msg_data);
    if (e == PGW_OK)
        dev->info.msix_active = true;
    return e;
}

uint16_t pgw_vio_pci_queue_max(pgw_vio_dev *dev, uint16_t qidx)
{
    pci_state *st = state_of(dev);
    if (st == NULL)
        return 0;
    common_w16(st, PGW_CC_QUEUE_SELECT, qidx);
    return common_r16(st, PGW_CC_QUEUE_SIZE);
}

static bool pci_id_matches(uint16_t wanted_type, uint16_t pci_device_id)
{
    if (wanted_type == (uint16_t)PGW_VIRTIO_ID_9P)
        return pci_device_id == (uint16_t)PGW_VIRTIO_PCI_DID_9P_TRANSITIONAL ||
               pci_device_id == (uint16_t)PGW_VIRTIO_PCI_DID_9P_MODERN;
    if (wanted_type == (uint16_t)PGW_VIRTIO_ID_FS)
        return pci_device_id == (uint16_t)PGW_VIRTIO_PCI_DID_FS_MODERN;
    return false;
}

static pgw_err pci_match_tag(pgw_vio_dev *dev, const char *expected)
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
        PGW_INFO("virtio-pci: device tag '%s' is not '%s'", tag, expected);
        return PGW_ENOENT;
    }
    return PGW_OK;
}

pgw_vio_dev *pgw_vio_pci_sim_open(uint16_t device_id,
                                  const pgw_pci_cfg_io *cfg,
                                  pgw_pci_bar_io bars,
                                  const char *expected_tag, bool allow_msix)
{
    if (cfg == NULL || cfg->read == NULL || bars.r32 == NULL ||
        bars.w32 == NULL) {
        PGW_ERR("virtio-pci: bus accessors are incomplete");
        return NULL;
    }

    pgw_pci_caps caps;
    pgw_err e = pgw_pci_scan_caps(cfg, &caps);
    if (e != PGW_OK) {
        PGW_ERR("virtio-pci: capability scan failed (%d)", (int)e);
        return NULL;
    }
    if (caps.vendor_id != (uint16_t)PGW_VIRTIO_PCI_VENDOR) {
        PGW_ERR("virtio-pci: vendor 0x%04x is not virtio",
                (unsigned)caps.vendor_id);
        return NULL;
    }
    if (!pci_id_matches(device_id, caps.device_id)) {
        PGW_ERR("virtio-pci: device 0x%04x is not virtio type %u",
                (unsigned)caps.device_id, (unsigned)device_id);
        return NULL;
    }

    pci_state *st = calloc(1u, sizeof(*st));
    if (st == NULL)
        return NULL;
    st->cfg = *cfg;
    st->bars = bars;
    st->caps = caps;

    pgw_vio_dev *dev = pgw_vio_create(&g_pci_ops, st, device_id, 1u);
    if (dev == NULL) {
        free(st);
        return NULL;
    }
    dev->info.vendor_id = caps.vendor_id;
    dev->info.num_queues = common_r16(st, PGW_CC_NUM_QUEUES);

    if (allow_msix &&
        pci_msix_program(st, PGW_MSIX_MSG_ADDR, PGW_MSIX_MSG_DATA) == PGW_OK) {
        dev->info.msix_active = true;
        PGW_INFO("virtio-pci: MSI-X enabled");
    }

    if (pci_match_tag(dev, expected_tag) != PGW_OK) {
        pgw_vio_destroy(dev);
        return NULL;
    }
    PGW_INFO("virtio-pci: device 0x%04x found, %u queues",
             (unsigned)caps.device_id, (unsigned)dev->info.num_queues);
    return dev;
}

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/src/pci_qnx.c - QNX 8.0 binding for the modern PCI transport.
 *
 * Compiled only when the target is QNX (see virtio/CMakeLists.txt); the PC
 * build never sees <pci/pci.h> or <sys/neutrino.h>.
 *
 * This file is deliberately thin. It does exactly three things, and each of
 * them needs the QNX PCI server:
 *   1. find the virtio device by vendor/device id and attach to it (VIO-01);
 *   2. expose PCI configuration space and the BARs through the two accessor
 *      tables of virtio_priv.h, so that pci_caps.c and pci_modern.c - which are
 *      unit-tested on the PC - do all the discovery and register programming
 *      (VIO-02);
 *   3. wire the interrupt: MSI-X when the caller allows it, otherwise the
 *      classic INTx line through InterruptAttachThread (VIO-04), and detach it
 *      on reset so that nothing is left attached (VIO-09).
 *
 * Rings are allocated physically contiguous here, because the device reads them
 * by DMA address.
 */
#include "virtio_priv.h"

#if defined(__QNX__)

#include "pgw/limits.h"
#include "pgw/log.h"

#include <errno.h>
#include <pci/pci.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>

#define PGW_QNX_MAX_BARS 6
#define PGW_QNX_IRQ_PRIO 21
#define PGW_QNX_PAGE     4096u

/* The PCI command register (PCI Local Bus 3.0, 6.2.2): memory space enable
 * (bit 1) and bus master enable (bit 2). The SDP 8.0 headers do not name
 * these bits, so they are spelled out here once. */
#define PGW_PCI_CMD_MEM    0x0002u
#define PGW_PCI_CMD_MASTER 0x0004u

typedef struct qnx_pci {
    pci_bdf_t bdf;
    pci_devhdl_t hdl;
    void *bar[PGW_QNX_MAX_BARS];
    size_t bar_len[PGW_QNX_MAX_BARS];
    int32_t irq;
    int32_t irq_id;
} qnx_pci;

/* --- accessors handed to the portable layer ------------------------------- */

static pgw_err qnx_cfg_read(void *ctx, uint16_t off, void *out, size_t len)
{
    qnx_pci *q = (qnx_pci *)ctx;
    uint8_t *p = (uint8_t *)out;
    for (size_t i = 0; i < len; i++) {
        uint16_t at = (uint16_t)(off + (uint16_t)i);
        uint32_t word = 0;
        if (pci_device_cfg_rd32(q->bdf, (uint16_t)(at & ~3u), &word) !=
            PCI_ERR_OK)
            return PGW_EIO;
        p[i] = (uint8_t)((word >> (8u * (at & 3u))) & 0xFFu);
    }
    return PGW_OK;
}

static pgw_err qnx_cfg_write(void *ctx, uint16_t off, const void *in,
                             size_t len)
{
    qnx_pci *q = (qnx_pci *)ctx;
    const uint8_t *p = (const uint8_t *)in;
    for (size_t i = 0; i < len; i++) {
        uint16_t at = (uint16_t)(off + (uint16_t)i);
        uint32_t word = 0;
        if (pci_device_cfg_rd32(q->bdf, (uint16_t)(at & ~3u), &word) !=
            PCI_ERR_OK)
            return PGW_EIO;
        uint32_t shift = 8u * (at & 3u);
        word = (word & ~(0xFFu << shift)) | ((uint32_t)p[i] << shift);
        if (pci_device_cfg_wr32(q->hdl, (uint16_t)(at & ~3u), word, &word) !=
            PCI_ERR_OK)
            return PGW_EIO;
    }
    return PGW_OK;
}

static uint32_t qnx_bar_r32(void *ctx, uint8_t bar, uint32_t off)
{
    qnx_pci *q = (qnx_pci *)ctx;
    if (bar >= PGW_QNX_MAX_BARS || q->bar[bar] == NULL)
        return 0;
    return *(volatile uint32_t *)((uint8_t *)q->bar[bar] + off);
}

static void qnx_bar_w32(void *ctx, uint8_t bar, uint32_t off, uint32_t val)
{
    qnx_pci *q = (qnx_pci *)ctx;
    if (bar >= PGW_QNX_MAX_BARS || q->bar[bar] == NULL)
        return;
    *(volatile uint32_t *)((uint8_t *)q->bar[bar] + off) = val;
}

/* --- interrupt (VIO-04, VIO-09) ------------------------------------------- */

static void qnx_irq_release(pgw_vio_dev *dev)
{
    qnx_pci *q = (qnx_pci *)dev->backend;
    if (q != NULL)
        pgw_qnx_irq_detach(&q->irq_id);
}

static bool qnx_irq_attach(qnx_pci *q)
{
    int_t nirq = 0;
    pci_irq_t irqs[4] = {0, 0, 0, 0};
    if (pci_device_read_irq(q->hdl, &nirq, irqs) != PCI_ERR_OK || nirq == 0) {
        PGW_ERR("virtio-pci: the device reports no interrupt line");
        return false;
    }
    q->irq = (int32_t)irqs[0];
    /* The handler thread waits on the line; the driver polls the used ring. */
    q->irq_id = pgw_qnx_irq_attach(q->irq, PGW_QNX_IRQ_PRIO);
    return q->irq_id > 0;
}

/* --- BAR mapping ---------------------------------------------------------- */

static void qnx_map_bars(qnx_pci *q)
{
    pci_ba_t bas[PGW_QNX_MAX_BARS];
    int_t nba = PGW_QNX_MAX_BARS;
    if (pci_device_read_ba(q->hdl, &nba, bas, pci_reqType_e_UNSPECIFIED) !=
        PCI_ERR_OK) {
        PGW_ERR("virtio-pci: pci_device_read_ba failed");
        return;
    }
    for (int_t i = 0; i < nba; i++) {
        uint8_t num = (uint8_t)bas[i].bar_num;
        if (num >= PGW_QNX_MAX_BARS || q->bar[num] != NULL)
            continue;
        size_t len = (size_t)bas[i].size;
        if (len == 0)
            len = PGW_QNX_PAGE;
        void *va = mmap_device_memory(NULL, len,
                                      PROT_READ | PROT_WRITE | PROT_NOCACHE, 0,
                                      (uint64_t)bas[i].addr);
        if (va == MAP_FAILED) {
            PGW_ERR("virtio-pci: cannot map BAR%u: %s", (unsigned)num,
                    strerror(errno));
            continue;
        }
        q->bar[num] = va;
        q->bar_len[num] = len;
    }
}

static void qnx_unmap_bars(qnx_pci *q)
{
    for (uint8_t i = 0; i < PGW_QNX_MAX_BARS; i++) {
        if (q->bar[i] != NULL)
            munmap(q->bar[i], q->bar_len[i]);
        q->bar[i] = NULL;
    }
}

/* --- opening -------------------------------------------------------------- */

static void qnx_pci_teardown(pgw_vio_dev *dev)
{
    qnx_pci *q = (qnx_pci *)dev->backend;
    if (q == NULL)
        return;
    qnx_irq_release(dev);
    qnx_unmap_bars(q);
    if (q->hdl != NULL)
        pci_device_detach(q->hdl);
}

/* The device id a virtio implementation presents depends on how it was
 * created: a modern-only device is 0x1040 + type, a transitional one
 * 0x1000 + type. QEMU's virtio-9p-pci is transitional by default, so the
 * driver looks for both (VIO-01). Returns how many ids were filled. */
static size_t pci_dids_for(uint16_t virtio_type, uint16_t *out, size_t max)
{
    if (virtio_type == (uint16_t)PGW_VIRTIO_ID_9P) {
        if (max < 2u)
            return 0;
        out[0] = (uint16_t)PGW_VIRTIO_PCI_DID_9P_MODERN;
        out[1] = (uint16_t)PGW_VIRTIO_PCI_DID_9P_TRANSITIONAL;
        return 2;
    }
    if (virtio_type == (uint16_t)PGW_VIRTIO_ID_FS) {
        if (max < 2u)
            return 0;
        out[0] = (uint16_t)PGW_VIRTIO_PCI_DID_FS_MODERN;
        out[1] = (uint16_t)PGW_VIRTIO_PCI_DID_FS_TRANSITIONAL;
        return 2;
    }
    return 0;
}

/* Find the device on the bus, modern id first then transitional. */
static pci_bdf_t pci_find_virtio(uint16_t virtio_type)
{
    uint16_t dids[2];
    size_t n = pci_dids_for(virtio_type, dids, 2u);
    for (size_t i = 0; i < n; i++) {
        pci_bdf_t bdf = pci_device_find(0, (uint16_t)PGW_VIRTIO_PCI_VENDOR,
                                        dids[i], PCI_CCODE_ANY);
        if (bdf != PCI_BDF_NONE)
            return bdf;
    }
    return PCI_BDF_NONE;
}

pgw_vio_dev *pgw_vio_pci_open(uint16_t device_id, const char *expected_tag,
                              bool allow_msix)
{
    uint16_t dids[2];
    if (pci_dids_for(device_id, dids, 2u) == 0u) {
        PGW_ERR("virtio-pci: unsupported virtio type %u", (unsigned)device_id);
        return NULL;
    }

    pci_bdf_t bdf = pci_find_virtio(device_id);
    if (bdf == PCI_BDF_NONE) {
        PGW_WARN("virtio-pci: no device 0x%04x/0x%04x on the bus",
                 (unsigned)dids[0], (unsigned)dids[1]);
        return NULL;
    }

    qnx_pci *q = calloc(1u, sizeof(*q));
    if (q == NULL)
        return NULL;
    q->bdf = bdf;
    q->irq = -1;
    q->irq_id = -1;

    pci_err_t err = PCI_ERR_OK;
    q->hdl = pci_device_attach(bdf, pci_attachFlags_e_OWNER, &err);
    if (q->hdl == NULL) {
        PGW_ERR("virtio-pci: pci_device_attach failed: %s", pci_strerror(err));
        free(q);
        return NULL;
    }

    /* Bus mastering and memory space, or the device cannot DMA the rings. */
    pci_cmd_t cmd = 0;
    if (pci_device_read_cmd(bdf, &cmd) == PCI_ERR_OK) {
        pci_cmd_t after = 0;
        (void)pci_device_write_cmd(q->hdl,
                                   (pci_cmd_t)(cmd | PGW_PCI_CMD_MEM |
                                               PGW_PCI_CMD_MASTER),
                                   &after);
    }
    qnx_map_bars(q);

    pgw_pci_cfg_io cfg = {qnx_cfg_read, qnx_cfg_write, q};
    pgw_pci_bar_io bars = {qnx_bar_r32, qnx_bar_w32, q};
    pgw_vio_dev *dev = pgw_vio_pci_sim_open(device_id, &cfg, bars,
                                            expected_tag, false);
    if (dev == NULL) {
        qnx_unmap_bars(q);
        pci_device_detach(q->hdl);
        free(q);
        return NULL;
    }
    dev->vq_alloc = pgw_qnx_vq_alloc;
    dev->vq_free = pgw_qnx_vq_free;
    dev->irq_release = qnx_pci_teardown;

    if (allow_msix) {
        /* x86 APIC delivery: destination 0, vector taken from the PCI server. */
        uint32_t vector = 0;
        if (pgw_vio_pci_sim_msix(dev, 0xFEE00000ULL, vector) != PGW_OK)
            PGW_WARN("virtio-pci: MSI-X unavailable, using INTx");
    }
    if (!dev->info.msix_active) {
        if (!qnx_irq_attach(q)) {
            pgw_vio_destroy(dev);
            return NULL;
        }
        dev->irq_attached = true;
    }
    return dev;
}

#endif /* __QNX__ */

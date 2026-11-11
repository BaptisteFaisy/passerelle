/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/src/virtio_priv.h - internal layout shared by the virtio backends.
 *
 * The public contract lives in pgw/virtio.h and is not duplicated here: this
 * header only describes what the backends (mock, MMIO, PCI) and the split
 * virtqueue need to share.
 *
 * It covers:
 *   VIO-03 - one ring layout that both MMIO v1 (contiguous, page aligned) and
 *            MMIO v2 / PCI modern (three independent rings) can use;
 *   VIO-07 - descriptors point straight at caller memory, no bounce buffer;
 *   VIO-08 - indirect descriptor pool and the EVENT_IDX / NO_NOTIFY knobs;
 *   VIO-10 - the mock transport drives the very same virtqueue code as a real
 *            device would, so the whole stack is testable on the PC.
 *
 * Nothing here allocates on the submission path: every buffer is sized when
 * the queue is created.
 *
 * Lifetime rule: destroy every queue of a device before the device itself, so
 * that pgw_vq_destroy() can still unregister the queue from its backend.
 */
#ifndef PGW_VIRTIO_PRIV_H
#define PGW_VIRTIO_PRIV_H

#include "pgw/virtio.h"

#include "pgw/limits.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Ring memory is page aligned so that a legacy MMIO queue fits in one
 * contiguous block (VIO-03); modern transports simply get three such blocks. */
#define PGW_VQ_ALIGN 4096u

/* One indirect table per in-flight request at most, and never more tables than
 * descriptors: 16 tables of PGW_SG_MAX entries cover a 128 descriptor queue. */
#define PGW_VQ_INDIRECT_TABLES 16u
#define PGW_VQ_NO_DESC 0xFFFFu

/* Descriptor flags (virtio 1.2 §2.6.5). */
#define PGW_VRING_DESC_F_NEXT     0x0001u
#define PGW_VRING_DESC_F_WRITE    0x0002u
#define PGW_VRING_DESC_F_INDIRECT 0x0004u

/* Ring flags: the driver asks the device not to interrupt, the device asks the
 * driver not to notify (virtio 1.2 §2.6.7, VIO-08). */
#define PGW_VRING_AVAIL_F_NO_INTERRUPT 0x0001u
#define PGW_VRING_USED_F_NO_NOTIFY     0x0001u

typedef struct pgw_vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} pgw_vring_desc;

typedef struct pgw_vring_used_elem {
    uint32_t id;
    uint32_t len;
} pgw_vring_used_elem;

/* One request as the *device* sees it: readable buffers first, then writable
 * ones, in chain order (VIO-07). */
typedef struct pgw_vq_req {
    pgw_vio_sg in[PGW_SG_MAX];
    size_t n_in;
    pgw_vio_sg out[PGW_SG_MAX];
    size_t n_out;
    uint16_t head;
} pgw_vq_req;

/* Ring allocation hook. The PC uses aligned_alloc; the QNX glue overrides it
 * with a physically contiguous allocation so that a legacy MMIO PFN is real. */
typedef void *(*pgw_vq_mem_alloc_fn)(pgw_vio_dev *dev, size_t bytes,
                                     size_t align, uint64_t *out_pa);
typedef void (*pgw_vq_mem_free_fn)(pgw_vio_dev *dev, void *ptr, size_t bytes);

struct pgw_virtq {
    pgw_vio_dev *dev;
    uint16_t qidx;
    uint16_t size;
    bool indirect;  /* indirect descriptors negotiated and wanted (VIO-08) */
    bool event_idx; /* VIRTIO_RING_F_EVENT_IDX negotiated (VIO-08) */
    bool legacy;    /* MMIO v1: one contiguous block, PFN addressed */

    /* Split ring memory. `avail_mem` and `used_mem` are raw bytes so that the
     * accessors below stay free of struct-packing assumptions. */
    pgw_vring_desc *desc;
    uint8_t *avail_mem;
    size_t avail_bytes;
    uint8_t *used_mem;
    size_t used_bytes;
    void *block;      /* single legacy block, NULL on modern transports */
    size_t block_bytes;

    uint64_t desc_pa;
    uint64_t avail_pa;
    uint64_t used_pa;

    /* Driver side cursors. */
    uint16_t last_used;
    uint16_t last_kick;
    uint16_t num_free;
    uint16_t free_head;
    uint16_t *free_next;
    void **cookies;        /* per head descriptor: caller cookie (VIO-07) */
    uint16_t *table_owner; /* per head descriptor: indirect table or NO_DESC */

    /* Device side cursor, used by the mock transport (VIO-10). */
    uint16_t dev_last_avail;

    /* Indirect descriptor pool (VIO-08). */
    pgw_vring_desc *tables;
    uint32_t table_mask; /* bit set = table in use */
};

struct pgw_vio_dev {
    const pgw_vio_bus_ops *ops;
    void *backend;
    pgw_vio_info info;
    uint64_t host_features;
    bool irq_attached;
    pgw_vq_mem_alloc_fn vq_alloc;
    pgw_vq_mem_free_fn vq_free;
    /* Optional backend teardown run by pgw_vio_reset() after the bus ops: the
     * QNX glue uses it to detach its interrupt thread (VIO-09). */
    void (*irq_release)(pgw_vio_dev *dev);
};

/* Ring sizes in bytes: flags+idx+ring[] plus the EVENT_IDX slot (virtio 1.2
 * §2.6.6 and §2.6.8). Shared by the allocator and the accessors below. */
static inline size_t pgw_vq_avail_bytes(uint16_t size)
{
    return 4u + 2u * (size_t)size + 2u;
}

static inline size_t pgw_vq_used_bytes(uint16_t size)
{
    return 4u + 8u * (size_t)size + 2u;
}

/* --- ring accessors ------------------------------------------------------- */

static inline uint16_t *pgw_vq_avail_flags(pgw_virtq *vq)
{
    return (uint16_t *)(void *)vq->avail_mem;
}

static inline uint16_t *pgw_vq_avail_idx(pgw_virtq *vq)
{
    return (uint16_t *)(void *)(vq->avail_mem + 2);
}

static inline uint16_t *pgw_vq_avail_ring(pgw_virtq *vq)
{
    return (uint16_t *)(void *)(vq->avail_mem + 4);
}

static inline uint16_t *pgw_vq_used_event(pgw_virtq *vq)
{
    return (uint16_t *)(void *)(vq->avail_mem + 4 + 2u * (size_t)vq->size);
}

static inline uint16_t *pgw_vq_used_flags(pgw_virtq *vq)
{
    return (uint16_t *)(void *)vq->used_mem;
}

static inline uint16_t *pgw_vq_used_idx(pgw_virtq *vq)
{
    return (uint16_t *)(void *)(vq->used_mem + 2);
}

static inline pgw_vring_used_elem *pgw_vq_used_ring(pgw_virtq *vq)
{
    return (pgw_vring_used_elem *)(void *)(vq->used_mem + 4);
}

static inline uint16_t *pgw_vq_avail_event(pgw_virtq *vq)
{
    return (uint16_t *)(void *)(vq->used_mem + 4 +
                                8u * (size_t)vq->size);
}

/* --- device side helpers (used by the mock backend, VIO-10) --------------- */

/* Take the next available chain, if any. `*got` is false when the driver has
 * published nothing new. */
pgw_err pgw_vq_take_avail(pgw_virtq *vq, pgw_vq_req *req, bool *got);

/* Publish a completed chain in the used ring. */
pgw_err pgw_vq_complete(pgw_virtq *vq, uint16_t head, uint32_t written);

/* Run `fn` over every available chain; returns how many were completed. */
size_t pgw_vq_mock_process(pgw_virtq *vq, pgw_vio_mock_fn fn, void *user);

/* --- mock transport (VIO-10) ---------------------------------------------- */

/* Drop a queue from the mock device's table before it is freed. */
void pgw_vio_mock_forget(pgw_vio_dev *dev, pgw_virtq *vq);

/* Test knobs: how the simulated device behaves and what it recorded. */
void pgw_vio_mock_set_sync(pgw_vio_dev *dev, bool sync);
uint32_t pgw_vio_mock_notify_count(const pgw_vio_dev *dev);
uint32_t pgw_vio_mock_reset_count(const pgw_vio_dev *dev);
uint32_t pgw_vio_mock_teardown_count(const pgw_vio_dev *dev);

/* Let the simulated device suppress notifications: set the used-ring flags and
 * the avail_event the driver must compare against (VIO-08). */
pgw_err pgw_vio_mock_queue_flags(pgw_vio_dev *dev, uint16_t qidx, uint16_t flags,
                                 uint16_t avail_event);

/* --- MMIO backend --------------------------------------------------------- */

/* Largest queue the device accepts, 0 when the backend is not MMIO. */
uint16_t pgw_vio_mmio_queue_max(pgw_vio_dev *dev, uint16_t qidx);

/* --- PCI backend ---------------------------------------------------------- */

/* PCI configuration space accessor: what a bus driver provides, nothing more.
 * Keeping discovery on top of this shape is what makes the whole modern
 * capability walk testable on the PC with a RAM-backed config space (VIO-02). */
typedef struct pgw_pci_cfg_io {
    pgw_err (*read)(void *ctx, uint16_t off, void *out, size_t len);
    pgw_err (*write)(void *ctx, uint16_t off, const void *in, size_t len);
    void *ctx;
} pgw_pci_cfg_io;

/* BAR accessor (32-bit registers, like MMIO). */
typedef struct pgw_pci_bar_io {
    uint32_t (*r32)(void *ctx, uint8_t bar, uint32_t off);
    void (*w32)(void *ctx, uint8_t bar, uint32_t off, uint32_t val);
    void *ctx;
} pgw_pci_bar_io;

/* The five modern capability types of virtio 1.2 §4.1.4 (VIO-02). */
#define PGW_PCI_CAP_COMMON_CFG   1u
#define PGW_PCI_CAP_NOTIFY_CFG   2u
#define PGW_PCI_CAP_ISR_CFG      3u
#define PGW_PCI_CAP_DEVICE_CFG   4u
#define PGW_PCI_CAP_PCI_CFG      5u

#define PGW_PCI_CAP_ID_VENDOR 0x09u
#define PGW_PCI_CAP_ID_MSIX   0x11u

typedef struct pgw_pci_cap {
    bool present;
    uint8_t bar;
    uint32_t offset;
    uint32_t length;
    uint32_t multiplier; /* notify_off_multiplier, notify capability only */
} pgw_pci_cap;

typedef struct pgw_pci_caps {
    uint16_t vendor_id;
    uint16_t device_id;
    pgw_pci_cap common;
    pgw_pci_cap notify;
    pgw_pci_cap isr;
    pgw_pci_cap device;
    pgw_pci_cap pci_cfg;
    uint16_t msix_off; /* configuration offset of the MSI-X capability, 0 if none */
} pgw_pci_caps;

/* Walk the capability list. Returns PGW_ENOENT when the device has no
 * capability list at all, PGW_EIO when the list loops or is malformed. */
pgw_err pgw_pci_scan_caps(const pgw_pci_cfg_io *io, pgw_pci_caps *out);

/* Largest queue the device accepts, 0 when the backend is not PCI. */
uint16_t pgw_vio_pci_queue_max(pgw_vio_dev *dev, uint16_t qidx);

/* Program MSI-X vector 0 with a delivery address the caller owns (the QNX glue
 * knows the APIC vector; the PC test uses the default one). */
pgw_err pgw_vio_pci_sim_msix(pgw_vio_dev *dev, uint64_t msg_addr,
                             uint32_t msg_data);

/* Open a PCI modern device from an abstract configuration space and BAR
 * accessor. `pgw_vio_pci_open()` is this function bound to the real bus. */
pgw_vio_dev *pgw_vio_pci_sim_open(uint16_t device_id,
                                  const pgw_pci_cfg_io *cfg,
                                  pgw_pci_bar_io bars,
                                  const char *expected_tag, bool allow_msix);

/* --- QNX only (built only if(QNX)) ---------------------------------------- */

#if defined(__QNX__)
/* Real bus: libpci probe plus mmap_device_memory of the BARs. */
pgw_vio_dev *pgw_vio_mmio_open_phys(uint64_t paddr, int32_t irq,
                                    const char *expected_tag);

/* Helpers of qnx_mem.c, shared by both bindings. */
void *pgw_qnx_vq_alloc(pgw_vio_dev *dev, size_t bytes, size_t align,
                       uint64_t *out_pa);
void pgw_qnx_vq_free(pgw_vio_dev *dev, void *ptr, size_t bytes);
int32_t pgw_qnx_irq_attach(int32_t irq, int32_t prio);
void pgw_qnx_irq_detach(int32_t *irq_id);
#endif

#endif /* PGW_VIRTIO_PRIV_H */

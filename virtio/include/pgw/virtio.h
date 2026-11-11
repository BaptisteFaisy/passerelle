/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pgw/virtio.h - Layer 1 (virtio transport).
 *
 * Implements requirements VIO-01 through VIO-10:
 *   - PCI modern capability discovery + MSI-X/INTx wiring (QNX 8.0)
 *   - MMIO v1 and v2 discovery and transport
 *   - Split virtqueue with indirect descriptors (VIO-08) and zero-copy
 *     scatter-gather buffers (VIO-07)
 *   - Notification suppression (EVENT_IDX / NO_NOTIFY)
 *   - Device tag reading and selection (VIO-05)
 *   - Clean reset & interrupt teardown (VIO-09)
 *   - Mock in-memory transport so every protocol and driver test runs on PC
 */
#ifndef PGW_VIRTIO_H
#define PGW_VIRTIO_H

#include "pgw/err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Virtio device status bits (spec §2.1). */
#define PGW_VIO_STATUS_ACK        0x01u
#define PGW_VIO_STATUS_DRIVER     0x02u
#define PGW_VIO_STATUS_DRIVER_OK  0x04u
#define PGW_VIO_STATUS_FEATURES   0x08u
#define PGW_VIO_STATUS_NEEDS_RST  0x40u
#define PGW_VIO_STATUS_FAILED     0x80u

/* Common virtio feature bits (spec §6). */
#define PGW_VIO_F_RING_INDIRECT_DESC (1ULL << 28)
#define PGW_VIO_F_RING_EVENT_IDX     (1ULL << 29)
#define PGW_VIO_F_VERSION_1          (1ULL << 32)

/* Device-specific feature bits. */
#define PGW_VIO_9P_F_MOUNT_TAG       (1ULL << 0)

/* Scatter-gather element pointing directly into caller memory (VIO-07). */
typedef struct pgw_vio_sg {
    void *addr;
    uint64_t phys; /* physical/DMA address if pre-translated, else 0 */
    uint32_t len;
    bool write;    /* true = device writes into buffer; false = device reads */
} pgw_vio_sg;

typedef struct pgw_vio_dev pgw_vio_dev;
typedef struct pgw_virtq pgw_virtq;

/* Bus operations implemented by pci, mmio, or mock backends. */
typedef struct pgw_vio_bus_ops {
    void (*destroy)(pgw_vio_dev *dev);
    void (*reset)(pgw_vio_dev *dev);
    uint8_t (*get_status)(pgw_vio_dev *dev);
    void (*set_status)(pgw_vio_dev *dev, uint8_t status);
    uint64_t (*get_features)(pgw_vio_dev *dev);
    void (*set_features)(pgw_vio_dev *dev, uint64_t features);
    pgw_err (*read_config)(pgw_vio_dev *dev, uint32_t offset, void *out, size_t len);
    pgw_err (*setup_queue)(pgw_vio_dev *dev, uint16_t qidx, pgw_virtq *vq);
    void (*notify_queue)(pgw_vio_dev *dev, uint16_t qidx);
    pgw_err (*wait_irq)(pgw_vio_dev *dev, int32_t timeout_ms);
    void (*teardown_irq)(pgw_vio_dev *dev);
} pgw_vio_bus_ops;

/* Device identity + negotiated capabilities. */
typedef struct pgw_vio_info {
    uint16_t device_id;    /* 9 (9P) or 26 (virtiofs) */
    uint16_t vendor_id;
    uint32_t bus_kind;     /* 0 = mock, 1 = PCI modern, 2 = MMIO v1/v2 */
    uint32_t mmio_version; /* 1 or 2 when bus_kind == 2 */
    uint16_t num_queues;
    uint64_t negotiated_features;
    bool msix_active;
    char tag[64];          /* mount tag (VIO-05, VFS-01) */
} pgw_vio_info;

/* --- core lifecycle ------------------------------------------------------- */

pgw_vio_dev *pgw_vio_create(const pgw_vio_bus_ops *ops, void *backend,
                            uint16_t device_id, uint32_t bus_kind);
void pgw_vio_destroy(pgw_vio_dev *dev); /* resets + tears down IRQ (VIO-09) */
void *pgw_vio_backend(pgw_vio_dev *dev);
const pgw_vio_info *pgw_vio_get_info(const pgw_vio_dev *dev);
pgw_vio_info *pgw_vio_info_mut(pgw_vio_dev *dev);

/* Negotiate features per virtio spec §3.1 and move to DRIVER_OK. */
pgw_err pgw_vio_init_device(pgw_vio_dev *dev, uint64_t driver_features);
void pgw_vio_reset(pgw_vio_dev *dev);
pgw_err pgw_vio_read_tag_9p(pgw_vio_dev *dev, char *out, size_t cap);
pgw_err pgw_vio_read_tag_fs(pgw_vio_dev *dev, char *out, size_t cap,
                            uint32_t *out_req_queues);

/* --- split virtqueue (pgw_virtq) ------------------------------------------ */

pgw_virtq *pgw_vq_create(pgw_vio_dev *dev, uint16_t qidx, uint16_t size,
                         bool indirect, bool event_idx);
void pgw_vq_destroy(pgw_virtq *vq);

uint16_t pgw_vq_size(const pgw_virtq *vq);
uint16_t pgw_vq_free_descs(const pgw_virtq *vq);

/* Add a scatter-gather request (zero-copy). `cookie` is returned unchanged
 * from pgw_vq_pop(). Uses indirect descriptor table when total > threshold
 * and indirect feature was negotiated (VIO-08). */
pgw_err pgw_vq_push(pgw_virtq *vq, const pgw_vio_sg *sg, size_t count,
                    void *cookie);

/* Notify device only if suppression flags allow it (VIO-08). */
bool pgw_vq_kick(pgw_virtq *vq);

/* Pop the next completed buffer, storing its cookie and bytes written.
 * Returns PGW_EAGAIN when the used ring is empty. */
pgw_err pgw_vq_pop(pgw_virtq *vq, void **out_cookie, uint32_t *out_len);

/* Raw ring pointers for bus setup (PCI common_cfg / MMIO queue registers). */
void pgw_vq_rings(const pgw_virtq *vq, uint64_t *desc_pa, uint64_t *avail_pa,
                  uint64_t *used_pa);

/* --- mock transport (PC testing, VIO-10) ---------------------------------- */

/* Device handler callback invoked synchronously or on kick when running a
 * simulated device. `in_sg` has device-readable buffers; `out_sg` has
 * device-writable buffers. Return total bytes written into `out_sg`. */
typedef uint32_t (*pgw_vio_mock_fn)(const pgw_vio_sg *in_sg, size_t n_in,
                                    const pgw_vio_sg *out_sg, size_t n_out,
                                    void *user);

pgw_vio_dev *pgw_vio_mock_open(uint16_t device_id, const char *tag,
                               uint16_t num_queues, uint64_t host_features,
                               pgw_vio_mock_fn handler, void *user);

/* Let tests simulate an asynchronous backend: process queued buffers. */
size_t pgw_vio_mock_step(pgw_vio_dev *dev, uint16_t qidx);

/* --- PCI and MMIO discovery (QNX 8.0 / simulated bus) --------------------- */

/* Abstract bus I/O for MMIO so unit tests can back MMIO registers with RAM. */
typedef struct pgw_mmio_io {
    uint32_t (*r32)(void *ctx, uint32_t off);
    void (*w32)(void *ctx, uint32_t off, uint32_t val);
    void *ctx;
} pgw_mmio_io;

/* Open an MMIO device from a register accessor (v1 or v2, VIO-03). */
pgw_vio_dev *pgw_vio_mmio_open(pgw_mmio_io io, const char *expected_tag);

/* Probe PCI on QNX 8.0 (libpci) for a virtio device matching device_id and
 * optional mount tag (VIO-01, VIO-02, VIO-04, VIO-05). On non-QNX host builds
 * returns NULL (ENODEV). */
pgw_vio_dev *pgw_vio_pci_open(uint16_t device_id, const char *expected_tag,
                              bool allow_msix);

#ifdef __cplusplus
}
#endif

#endif /* PGW_VIRTIO_H */

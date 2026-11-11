/* SPDX-License-Identifier: Apache-2.0 */
/*
 * clab_pci.h - chapter 3's exercise: decode virtio PCI capabilities.
 *
 * A modern virtio PCI device does not put its registers at fixed offsets. It
 * publishes vendor-specific PCI capabilities (virtio 1.2, 4.1.4), each naming a
 * BAR, an offset inside it and a length:
 *
 *   cfg_type 1  COMMON   features, device status, per-queue setup
 *   cfg_type 2  NOTIFY   where to write a queue index to ring the doorbell
 *   cfg_type 3  ISR      one byte, read to acknowledge an INTx interrupt
 *   cfg_type 4  DEVICE   device-specific config (for 9P: the mount tag)
 *   cfg_type 5  PCI      an alternate window into config space
 *
 * This file holds only the arithmetic. Config space is read through a callback,
 * which is what makes the exercise testable on a PC with no device at all: the
 * test supplies 256 bytes of RAM and answers the reads. In the real driver
 * (virtio/src/virtio_pci_caps.c) the callback is libpci.
 */
#ifndef CLAB_PCI_H
#define CLAB_PCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLAB_PCI_CAP_VENDOR 0x09u
#define CLAB_PCI_CAP_MSIX   0x11u

#define CLAB_VIRTIO_CFG_COMMON 1u
#define CLAB_VIRTIO_CFG_NOTIFY 2u
#define CLAB_VIRTIO_CFG_ISR    3u
#define CLAB_VIRTIO_CFG_DEVICE 4u
#define CLAB_VIRTIO_CFG_PCI    5u

/* One decoded capability. */
typedef struct {
    uint8_t cfg_type;
    uint8_t bar;
    uint32_t offset;
    uint32_t length;
    uint32_t notify_mult; /* NOTIFY only */
} clab_pci_cap;

typedef struct {
    clab_pci_cap common;
    clab_pci_cap notify;
    clab_pci_cap isr;
    clab_pci_cap device;
    clab_pci_cap pci;
    bool have_common;
    bool have_notify;
    bool have_isr;
    bool have_device;
    bool have_pci;
} clab_pci_caps;

/* Reads `len` bytes of config space at `off`. Returns 0, or -1 on failure. */
typedef int (*clab_cfg_read)(void *ctx, uint32_t off, void *dst, size_t len);

/* Walk the capability linked list, which starts at offset 0x34. */
int clab_pci_scan_chain(clab_cfg_read read, void *ctx, clab_pci_caps *out);

/* Sweep 0x40..0xFF with 32-bit reads only, for hosts that cannot read 0x34. */
int clab_pci_scan_linear(clab_cfg_read read, void *ctx, clab_pci_caps *out);

/* Chain first, linear sweep if the chain did not produce a usable set. */
int clab_pci_scan(clab_cfg_read read, void *ctx, clab_pci_caps *out);

/* COMMON and NOTIFY are the two a driver cannot work without. */
bool clab_pci_usable(const clab_pci_caps *caps);

/* Where to write the queue index to notify the device (virtio 1.2, 4.1.4.4). */
uint32_t clab_pci_notify_addr(const clab_pci_caps *caps, uint16_t notify_off);

/* Largest power of two <= min(device_max, wanted), never below 2. */
uint16_t clab_pci_queue_size(uint16_t device_max, uint16_t wanted);

/* Human-readable name of a cfg_type, for the exercise's output. */
const char *clab_pci_cfg_name(uint8_t cfg_type);

#endif /* CLAB_PCI_H */

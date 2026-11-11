/* SPDX-License-Identifier: Apache-2.0 */
/*
 * clab_pci.c - chapter 3's exercise, implementation.
 *
 * See clab_pci.h for what a virtio PCI capability is. Two things here are worth
 * reading twice:
 *
 *  - the chain walk needs a read at offset 0x34, which is below 0x40. QNX's
 *    pci_device_cfg_rd*() cannot do that, which is why the linear sweep exists;
 *  - the linear sweep must skip the interior of each capability it recognises,
 *    otherwise a length or offset field that happens to start with 0x09 is
 *    decoded as a second capability. Test case 5 in tests/test_clab.c is
 *    exactly that trap.
 */
#include "clab_pci.h"

#include <string.h>

#define CHAIN_MAX 48u  /* a malformed chain must not hang the driver */
#define CAP_MIN_LEN 16u /* 4-byte PCI header + bar + 3 pad + offset + length */

static void store(clab_pci_caps *out, const clab_pci_cap *cap)
{
    switch (cap->cfg_type) {
    case CLAB_VIRTIO_CFG_COMMON:
        out->common = *cap;
        out->have_common = true;
        break;
    case CLAB_VIRTIO_CFG_NOTIFY:
        out->notify = *cap;
        out->have_notify = true;
        break;
    case CLAB_VIRTIO_CFG_ISR:
        out->isr = *cap;
        out->have_isr = true;
        break;
    case CLAB_VIRTIO_CFG_DEVICE:
        out->device = *cap;
        out->have_device = true;
        break;
    case CLAB_VIRTIO_CFG_PCI:
        out->pci = *cap;
        out->have_pci = true;
        break;
    default:
        break; /* an unknown type must be ignored, not rejected */
    }
}

/* Decode one vendor-specific capability at `off`; hdr is its first 4 bytes. */
static void decode_vendor(clab_cfg_read read, void *ctx, uint32_t off,
                          const uint8_t *hdr, clab_pci_caps *out)
{
    clab_pci_cap cap;
    uint32_t bar_word = 0;
    uint32_t mult = 0;

    if (hdr[2] < CAP_MIN_LEN || hdr[3] < CLAB_VIRTIO_CFG_COMMON ||
        hdr[3] > CLAB_VIRTIO_CFG_PCI)
        return;
    if (read(ctx, off + 4u, &bar_word, sizeof bar_word) != 0)
        return;
    memset(&cap, 0, sizeof cap);
    cap.cfg_type = hdr[3];
    cap.bar = (uint8_t)(bar_word & 0xFFu);
    if (read(ctx, off + 8u, &cap.offset, sizeof cap.offset) != 0)
        return;
    if (read(ctx, off + 12u, &cap.length, sizeof cap.length) != 0)
        return;
    if (cap.cfg_type == CLAB_VIRTIO_CFG_NOTIFY && hdr[2] >= 20u) {
        if (read(ctx, off + 16u, &mult, sizeof mult) != 0)
            return;
        cap.notify_mult = mult;
    }
    if (cap.bar > 5u || cap.length == 0u)
        return;
    store(out, &cap);
}

int clab_pci_scan_chain(clab_cfg_read read, void *ctx, clab_pci_caps *out)
{
    uint16_t status = 0;
    uint8_t off = 0;

    if (read == NULL || out == NULL)
        return -1;
    memset(out, 0, sizeof *out);
    if (read(ctx, 0x06u, &status, sizeof status) != 0)
        return -1;
    if ((status & 0x10u) == 0u)
        return 0; /* bit 4 of the status word: no capability list */
    if (read(ctx, 0x34u, &off, sizeof off) != 0)
        return -1;

    for (unsigned hops = 0; hops < CHAIN_MAX && off >= 0x40u; hops++) {
        uint8_t hdr[4];
        if (read(ctx, off, hdr, sizeof hdr) != 0 || hdr[0] == 0u)
            break;
        if (hdr[0] == CLAB_PCI_CAP_VENDOR)
            decode_vendor(read, ctx, off, hdr, out);
        if (hdr[1] == 0u || hdr[1] == off)
            break; /* end of list, or a self-loop */
        off = hdr[1];
    }
    return 0;
}

int clab_pci_scan_linear(clab_cfg_read read, void *ctx, clab_pci_caps *out)
{
    if (read == NULL || out == NULL)
        return -1;
    memset(out, 0, sizeof *out);

    for (uint32_t off = 0x40u; off <= 0xFCu; off += 4u) {
        uint32_t word = 0;
        uint8_t hdr[4];
        if (read(ctx, off, &word, sizeof word) != 0)
            return -1;
        hdr[0] = (uint8_t)(word & 0xFFu);
        hdr[1] = (uint8_t)((word >> 8u) & 0xFFu);
        hdr[2] = (uint8_t)((word >> 16u) & 0xFFu);
        hdr[3] = (uint8_t)((word >> 24u) & 0xFFu);
        if (hdr[0] != CLAB_PCI_CAP_VENDOR)
            continue;
        decode_vendor(read, ctx, off, hdr, out);
        if (hdr[2] >= CAP_MIN_LEN)
            off += ((uint32_t)hdr[2] + 3u) / 4u * 4u - 4u;
    }
    return 0;
}

int clab_pci_scan(clab_cfg_read read, void *ctx, clab_pci_caps *out)
{
    clab_pci_caps chain;

    if (clab_pci_scan_chain(read, ctx, &chain) == 0 && clab_pci_usable(&chain)) {
        *out = chain;
        return 0;
    }
    return clab_pci_scan_linear(read, ctx, out);
}

bool clab_pci_usable(const clab_pci_caps *caps)
{
    return caps != NULL && caps->have_common && caps->have_notify;
}

uint32_t clab_pci_notify_addr(const clab_pci_caps *caps, uint16_t notify_off)
{
    if (caps == NULL || !caps->have_notify)
        return 0;
    return caps->notify.offset + (uint32_t)notify_off * caps->notify.notify_mult;
}

uint16_t clab_pci_queue_size(uint16_t device_max, uint16_t wanted)
{
    uint16_t size = wanted < device_max ? wanted : device_max;

    if (size < 2u)
        return 2u;
    while ((size & (uint16_t)(size - 1u)) != 0u)
        size = (uint16_t)(size & (uint16_t)(size - 1u));
    return size;
}

const char *clab_pci_cfg_name(uint8_t cfg_type)
{
    switch (cfg_type) {
    case CLAB_VIRTIO_CFG_COMMON:
        return "COMMON";
    case CLAB_VIRTIO_CFG_NOTIFY:
        return "NOTIFY";
    case CLAB_VIRTIO_CFG_ISR:
        return "ISR";
    case CLAB_VIRTIO_CFG_DEVICE:
        return "DEVICE";
    case CLAB_VIRTIO_CFG_PCI:
        return "PCI";
    default:
        return "?";
    }
}

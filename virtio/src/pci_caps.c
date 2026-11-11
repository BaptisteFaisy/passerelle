/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/src/pci_caps.c - modern PCI capability discovery, bus independent.
 *
 * Covers VIO-01 and VIO-02: a virtio 1.x PCI device advertises its registers
 * through vendor specific capabilities of type 1 (COMMON), 2 (NOTIFY), 3 (ISR),
 * 4 (DEVICE) and 5 (PCI_CFG) (virtio 1.2 §4.1.4). This file walks that list
 * over an abstract configuration-space accessor, so the walk is unit-tested on
 * the PC against a RAM-backed config space, with no libpci in sight.
 *
 * Everything read here is validated: capability length, capability type, BAR
 * index, and a hard bound on the number of hops so that a looping list cannot
 * hang the driver (ROB-06).
 */
#include "virtio_priv.h"

#include "pgw/log.h"

#include <string.h>

#define PGW_PCI_STATUS_CAP_LIST 0x0010u
#define PGW_PCI_CAP_PTR_OFF     0x34u
#define PGW_PCI_CAP_MIN_LEN     4u
#define PGW_PCI_CAP_VNDR_LEN    16u
#define PGW_PCI_CAP_NOTIFY_LEN  20u
#define PGW_PCI_CAP_MSIX_LEN    12u
#define PGW_PCI_CAP_MAX_HOPS    64u
#define PGW_PCI_CFG_MAX         256u

typedef struct cap_reader {
    const pgw_pci_cfg_io *io;
    bool fail;
} cap_reader;

static uint8_t rd8(cap_reader *r, uint16_t off)
{
    uint8_t v = 0;
    if (r->io->read(r->io->ctx, off, &v, 1u) != PGW_OK)
        r->fail = true;
    return v;
}

static uint16_t rd16(cap_reader *r, uint16_t off)
{
    uint8_t b[2] = {0, 0};
    if (r->io->read(r->io->ctx, off, b, sizeof(b)) != PGW_OK) {
        r->fail = true;
        return 0;
    }
    return (uint16_t)((uint16_t)b[0] | (uint16_t)((uint16_t)b[1] << 8));
}

static uint32_t rd32(cap_reader *r, uint16_t off)
{
    uint8_t b[4] = {0, 0, 0, 0};
    if (r->io->read(r->io->ctx, off, b, sizeof(b)) != PGW_OK) {
        r->fail = true;
        return 0;
    }
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static void set_cap(pgw_pci_cap *cap, uint8_t bar, uint32_t off, uint32_t len,
                    uint32_t multiplier)
{
    if (cap->present)
        return; /* first capability of a type wins, as the spec requires */
    cap->present = true;
    cap->bar = bar;
    cap->offset = off;
    cap->length = len;
    cap->multiplier = multiplier;
}

/* One vendor capability: cfg_type at +3, bar at +4, offset at +8, length at +12
 * (virtio 1.2 §4.1.4.1). */
static void read_vendor_cap(cap_reader *r, uint16_t ptr, uint8_t cap_len,
                            pgw_pci_caps *out)
{
    if (cap_len < PGW_PCI_CAP_VNDR_LEN)
        return;
    uint8_t cfg_type = rd8(r, (uint16_t)(ptr + 3u));
    uint8_t bar = rd8(r, (uint16_t)(ptr + 4u));
    uint32_t off = rd32(r, (uint16_t)(ptr + 8u));
    uint32_t len = rd32(r, (uint16_t)(ptr + 12u));
    if (r->fail)
        return;
    if (bar > 5u) {
        PGW_WARN("virtio-pci: capability at 0x%02x uses BAR %u", (unsigned)ptr,
                 (unsigned)bar);
        return;
    }
    uint32_t mult = 0;
    if (cfg_type == PGW_PCI_CAP_NOTIFY_CFG && cap_len >= PGW_PCI_CAP_NOTIFY_LEN)
        mult = rd32(r, (uint16_t)(ptr + 16u));

    switch (cfg_type) {
    case PGW_PCI_CAP_COMMON_CFG:
        set_cap(&out->common, bar, off, len, 0);
        break;
    case PGW_PCI_CAP_NOTIFY_CFG:
        set_cap(&out->notify, bar, off, len, mult);
        break;
    case PGW_PCI_CAP_ISR_CFG:
        set_cap(&out->isr, bar, off, len, 0);
        break;
    case PGW_PCI_CAP_DEVICE_CFG:
        set_cap(&out->device, bar, off, len, 0);
        break;
    case PGW_PCI_CAP_PCI_CFG:
        set_cap(&out->pci_cfg, bar, off, len, 0);
        break;
    default:
        PGW_DBG("virtio-pci: ignoring capability type %u", (unsigned)cfg_type);
        break;
    }
}

/* Walk the linked list, recording every capability type we understand. A list
 * that loops or points outside configuration space is refused (ROB-06). */
static pgw_err walk_caps(cap_reader *r, pgw_pci_caps *out)
{
    uint16_t ptr = rd8(r, PGW_PCI_CAP_PTR_OFF);
    if (r->fail)
        return PGW_EIO;
    if (ptr < 0x40u || ptr >= PGW_PCI_CFG_MAX) {
        PGW_ERR("virtio-pci: capability pointer 0x%02x is out of range",
                (unsigned)ptr);
        return PGW_EIO;
    }

    for (uint32_t hop = 0; hop < PGW_PCI_CAP_MAX_HOPS && ptr != 0u; hop++) {
        if (ptr >= PGW_PCI_CFG_MAX) {
            PGW_ERR("virtio-pci: capability link 0x%02x leaves config space",
                    (unsigned)ptr);
            return PGW_EIO;
        }
        uint8_t cap_id = rd8(r, ptr);
        uint8_t next = rd8(r, (uint16_t)(ptr + 1u));
        uint8_t cap_len = rd8(r, (uint16_t)(ptr + 2u));
        if (r->fail)
            return PGW_EIO;
        if (cap_len < PGW_PCI_CAP_MIN_LEN) {
            PGW_ERR("virtio-pci: capability 0x%02x has length %u",
                    (unsigned)cap_id, (unsigned)cap_len);
            return PGW_EIO;
        }
        if (cap_id == PGW_PCI_CAP_ID_VENDOR)
            read_vendor_cap(r, ptr, cap_len, out);
        else if (cap_id == PGW_PCI_CAP_ID_MSIX &&
                 cap_len >= PGW_PCI_CAP_MSIX_LEN && out->msix_off == 0u)
            out->msix_off = ptr;
        if (r->fail)
            return PGW_EIO;
        ptr = next;
    }
    return PGW_OK;
}

pgw_err pgw_pci_scan_caps(const pgw_pci_cfg_io *io, pgw_pci_caps *out)
{
    if (io == NULL || io->read == NULL || out == NULL)
        return PGW_EINVAL;
    memset(out, 0, sizeof(*out));

    cap_reader r = {io, false};
    out->vendor_id = rd16(&r, 0x00u);
    out->device_id = rd16(&r, 0x02u);
    uint16_t status = rd16(&r, 0x06u);
    if (r.fail)
        return PGW_EIO;
    if ((status & PGW_PCI_STATUS_CAP_LIST) == 0) {
        PGW_ERR("virtio-pci: device has no capability list");
        return PGW_ENOENT;
    }

    pgw_err e = walk_caps(&r, out);
    if (e != PGW_OK)
        return e;

    if (!out->common.present) {
        PGW_ERR("virtio-pci: no COMMON_CFG capability (not a modern device)");
        return PGW_ENOENT;
    }
    return PGW_OK;
}

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/src/pci_msix.c - the portable MSI-X programming (virtio 1.2 §4.1.5).
 *
 * It receives the message supplied by the platform. On QNX the PCI server owns
 * allocation; on the PC bench the caller supplies a deterministic APIC message.
 */
#include "virtio_priv.h"

#include "pgw/log.h"

#define PGW_MSIX_CTRL_ENABLE 0x8000u

static pgw_err vector0(pci_state *st, uint8_t *bar, uint32_t *off, uint16_t *ctrl)
{
    if (st->caps.msix_off == 0u)
        return PGW_ENOENT;
    *ctrl = st->cfg.read(st->cfg.ctx, (uint16_t)(st->caps.msix_off + 2u), (uint8_t *)ctrl,
                         sizeof(*ctrl)) == PGW_OK ?
                    *ctrl :
                    0u;
    uint16_t vectors = (uint16_t)((*ctrl & 0x07FFu) + 1u);
    if (vectors == 0u)
        return PGW_EIO;
    uint8_t table_bytes[4] = {0u, 0u, 0u, 0u};
    if (st->cfg.read(st->cfg.ctx, (uint16_t)(st->caps.msix_off + 4u), table_bytes,
                     sizeof(table_bytes)) != PGW_OK)
        return PGW_EIO;
    uint32_t table = (uint32_t)table_bytes[0] | ((uint32_t)table_bytes[1] << 8) |
                     ((uint32_t)table_bytes[2] << 16) | ((uint32_t)table_bytes[3] << 24);
    *bar = (uint8_t)(table & 0x07u);
    *off = table & ~0x07u;
    return (*bar <= 5u) ? PGW_OK : PGW_EIO;
}

pgw_err pgw_pci_msix_program(pgw_vio_dev *dev, uint64_t msg_addr, uint32_t msg_data)
{
    pci_state *st = pgw_pci_state(dev);
    if (st == NULL)
        return PGW_EINVAL;
    uint8_t bar = 0u;
    uint32_t off = 0u;
    uint16_t ctrl = 0u;
    pgw_err e = vector0(st, &bar, &off, &ctrl);
    if (e != PGW_OK)
        return e;
    st->bars.w32(st->bars.ctx, bar, off + 0u, (uint32_t)(msg_addr & 0xFFFFFFFFu));
    st->bars.w32(st->bars.ctx, bar, off + 4u, (uint32_t)(msg_addr >> 32));
    st->bars.w32(st->bars.ctx, bar, off + 8u, msg_data);
    st->bars.w32(st->bars.ctx, bar, off + 12u, 0u);
    uint16_t enabled = (uint16_t)(ctrl | PGW_MSIX_CTRL_ENABLE);
    (void)st->cfg.write(st->cfg.ctx, (uint16_t)(st->caps.msix_off + 2u),
                        (const uint8_t *)&enabled, sizeof(enabled));
    uint16_t after = 0u;
    if (st->cfg.read(st->cfg.ctx, (uint16_t)(st->caps.msix_off + 2u), (uint8_t *)&after,
                     sizeof(after)) != PGW_OK ||
        (after & PGW_MSIX_CTRL_ENABLE) == 0u)
        return PGW_EOPNOTSUPP;
    st->msix_enabled = true;
    uint32_t base = st->caps.common.offset + 0x10u;
    uint32_t word = st->bars.r32(st->bars.ctx, st->caps.common.bar, base & ~3u);
    uint32_t shift = 8u * (base & 3u);
    word = (word & ~(0xFFFFu << shift)) | (0u << shift);
    st->bars.w32(st->bars.ctx, st->caps.common.bar, base & ~3u, word);
    dev->info.msix_active = true;
    return PGW_OK;
}

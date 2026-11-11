/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ex02_pci_caps.c - chapter 3's exercise: print the capability table of a
 * synthetic virtio PCI device, then of one whose config space cannot be read
 * below 0x40 (the QNX case), and check that both scans agree.
 *
 * On a real machine the config-space callback would be libpci
 * (pci_device_cfg_rd32); here it is 256 bytes of RAM, which is enough to
 * develop and debug the decoder without a hypervisor.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "clab_check.h"
#include "clab_pci.h"

/* A 256-byte config space, optionally refusing reads below 0x40 the way QNX's
 * pci_device_cfg_rd*() does. */
typedef struct {
    uint8_t bytes[256];
    int deny_below_0x40;
} fake_cfg;

static int cfg_read(void *ctx, uint32_t off, void *dst, size_t len)
{
    fake_cfg *c = ctx;

    if (c->deny_below_0x40 && off < 0x40u)
        return -1;
    if ((size_t)off + len > sizeof c->bytes)
        return -1;
    memcpy(dst, c->bytes + off, len);
    return 0;
}

static void put32(fake_cfg *c, uint32_t off, uint32_t v)
{
    for (unsigned i = 0; i < 4u; i++)
        c->bytes[off + i] = (uint8_t)(v >> (8u * i));
}

/* One virtio capability: 4-byte PCI header, bar byte, 3 pad, offset, length,
 * then the notify multiplier for NOTIFY. */
static void put_cap(fake_cfg *c, uint32_t off, uint8_t next, uint8_t type,
                    uint8_t bar, uint32_t cap_off, uint32_t len, uint32_t mult)
{
    const uint8_t cap_len = type == CLAB_VIRTIO_CFG_NOTIFY ? 20u : 16u;

    c->bytes[off + 0u] = CLAB_PCI_CAP_VENDOR;
    c->bytes[off + 1u] = next;
    c->bytes[off + 2u] = cap_len;
    c->bytes[off + 3u] = type;
    put32(c, off + 4u, bar);
    put32(c, off + 8u, cap_off);
    put32(c, off + 12u, len);
    if (cap_len == 20u)
        put32(c, off + 16u, mult);
}

/* The device QEMU builds for -virtfs: four capabilities on BAR 4. */
static void build_device(fake_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->bytes[0x06] = 0x10u; /* status: capability list present */
    c->bytes[0x34] = 0x40u; /* first capability */
    put_cap(c, 0x40u, 0x50u, CLAB_VIRTIO_CFG_COMMON, 4u, 0x1000u, 0x1000u, 0u);
    put_cap(c, 0x50u, 0x64u, CLAB_VIRTIO_CFG_NOTIFY, 4u, 0x2000u, 0x1000u, 4u);
    put_cap(c, 0x64u, 0x74u, CLAB_VIRTIO_CFG_ISR, 4u, 0x3000u, 0x1000u, 0u);
    put_cap(c, 0x74u, 0x00u, CLAB_VIRTIO_CFG_DEVICE, 4u, 0x4000u, 0x1000u, 0u);
}

static void print_caps(const clab_pci_caps *caps)
{
    const clab_pci_cap *all[] = {&caps->common, &caps->notify, &caps->isr,
                                &caps->device, &caps->pci};
    const bool have[] = {caps->have_common, caps->have_notify, caps->have_isr,
                         caps->have_device, caps->have_pci};

    for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) {
        if (!have[i])
            continue;
        printf("  %-7s bar %u  offset 0x%04x  length 0x%04x", 
               clab_pci_cfg_name(all[i]->cfg_type), all[i]->bar, all[i]->offset,
               all[i]->length);
        if (all[i]->cfg_type == CLAB_VIRTIO_CFG_NOTIFY)
            printf("  multiplier %u", all[i]->notify_mult);
        printf("\n");
    }
}

static void compare(const clab_pci_caps *a, const clab_pci_caps *b)
{
    CLAB_CHECK_EQ(a->common.offset, b->common.offset);
    CLAB_CHECK_EQ(a->notify.offset, b->notify.offset);
    CLAB_CHECK_EQ(a->notify.notify_mult, b->notify.notify_mult);
    CLAB_CHECK_EQ(a->isr.offset, b->isr.offset);
    CLAB_CHECK_EQ(a->device.offset, b->device.offset);
}

int main(void)
{
    fake_cfg cfg;
    clab_pci_caps chain;
    clab_pci_caps linear;
    clab_pci_caps picked;

    printf("chapter 3 - virtio PCI capabilities\n");
    build_device(&cfg);

    printf("capability chain (from 0x34):\n");
    CLAB_CHECK_EQ(clab_pci_scan_chain(cfg_read, &cfg, &chain), 0);
    CLAB_CHECK(clab_pci_usable(&chain));
    print_caps(&chain);

    printf("linear sweep (0x40..0xFF, 32-bit reads):\n");
    CLAB_CHECK_EQ(clab_pci_scan_linear(cfg_read, &cfg, &linear), 0);
    print_caps(&linear);
    compare(&chain, &linear);

    /* Now the QNX case: 0x34 is unreadable. The chain scan fails, the fallback
     * has to find everything anyway. */
    cfg.deny_below_0x40 = 1;
    printf("same device, config space unreadable below 0x40:\n");
    CLAB_CHECK_EQ(clab_pci_scan_chain(cfg_read, &cfg, &chain), -1);
    CLAB_CHECK_EQ(clab_pci_scan(cfg_read, &cfg, &picked), 0);
    CLAB_CHECK(clab_pci_usable(&picked));
    print_caps(&picked);

    printf("notify address: queue 0 -> +0x%x, queue 3 -> +0x%x\n",
           clab_pci_notify_addr(&picked, 0), clab_pci_notify_addr(&picked, 3));
    CLAB_CHECK_EQ(clab_pci_notify_addr(&picked, 0), 0x2000);
    CLAB_CHECK_EQ(clab_pci_notify_addr(&picked, 3), 0x200C);

    printf("queue size: device max 1024, we want 128 -> %u; want 100 -> %u\n",
           clab_pci_queue_size(1024, 128), clab_pci_queue_size(1024, 100));
    CLAB_CHECK_EQ(clab_pci_queue_size(1024, 128), 128);
    CLAB_CHECK_EQ(clab_pci_queue_size(1024, 100), 64);

    return clab_result();
}

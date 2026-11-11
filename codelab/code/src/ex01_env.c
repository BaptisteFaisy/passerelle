/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ex01_env.c - chapter 1's exercise: check the assumptions the rest of the
 * tutorial makes, before trusting any of it.
 *
 * Build it and run it twice: once on the PC, once in the guest. Everything here
 * is a fact the driver will rely on, and a mismatch found now costs a minute
 * instead of an afternoon:
 *
 *  - the virtio PCI identity numbers (vendor 0x1AF4, 0x1009 transitional and
 *    0x1049 modern-only, which are 0x1000 + type and 0x1040 + type);
 *  - the sizes and signedness the wire format depends on (a 9P size field is
 *    32 bits, a descriptor's length 32 bits, its address 64);
 *  - that the platform is little-endian, which the ring layout assumes.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "clab_check.h"

/* virtio 1.2, 4.1.2 and 5.3: vendor 0x1AF4, device id = 0x1000 + type on the
 * transitional interface and 0x1040 + type on the modern-only one. Type 9 is
 * 9P; type 26 is virtiofs. */
#define CLAB_VIRTIO_VENDOR 0x1AF4u
#define CLAB_VIRTIO_TYPE_9P 9u
#define CLAB_VIRTIO_TYPE_FS 26u

static uint16_t pci_did_transitional(uint16_t type)
{
    return (uint16_t)(0x1000u + type);
}

static uint16_t pci_did_modern(uint16_t type)
{
    return (uint16_t)(0x1040u + type);
}

static void check_identity(void)
{
    CLAB_CHECK_EQ(pci_did_transitional(CLAB_VIRTIO_TYPE_9P), 0x1009);
    CLAB_CHECK_EQ(pci_did_modern(CLAB_VIRTIO_TYPE_9P), 0x1049);
    CLAB_CHECK_EQ(pci_did_modern(CLAB_VIRTIO_TYPE_FS), 0x105A);
    printf("virtio 9P:  vendor 0x%04X  pci id 0x%04X (transitional) 0x%04X (modern)\n",
           CLAB_VIRTIO_VENDOR, pci_did_transitional(CLAB_VIRTIO_TYPE_9P),
           pci_did_modern(CLAB_VIRTIO_TYPE_9P));
    printf("virtio-fs:  vendor 0x%04X  pci id 0x%04X (modern)\n", CLAB_VIRTIO_VENDOR,
           pci_did_modern(CLAB_VIRTIO_TYPE_FS));
}

static void check_types(void)
{
    CLAB_CHECK_EQ(sizeof(uint8_t), 1);
    CLAB_CHECK_EQ(sizeof(uint16_t), 2);
    CLAB_CHECK_EQ(sizeof(uint32_t), 4);
    CLAB_CHECK_EQ(sizeof(uint64_t), 8);
    CLAB_CHECK_EQ(sizeof(void *), 8); /* both QNX 8.0 targets are 64-bit */
    printf("types: u8=%zu u16=%zu u32=%zu u64=%zu ptr=%zu\n", sizeof(uint8_t),
           sizeof(uint16_t), sizeof(uint32_t), sizeof(uint64_t), sizeof(void *));
}

static void check_byte_order(void)
{
    const uint16_t probe = 0x0102u;
    unsigned char bytes[2];

    memcpy(bytes, &probe, sizeof bytes);
    printf("byte order: %s\n", bytes[0] == 0x02u ? "little-endian" : "big-endian");
    /* The split ring and every 9P integer are little-endian, so a big-endian
     * host would need byte swapping everywhere; this tutorial does not do it. */
    CLAB_CHECK(bytes[0] == 0x02u);
}

int main(void)
{
    printf("chapter 1 - environment check\n");
    check_identity();
    check_types();
    check_byte_order();
    return clab_result();
}

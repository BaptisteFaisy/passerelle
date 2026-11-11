/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_clab.c - the checks behind the codelab's "solution" sections.
 *
 * Every chapter ends with an exercise and a solution; this file is the
 * solution as a test, so a reader can see what "correct" means and can break
 * the code on purpose and watch a check fail. Each test function names the
 * chapter it belongs to.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "clab_check.h"
#include "clab_irq.h"
#include "clab_neg.h"
#include "clab_p9.h"
#include "clab_pci.h"
#include "clab_vring.h"

/* --- chapter 3: capability decoding -------------------------------------- */

typedef struct {
    uint8_t bytes[256];
    int deny_below_0x40;
} cfg_t;

static int cfg_read(void *ctx, uint32_t off, void *dst, size_t len)
{
    cfg_t *c = ctx;

    if (c->deny_below_0x40 && off < 0x40u)
        return -1;
    if ((size_t)off + len > sizeof c->bytes)
        return -1;
    memcpy(dst, c->bytes + off, len);
    return 0;
}

static void put32(cfg_t *c, uint32_t off, uint32_t v)
{
    for (unsigned i = 0; i < 4u; i++)
        c->bytes[off + i] = (uint8_t)(v >> (8u * i));
}

static void put_cap(cfg_t *c, uint32_t off, uint8_t next, uint8_t type, uint8_t bar,
                    uint32_t cap_off, uint32_t len, uint32_t mult)
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

static void device(cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->bytes[0x06] = 0x10u;
    c->bytes[0x34] = 0x40u;
    put_cap(c, 0x40u, 0x50u, CLAB_VIRTIO_CFG_COMMON, 4u, 0x1000u, 0x1000u, 0u);
    put_cap(c, 0x50u, 0x64u, CLAB_VIRTIO_CFG_NOTIFY, 4u, 0x2000u, 0x1000u, 4u);
    put_cap(c, 0x64u, 0x74u, CLAB_VIRTIO_CFG_ISR, 4u, 0x3000u, 0x1000u, 0u);
    put_cap(c, 0x74u, 0x00u, CLAB_VIRTIO_CFG_DEVICE, 4u, 0x4000u, 0x1000u, 0u);
}

static void test_ch3_chain(void)
{
    cfg_t cfg;
    clab_pci_caps caps;

    device(&cfg);
    CLAB_CHECK_EQ(clab_pci_scan_chain(cfg_read, &cfg, &caps), 0);
    CLAB_CHECK(clab_pci_usable(&caps));
    CLAB_CHECK_EQ(caps.common.bar, 4);
    CLAB_CHECK_EQ(caps.common.offset, 0x1000);
    CLAB_CHECK_EQ(caps.notify.notify_mult, 4);
    CLAB_CHECK_EQ(caps.device.offset, 0x4000);
}

static void test_ch3_cycle_and_missing(void)
{
    cfg_t cfg;
    clab_pci_caps caps;

    device(&cfg);
    cfg.bytes[0x75] = 0x40u; /* DEVICE -> COMMON: a cycle */
    CLAB_CHECK_EQ(clab_pci_scan_chain(cfg_read, &cfg, &caps), 0);
    CLAB_CHECK(clab_pci_usable(&caps));

    device(&cfg);
    cfg.bytes[0x50 + 3u] = CLAB_VIRTIO_CFG_ISR; /* no NOTIFY */
    CLAB_CHECK_EQ(clab_pci_scan_chain(cfg_read, &cfg, &caps), 0);
    CLAB_CHECK(!clab_pci_usable(&caps));
}

static void test_ch3_fallback_and_interior(void)
{
    cfg_t cfg;
    clab_pci_caps caps;

    device(&cfg);
    cfg.deny_below_0x40 = 1;
    CLAB_CHECK_EQ(clab_pci_scan_chain(cfg_read, &cfg, &caps), -1);
    CLAB_CHECK_EQ(clab_pci_scan(cfg_read, &cfg, &caps), 0);
    CLAB_CHECK(clab_pci_usable(&caps));

    device(&cfg);
    /* A dword inside NOTIFY's 20 bytes that looks like an ISR capability. */
    cfg.bytes[0x60] = CLAB_PCI_CAP_VENDOR;
    cfg.bytes[0x62] = 16u;
    cfg.bytes[0x63] = CLAB_VIRTIO_CFG_ISR;
    CLAB_CHECK_EQ(clab_pci_scan_linear(cfg_read, &cfg, &caps), 0);
    CLAB_CHECK_EQ(caps.isr.offset, 0x3000);
}

static void test_ch3_helpers(void)
{
    cfg_t cfg;
    clab_pci_caps caps;

    device(&cfg);
    CLAB_CHECK_EQ(clab_pci_scan_chain(cfg_read, &cfg, &caps), 0);
    CLAB_CHECK_EQ(clab_pci_notify_addr(&caps, 0), 0x2000);
    CLAB_CHECK_EQ(clab_pci_notify_addr(&caps, 3), 0x200C);
    CLAB_CHECK_EQ(clab_pci_queue_size(1024, 128), 128);
    CLAB_CHECK_EQ(clab_pci_queue_size(1024, 100), 64);
    CLAB_CHECK_EQ(clab_pci_queue_size(1, 16), 2);
}

/* --- chapter 4: negotiation ---------------------------------------------- */

static void test_ch4_sequence(void)
{
    const uint64_t wanted = CLAB_F_VERSION_1 | CLAB_F_MOUNT_TAG | CLAB_F_INDIRECT;
    clab_neg_dev dev;
    uint64_t got = 0;

    clab_neg_init(&dev, wanted);
    CLAB_CHECK_EQ(clab_neg_reset(&dev), CLAB_NEG_OK);
    CLAB_CHECK_EQ(clab_neg_acknowledge(&dev), CLAB_NEG_OK);
    CLAB_CHECK_EQ(dev.status, CLAB_STATUS_ACK | CLAB_STATUS_DRIVER);
    CLAB_CHECK_EQ(clab_neg_features(&dev, wanted, &got), CLAB_NEG_OK);
    CLAB_CHECK_EQ(got, wanted);
    CLAB_CHECK_EQ(clab_neg_ready(&dev), CLAB_NEG_OK);
    CLAB_CHECK_EQ(dev.status,
                  CLAB_STATUS_ACK | CLAB_STATUS_DRIVER | CLAB_STATUS_FEATURES_OK |
                      CLAB_STATUS_DRIVER_OK);
    CLAB_CHECK_EQ(clab_neg_check_health(&dev), CLAB_NEG_OK);
}

static void test_ch4_intersection_only(void)
{
    clab_neg_dev dev;
    uint64_t got = 0;

    /* The device offers less than the driver asks for: the driver must keep
     * only what was offered. */
    clab_neg_init(&dev, CLAB_F_VERSION_1 | CLAB_F_MOUNT_TAG);
    CLAB_CHECK_EQ(clab_neg_acknowledge(&dev), CLAB_NEG_OK);
    CLAB_CHECK_EQ(clab_neg_features(&dev, CLAB_F_VERSION_1 | CLAB_F_INDIRECT, &got),
                  CLAB_NEG_OK);
    CLAB_CHECK_EQ(got, CLAB_F_VERSION_1);
    CLAB_CHECK_EQ(dev.accepted, CLAB_F_VERSION_1);
}

static void test_ch4_failures(void)
{
    clab_neg_dev dev;
    uint64_t got = 0;

    clab_neg_init(&dev, CLAB_F_VERSION_1 | CLAB_F_MOUNT_TAG);
    dev.reject_features = true;
    CLAB_CHECK_EQ(clab_neg_acknowledge(&dev), CLAB_NEG_OK);
    CLAB_CHECK_EQ(clab_neg_features(&dev, CLAB_F_VERSION_1, &got),
                  CLAB_NEG_ERR_FEATURES);
    CLAB_CHECK_EQ(dev.status & CLAB_STATUS_FEATURES_OK, 0);
    /* DRIVER_OK without FEATURES_OK is refused, not silently accepted. */
    CLAB_CHECK_EQ(clab_neg_ready(&dev), CLAB_NEG_ERR_ORDER);

    /* No VIRTIO_F_VERSION_1: not a modern device. */
    clab_neg_init(&dev, CLAB_F_MOUNT_TAG);
    CLAB_CHECK_EQ(clab_neg_acknowledge(&dev), CLAB_NEG_OK);
    CLAB_CHECK_EQ(clab_neg_features(&dev, CLAB_F_VERSION_1, &got),
                  CLAB_NEG_ERR_UNSUPPORTED);
}

/* --- chapter 5: the virtqueue -------------------------------------------- */

static void test_ch5_round_trip(void)
{
    uint8_t arena[96];
    clab_vring vq;
    clab_vring_sg sg[2];
    uint16_t head = 0;
    uint32_t len = 0;

    memset(arena, 0, sizeof arena);
    memcpy(arena, "abc", 3);
    clab_vring_init(&vq, arena, sizeof arena, false);
    sg[0] = (clab_vring_sg){.off = 0, .len = 3, .writable = false};
    sg[1] = (clab_vring_sg){.off = 32, .len = 8, .writable = true};

    const int added = clab_vring_add(&vq, sg, 2);
    CLAB_CHECK(added >= 0);
    CLAB_CHECK_EQ(vq.num_free, CLAB_VRING_SIZE - 2u);
    clab_vring_publish(&vq);
    CLAB_CHECK_EQ(vq.avail.idx, 1);

    CLAB_CHECK_EQ(clab_vring_fake_device(&vq, (uint16_t)added), 3);
    CLAB_CHECK_EQ(clab_vring_pop(&vq, &head, &len), 1);
    CLAB_CHECK_EQ(head, (uint16_t)added);
    CLAB_CHECK_EQ(len, 3);
    CLAB_CHECK_MEM(arena + 32, "ABC", 3);

    clab_vring_recycle(&vq, head);
    CLAB_CHECK_EQ(vq.num_free, CLAB_VRING_SIZE);
    CLAB_CHECK_EQ(clab_vring_pop(&vq, &head, &len), 0);
}

static void test_ch5_indirect_and_full(void)
{
    uint8_t arena[96];
    clab_vring vq;
    clab_vring_sg sg[CLAB_VRING_SG_MAX];

    clab_vring_init(&vq, arena, sizeof arena, true);
    for (unsigned i = 0; i < CLAB_VRING_SG_MAX; i++)
        sg[i] = (clab_vring_sg){.off = i, .len = 1, .writable = (i % 2u) != 0u};

    /* One descriptor for the whole chain, because indirect is enabled. */
    CLAB_CHECK(clab_vring_add(&vq, sg, CLAB_VRING_SG_MAX) >= 0);
    CLAB_CHECK_EQ(vq.num_free, CLAB_VRING_SIZE - 1u);
    clab_vring_publish(&vq);

    /* Fill the ring: every add needs at least one descriptor. */
    unsigned added = 0;
    while (clab_vring_add(&vq, sg, 1) >= 0) {
        clab_vring_publish(&vq);
        added++;
    }
    CLAB_CHECK_EQ(added, CLAB_VRING_SIZE - 1u);
    CLAB_CHECK_EQ(vq.num_free, 0);
    CLAB_CHECK_EQ(clab_vring_add(&vq, sg, 1), -1);
}

/* --- chapter 6: interrupts ----------------------------------------------- */

static void test_ch6_ladder(void)
{
    clab_irq_facts f = {.capability_present = true,
                        .table_mapped = true,
                        .vector_allocated = true,
                        .device_accepted = true,
                        .line_assigned = true,
                        .attach_errno = 0};

    CLAB_CHECK_EQ(clab_irq_choose(&f), CLAB_IRQ_MSIX);
    f.vector_allocated = false;
    CLAB_CHECK_EQ(clab_irq_choose(&f), CLAB_IRQ_INTX);
    f.line_assigned = false;
    CLAB_CHECK_EQ(clab_irq_choose(&f), CLAB_IRQ_POLL);
    f.line_assigned = true;
    f.attach_errno = 5;
    CLAB_CHECK_EQ(clab_irq_choose(&f), CLAB_IRQ_POLL);
}

static void test_ch6_order(void)
{
    CLAB_CHECK(clab_ist_transition_ok(CLAB_IST_ATTACH, CLAB_IST_WAIT));
    CLAB_CHECK(clab_ist_transition_ok(CLAB_IST_WAIT, CLAB_IST_ACK));
    CLAB_CHECK(clab_ist_transition_ok(CLAB_IST_ACK, CLAB_IST_REAP));
    CLAB_CHECK(clab_ist_transition_ok(CLAB_IST_REAP, CLAB_IST_WAIT));
    CLAB_CHECK(clab_ist_transition_ok(CLAB_IST_WAIT, CLAB_IST_DONE));
    /* Reaping before acknowledging is the interrupt-storm bug. */
    CLAB_CHECK(!clab_ist_transition_ok(CLAB_IST_WAIT, CLAB_IST_REAP));
    CLAB_CHECK(!clab_ist_transition_ok(CLAB_IST_DONE, CLAB_IST_WAIT));
}

/* --- chapter 7: 9P ------------------------------------------------------- */

static size_t build_rread(uint8_t *buf, uint16_t tag, const void *data, uint32_t dlen)
{
    const size_t total = CLAB_P9_HDR + 4u + dlen;
    size_t at = 0;

    for (unsigned i = 0; i < 4u; i++)
        buf[at++] = (uint8_t)((uint32_t)total >> (8u * i));
    buf[at++] = (uint8_t)CLAB_P9_RREAD;
    buf[at++] = (uint8_t)(tag & 0xFFu);
    buf[at++] = (uint8_t)(tag >> 8u);
    for (unsigned i = 0; i < 4u; i++)
        buf[at++] = (uint8_t)(dlen >> (8u * i));
    memcpy(buf + at, data, dlen);
    return total;
}

static void test_ch7_encoders(void)
{
    uint8_t req[64];
    const char *names[] = {"a"};

    CLAB_CHECK_EQ(clab_p9_enc_tversion(req, sizeof req, CLAB_P9_NOTAG, 8192u, "9P2000.L"),
                  (int)(CLAB_P9_HDR + 4u + 2u + 8u));
    CLAB_CHECK_EQ(req[4], CLAB_P9_TVERSION);
    CLAB_CHECK_EQ(req[5], 0xFF);
    CLAB_CHECK_EQ(req[6], 0xFF);

    CLAB_CHECK_EQ(clab_p9_enc_tattach(req, sizeof req, 1u, 0u, "u", ""),
                  (int)(CLAB_P9_HDR + 4u + 4u + 2u + 1u + 2u + 4u));
    CLAB_CHECK_EQ(clab_p9_enc_twalk(req, sizeof req, 2u, 0u, 1u, 1u, names),
                  (int)(CLAB_P9_HDR + 4u + 4u + 2u + 2u + 1u));
    CLAB_CHECK_EQ(clab_p9_enc_tlopen(req, sizeof req, 3u, 1u, 0u),
                  (int)(CLAB_P9_HDR + 4u + 4u));
    CLAB_CHECK_EQ(clab_p9_enc_tread(req, sizeof req, 4u, 1u, 0u, 4096u),
                  (int)(CLAB_P9_HDR + 4u + 8u + 4u));

    /* A capacity too small must fail, not overflow. */
    CLAB_CHECK(clab_p9_enc_tversion(req, 8u, 0u, 8192u, "9P2000.L") < 0);
}

static void test_ch7_validation(void)
{
    uint8_t rpl[64];
    clab_p9_msg msg;
    const uint8_t *data = NULL;
    uint32_t count = 0;
    const size_t total = build_rread(rpl, 4u, "hello", 5u);

    CLAB_CHECK_EQ(clab_p9_check(rpl, total, CLAB_P9_RREAD, 4u, &msg), CLAB_OK);
    CLAB_CHECK_EQ(clab_p9_dec_rread(&msg, &data, &count), CLAB_OK);
    CLAB_CHECK_EQ(count, 5);
    CLAB_CHECK_EQ(data[0], 'h');

    /* Truncated: the size field and the buffer disagree. */
    CLAB_CHECK_EQ(clab_p9_check(rpl, total - 1u, CLAB_P9_RREAD, 4u, &msg),
                  CLAB_ERR_SIZE);
    /* Wrong tag: a reply to somebody else's request. */
    CLAB_CHECK_EQ(clab_p9_check(rpl, total, CLAB_P9_RREAD, 7u, &msg), CLAB_ERR_TAG);
    /* Wrong type. */
    CLAB_CHECK_EQ(clab_p9_check(rpl, total, CLAB_P9_RVERSION, 4u, &msg),
                  CLAB_ERR_TYPE);
    /* Count larger than the payload. */
    rpl[CLAB_P9_HDR] = 200u;
    CLAB_CHECK_EQ(clab_p9_check(rpl, total, CLAB_P9_RREAD, 4u, &msg), CLAB_OK);
    CLAB_CHECK_EQ(clab_p9_dec_rread(&msg, &data, &count), CLAB_ERR_SIZE);
}

static void test_ch7_rversion_and_error(void)
{
    uint8_t rpl[64];
    clab_p9_msg msg;
    char version[16];
    uint32_t msize = 0;
    const char ver[] = "9P2000.L";
    const size_t total = CLAB_P9_HDR + 4u + 2u + 8u;
    size_t at = 0;

    for (unsigned i = 0; i < 4u; i++)
        rpl[at++] = (uint8_t)((uint32_t)total >> (8u * i));
    rpl[at++] = (uint8_t)CLAB_P9_RVERSION;
    rpl[at++] = 0x34u;
    rpl[at++] = 0x12u;
    rpl[at++] = 0x00u;
    rpl[at++] = 0x20u;
    rpl[at++] = 0x00u;
    rpl[at++] = 0x00u;
    rpl[at++] = 8u;
    rpl[at++] = 0u;
    memcpy(rpl + at, ver, 8);

    CLAB_CHECK_EQ(clab_p9_check(rpl, total, CLAB_P9_RVERSION, 0x1234u, &msg), CLAB_OK);
    CLAB_CHECK_EQ(clab_p9_dec_rversion(&msg, &msize, version, sizeof version), CLAB_OK);
    CLAB_CHECK_EQ(msize, 8192);
    CLAB_CHECK_STR(version, "9P2000.L");

    /* Rlerror: one Linux errno, and it is an error, never a success. */
    uint8_t err[32];
    const size_t etotal = CLAB_P9_HDR + 4u;
    at = 0;
    for (unsigned i = 0; i < 4u; i++)
        err[at++] = (uint8_t)((uint32_t)etotal >> (8u * i));
    err[at++] = (uint8_t)CLAB_P9_RLERROR;
    err[at++] = 0x01u;
    err[at++] = 0x00u;
    err[at++] = 2u; /* ENOENT on Linux */
    err[at++] = 0u;
    err[at++] = 0u;
    err[at++] = 0u;
    CLAB_CHECK_EQ(clab_p9_check(err, etotal, CLAB_P9_RLERROR, 1u, &msg), CLAB_OK);
    uint32_t linux_errno = 0;
    CLAB_CHECK_EQ(clab_p9_dec_rlerror(&msg, &linux_errno), CLAB_ERR_REMOTE);
    CLAB_CHECK_EQ(linux_errno, 2);
}

int main(void)
{
    test_ch3_chain();
    test_ch3_cycle_and_missing();
    test_ch3_fallback_and_interior();
    test_ch3_helpers();
    test_ch4_sequence();
    test_ch4_intersection_only();
    test_ch4_failures();
    test_ch5_round_trip();
    test_ch5_indirect_and_full();
    test_ch6_ladder();
    test_ch6_order();
    test_ch7_encoders();
    test_ch7_validation();
    test_ch7_rversion_and_error();
    return clab_result();
}

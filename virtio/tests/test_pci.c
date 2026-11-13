/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/tests/test_pci.c - modern PCI discovery and transport on a fake bus.
 *
 * Covers VIO-01, VIO-02, VIO-04 and VIO-05 without a PCI server: the device is
 * a 256 byte configuration space and two RAM arrays standing for its BARs. The
 * capability walk (types 1 COMMON, 2 NOTIFY with notify_off_multiplier, 3 ISR,
 * 4 DEVICE, 5 PCI_CFG), the MSI-X table programming and the queue register
 * writes are the real code paths; only the bus underneath is fake.
 */
#include "pgw/test.h"
#include "pgw/virtio.h"
#include "virtio_priv.h"

#include <string.h>

#define CFG_BYTES 256u
#define BAR0_BYTES 0x200u
#define BAR1_BYTES 0x800u

/* Common configuration offsets (virtio 1.2 §4.1.4.3). */
#define CC_DEV_FEAT_SEL 0x00u
#define CC_DEV_FEAT     0x04u
#define CC_NUM_QUEUES   0x12u
#define CC_STATUS       0x14u
#define CC_QSEL         0x16u
#define CC_QSIZE        0x18u
#define CC_QENABLE      0x1cu
#define CC_QNOTIFY      0x1eu
#define CC_QDESC        0x20u
#define CC_QDRIVER      0x28u
#define CC_QDEVICE      0x30u

typedef struct fake_pci {
    uint8_t cfg[CFG_BYTES];
    uint8_t bar0[BAR0_BYTES];
    uint8_t bar1[BAR1_BYTES];
    uint64_t features;
    uint16_t queue_max;
    uint16_t notify_off;
    uint32_t notify_hits;
    uint16_t msix_off;
    /* Real length of the device-specific region at BAR1 + 0x300, 0 = as long
     * as the capability claims. QEMU advertises 0x1000 bytes but answers 0xff
     * past the real structure (2 + tag length for virtio-9p). */
    uint32_t devcfg_len;
} fake_pci;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

/* --- configuration space accessor ----------------------------------------- */

static pgw_err cfg_read(void *ctx, uint16_t off, void *out, size_t len)
{
    fake_pci *f = (fake_pci *)ctx;
    if ((size_t)off + len > CFG_BYTES)
        return PGW_EIO;
    memcpy(out, &f->cfg[off], len);
    return PGW_OK;
}

static pgw_err cfg_write(void *ctx, uint16_t off, const void *in, size_t len)
{
    fake_pci *f = (fake_pci *)ctx;
    if ((size_t)off + len > CFG_BYTES)
        return PGW_EIO;
    memcpy(&f->cfg[off], in, len);
    return PGW_OK;
}

/* --- BAR accessor --------------------------------------------------------- */

static uint8_t *bar_ptr(fake_pci *f, uint8_t bar, uint32_t off, size_t len)
{
    if (bar == 0u && (size_t)off + len <= BAR0_BYTES)
        return &f->bar0[off];
    if (bar == 1u && (size_t)off + len <= BAR1_BYTES)
        return &f->bar1[off];
    return NULL;
}

static uint32_t bar_r32(void *ctx, uint8_t bar, uint32_t off)
{
    fake_pci *f = (fake_pci *)ctx;
    /* Two registers answer dynamically, like a real device would. */
    if (bar == 0u && off == CC_DEV_FEAT) {
        uint32_t sel = rd32(&f->bar0[CC_DEV_FEAT_SEL]);
        return sel == 0u ? (uint32_t)(f->features & 0xFFFFFFFFu)
                         : (uint32_t)(f->features >> 32);
    }
    /* A read that crosses the real end of the device-specific region. */
    if (bar == 1u && f->devcfg_len != 0u && off >= 0x300u &&
        off + 4u > 0x300u + f->devcfg_len)
        return 0xFFFFFFFFu;
    uint8_t *p = bar_ptr(f, bar, off, 4u);
    return p == NULL ? 0u : rd32(p);
}

/* Real byte access, what a bus driver with a mapped BAR can do. */
static uint8_t bar_r8(void *ctx, uint8_t bar, uint32_t off)
{
    uint8_t *p = bar_ptr((fake_pci *)ctx, bar, off, 1u);
    return p == NULL ? 0u : *p;
}

static void bar_w8(void *ctx, uint8_t bar, uint32_t off, uint8_t val)
{
    uint8_t *p = bar_ptr((fake_pci *)ctx, bar, off, 1u);
    if (p != NULL)
        *p = val;
}

static void bar_w32(void *ctx, uint8_t bar, uint32_t off, uint32_t val)
{
    fake_pci *f = (fake_pci *)ctx;
    if (bar == 0u && off == 0x100u + (uint32_t)f->notify_off * 4u)
        f->notify_hits++;
    uint8_t *p = bar_ptr(f, bar, off, 4u);
    if (p != NULL)
        wr32(p, val);
}

/* --- a complete modern device --------------------------------------------- */

static void put_vendor_cap(fake_pci *f, uint16_t at, uint8_t next,
                           uint8_t cfg_type, uint8_t bar, uint32_t off,
                           uint32_t len)
{
    uint8_t *c = &f->cfg[at];
    c[0] = 0x09u; /* vendor specific */
    c[1] = next;
    c[2] = 16u; /* cap_len */
    c[3] = cfg_type;
    c[4] = bar;
    c[5] = 0u;
    c[6] = 0u;
    c[7] = 0u;
    wr32(&c[8], off);
    wr32(&c[12], len);
}

/* Five vendor capabilities, chained 0x20 bytes apart (a NOTIFY capability is
 * 20 bytes long, so packing them closer would overlap). */
static void add_vendor_caps(fake_pci *f)
{
    put_vendor_cap(f, 0x40u, 0x60u, PGW_PCI_CAP_COMMON_CFG, 0u, 0x000u, 0x40u);
    put_vendor_cap(f, 0x60u, 0x80u, PGW_PCI_CAP_NOTIFY_CFG, 0u, 0x100u, 0x40u);
    wr32(&f->cfg[0x60u + 16u], 4u); /* notify_off_multiplier */
    f->cfg[0x62u] = 20u;            /* cap_len of the notify capability */
    put_vendor_cap(f, 0x80u, 0xa0u, PGW_PCI_CAP_ISR_CFG, 1u, 0x200u, 0x10u);
    put_vendor_cap(f, 0xa0u, 0xc0u, PGW_PCI_CAP_DEVICE_CFG, 1u, 0x300u, 0x40u);
    put_vendor_cap(f, 0xc0u, 0xe0u, PGW_PCI_CAP_PCI_CFG, 1u, 0x400u, 0x10u);
}

/* MSI-X: one vector, table in BAR1 at 0x500, PBA at 0x600. */
static void add_msix_cap(fake_pci *f)
{
    uint8_t *m = &f->cfg[0xe0u];
    m[0] = 0x11u;
    m[1] = 0x00u; /* end of the capability list */
    wr16(&m[2], 12u);           /* cap_len */
    wr16(&m[4], 0u);            /* message control: 1 vector, disabled */
    wr32(&m[4], 0x00000501u);   /* table: BAR1, offset 0x500 */
    wr32(&m[8], 0x00000601u);   /* PBA: BAR1, offset 0x600 */
    f->msix_off = 0xe0u;
}

/* Device configuration space at DEVICE_CFG: virtio_9p_config is a length
 * prefixed tag, virtio_fs_config a fixed 36 byte tag plus the number of
 * request queues. */
static void add_device_cfg(fake_pci *f, uint16_t device_id, const char *tag)
{
    size_t len = strlen(tag);
    if (device_id == PGW_VIRTIO_PCI_DID_FS_MODERN) {
        memcpy(&f->bar1[0x300], tag, len);
        wr32(&f->bar1[0x300 + 36], 2u); /* two request queues */
    } else {
        f->bar1[0x300] = (uint8_t)(len & 0xFFu);
        f->bar1[0x301] = (uint8_t)(len >> 8);
        memcpy(&f->bar1[0x302], tag, len);
    }
}

static fake_pci *make_pci(uint16_t device_id, uint16_t vendor_id)
{
    static fake_pci f;
    memset(&f, 0, sizeof(f));
    f.features = PGW_VIO_F_VERSION_1 | PGW_VIO_F_RING_INDIRECT_DESC |
                 PGW_VIO_F_RING_EVENT_IDX;
    f.queue_max = 16u;
    f.notify_off = 2u;

    wr16(&f.cfg[0x00], vendor_id);
    wr16(&f.cfg[0x02], device_id);
    wr16(&f.cfg[0x06], 0x0010u); /* status: capability list present */
    f.cfg[0x34] = 0x40u;         /* first capability */

    add_vendor_caps(&f);
    add_msix_cap(&f);

    /* COMMON_CFG registers that the driver only reads: the number of queues,
     * the largest queue it accepts, and the notification offset of a queue. */
    wr16(&f.bar0[CC_NUM_QUEUES], 4u);
    wr16(&f.bar0[CC_QSIZE], f.queue_max);
    wr16(&f.bar0[CC_QNOTIFY], f.notify_off);

    add_device_cfg(&f, device_id, "hostshare");
    return &f;
}

static pgw_vio_dev *open_pci(fake_pci *f, uint16_t device_id,
                             const char *tag, bool msix)
{
    pgw_pci_cfg_io cfg = {cfg_read, cfg_write, f};
    pgw_pci_bar_io bars = {bar_r32, bar_w32, f, NULL, NULL};
    return pgw_vio_pci_sim_open(device_id, &cfg, bars, tag, msix);
}

static pgw_vio_dev *open_pci_bytes(fake_pci *f, uint16_t device_id,
                                   const char *tag)
{
    pgw_pci_cfg_io cfg = {cfg_read, cfg_write, f};
    pgw_pci_bar_io bars = {bar_r32, bar_w32, f, bar_r8, bar_w8};
    return pgw_vio_pci_sim_open(device_id, &cfg, bars, tag, false);
}

/* --- identity (VIO-01) ---------------------------------------------------- */

static void test_modern_9p_device_is_found(void)
{
    fake_pci *f = make_pci(PGW_VIRTIO_PCI_DID_9P_MODERN, PGW_VIRTIO_PCI_VENDOR);
    pgw_vio_dev *dev = open_pci(f, PGW_VIRTIO_ID_9P, "hostshare", false);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_U(pgw_vio_get_info(dev)->bus_kind, 1u);
    PGW_ASSERT_EQ_U(pgw_vio_get_info(dev)->device_id, PGW_VIRTIO_ID_9P);
    PGW_ASSERT_EQ_U(pgw_vio_get_info(dev)->num_queues, 4u);
    PGW_ASSERT_STR_EQ(pgw_vio_get_info(dev)->tag, "hostshare");
    pgw_vio_destroy(dev);
}

static void test_transitional_9p_id_is_accepted(void)
{
    fake_pci *f =
        make_pci(PGW_VIRTIO_PCI_DID_9P_TRANSITIONAL, PGW_VIRTIO_PCI_VENDOR);
    pgw_vio_dev *dev = open_pci(f, PGW_VIRTIO_ID_9P, NULL, false);
    PGW_ASSERT(dev != NULL);
    pgw_vio_destroy(dev);
}

static void test_virtiofs_id_is_accepted(void)
{
    fake_pci *f = make_pci(PGW_VIRTIO_PCI_DID_FS_MODERN, PGW_VIRTIO_PCI_VENDOR);
    pgw_vio_dev *dev = open_pci(f, PGW_VIRTIO_ID_FS, NULL, false);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_U(pgw_vio_get_info(dev)->device_id, PGW_VIRTIO_ID_FS);
    char tag[64];
    uint32_t req = 0;
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_fs(dev, tag, sizeof(tag), &req), PGW_OK);
    PGW_ASSERT_STR_EQ(tag, "hostshare");
    PGW_ASSERT_EQ_U(req, 2u);
    pgw_vio_destroy(dev);
}

static void test_wrong_vendor_or_device_is_refused(void)
{
    fake_pci *f = make_pci(PGW_VIRTIO_PCI_DID_9P_MODERN, 0x8086u);
    PGW_ASSERT(open_pci(f, PGW_VIRTIO_ID_9P, NULL, false) == NULL);

    f = make_pci(PGW_VIRTIO_PCI_DID_FS_MODERN, PGW_VIRTIO_PCI_VENDOR);
    /* A virtiofs device is not a 9P device. */
    PGW_ASSERT(open_pci(f, PGW_VIRTIO_ID_9P, NULL, false) == NULL);
}

static void test_tag_selects_the_device(void)
{
    fake_pci *f = make_pci(PGW_VIRTIO_PCI_DID_9P_MODERN, PGW_VIRTIO_PCI_VENDOR);
    PGW_ASSERT(open_pci(f, PGW_VIRTIO_ID_9P, "other", false) == NULL);
    pgw_vio_dev *dev = open_pci(f, PGW_VIRTIO_ID_9P, "hostshare", false);
    PGW_ASSERT(dev != NULL);
    pgw_vio_destroy(dev);
}

/* --- capability walk (VIO-02) --------------------------------------------- */

static void test_capabilities_are_all_discovered(void)
{
    fake_pci *f = make_pci(PGW_VIRTIO_PCI_DID_9P_MODERN, PGW_VIRTIO_PCI_VENDOR);
    pgw_pci_cfg_io cfg = {cfg_read, cfg_write, f};
    pgw_pci_caps caps;
    PGW_ASSERT_EQ_I(pgw_pci_scan_caps(&cfg, &caps), PGW_OK);

    PGW_ASSERT_EQ_U(caps.vendor_id, PGW_VIRTIO_PCI_VENDOR);
    PGW_ASSERT_EQ_U(caps.device_id, PGW_VIRTIO_PCI_DID_9P_MODERN);
    PGW_ASSERT(caps.common.present);
    PGW_ASSERT_EQ_U(caps.common.bar, 0u);
    PGW_ASSERT_EQ_U(caps.common.offset, 0x000u);
    PGW_ASSERT(caps.notify.present);
    PGW_ASSERT_EQ_U(caps.notify.offset, 0x100u);
    PGW_ASSERT_EQ_U(caps.notify.multiplier, 4u); /* notify_off_multiplier */
    PGW_ASSERT(caps.isr.present);
    PGW_ASSERT_EQ_U(caps.isr.bar, 1u);
    PGW_ASSERT(caps.device.present);
    PGW_ASSERT_EQ_U(caps.device.offset, 0x300u);
    PGW_ASSERT(caps.pci_cfg.present);
    PGW_ASSERT_EQ_U(caps.msix_off, 0xe0u);
}

static void test_device_without_capability_list_is_refused(void)
{
    fake_pci *f = make_pci(PGW_VIRTIO_PCI_DID_9P_MODERN, PGW_VIRTIO_PCI_VENDOR);
    wr16(&f->cfg[0x06], 0u); /* status: no capability list */
    PGW_ASSERT(open_pci(f, PGW_VIRTIO_ID_9P, NULL, false) == NULL);
}

static void test_device_without_common_cfg_is_refused(void)
{
    fake_pci *f = make_pci(PGW_VIRTIO_PCI_DID_9P_MODERN, PGW_VIRTIO_PCI_VENDOR);
    f->cfg[0x40u + 3u] = 9u; /* COMMON becomes a vendor type we ignore */
    PGW_ASSERT(open_pci(f, PGW_VIRTIO_ID_9P, NULL, false) == NULL);
}

static void test_looping_capability_list_is_refused(void)
{
    fake_pci *f = make_pci(PGW_VIRTIO_PCI_DID_9P_MODERN, PGW_VIRTIO_PCI_VENDOR);
    f->cfg[0x40u + 1u] = 0x40u; /* the first capability points at itself */
    PGW_ASSERT(open_pci(f, PGW_VIRTIO_ID_9P, NULL, false) == NULL);
}

/* --- MSI-X (VIO-04) ------------------------------------------------------- */

static void test_msix_table_is_programmed(void)
{
    fake_pci *f = make_pci(PGW_VIRTIO_PCI_DID_9P_MODERN, PGW_VIRTIO_PCI_VENDOR);
    pgw_vio_dev *dev = open_pci(f, PGW_VIRTIO_ID_9P, NULL, true);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT(pgw_vio_get_info(dev)->msix_active);

    /* Vector 0: address 0xFEE00000, data 0, unmasked. */
    PGW_ASSERT_EQ_U(rd32(&f->bar1[0x500]), 0xFEE00000u);
    PGW_ASSERT_EQ_U(rd32(&f->bar1[0x504]), 0u);
    PGW_ASSERT_EQ_U(rd32(&f->bar1[0x508]), 0u);
    PGW_ASSERT_EQ_U(rd32(&f->bar1[0x50c]), 0u);
    /* And the capability was enabled in configuration space. */
    PGW_ASSERT((rd16(&f->cfg[0xe2]) & 0x8000u) != 0u);

    /* Reset detaches the interrupt and disables MSI-X (VIO-09). */
    pgw_vio_reset(dev);
    PGW_ASSERT((rd16(&f->cfg[0xe2]) & 0x8000u) == 0u);
    PGW_ASSERT_FALSE(pgw_vio_get_info(dev)->msix_active);
    pgw_vio_destroy(dev);
}

static void test_msix_can_be_declined(void)
{
    fake_pci *f = make_pci(PGW_VIRTIO_PCI_DID_9P_MODERN, PGW_VIRTIO_PCI_VENDOR);
    pgw_vio_dev *dev = open_pci(f, PGW_VIRTIO_ID_9P, NULL, false);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_FALSE(pgw_vio_get_info(dev)->msix_active);
    PGW_ASSERT((rd16(&f->cfg[0xe2]) & 0x8000u) == 0u);
    pgw_vio_destroy(dev);
}

/* QEMU's strict device-specific region: the DEVICE capability advertises 0x40
 * bytes while the structure is 2 + strlen(tag) long, and a read that crosses
 * the real end answers 0xff (VIO-05, found on the real virtio-9p device). */
static void test_short_device_region_needs_byte_access(void)
{
    fake_pci *f = make_pci(PGW_VIRTIO_PCI_DID_9P_MODERN, PGW_VIRTIO_PCI_VENDOR);
    f->devcfg_len = 2u + 9u; /* the 11 bytes of virtio_9p_config */

    /* Without byte access the tail of the tag is 0xff: the device is refused. */
    PGW_ASSERT(open_pci(f, PGW_VIRTIO_ID_9P, "hostshare", false) == NULL);

    pgw_vio_dev *dev = open_pci_bytes(f, PGW_VIRTIO_ID_9P, "hostshare");
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_STR_EQ(pgw_vio_get_info(dev)->tag, "hostshare");
    PGW_ASSERT_EQ_I(dev->ops->wait_irq(dev, 0), PGW_ETIMEDOUT);
    f->bar1[0x200] = 1u; /* the ISR byte says "queue 0 changed" */
    PGW_ASSERT_EQ_I(dev->ops->wait_irq(dev, 0), PGW_OK);
    pgw_vio_destroy(dev);

    /* The fallback without byte accessors still reads the 1-byte ISR. */
    pgw_vio_dev *legacy = open_pci(f, PGW_VIRTIO_ID_9P, NULL, false);
    PGW_ASSERT(legacy != NULL);
    PGW_ASSERT_EQ_I(legacy->ops->wait_irq(legacy, 0), PGW_OK);
    pgw_vio_destroy(legacy);
}

/* --- queue and notification ---------------------------------------------- */

static void test_queue_registers_are_written(void)
{
    fake_pci *f = make_pci(PGW_VIRTIO_PCI_DID_9P_MODERN, PGW_VIRTIO_PCI_VENDOR);
    pgw_vio_dev *dev = open_pci(f, PGW_VIRTIO_ID_9P, NULL, false);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1), PGW_OK);

    pgw_virtq *vq = pgw_vq_create(dev, 0, 8u, false, false);
    PGW_ASSERT(vq != NULL);
    PGW_ASSERT_EQ_U(rd16(&f->bar0[CC_QSIZE]), 8u);
    PGW_ASSERT_EQ_U(rd32(&f->bar0[CC_QDESC]), (uint32_t)(vq->desc_pa & 0xFFFFFFFFu));
    PGW_ASSERT_EQ_U(rd32(&f->bar0[CC_QDESC + 4u]), (uint32_t)(vq->desc_pa >> 32));
    PGW_ASSERT_EQ_U(rd32(&f->bar0[CC_QDRIVER]), (uint32_t)(vq->avail_pa & 0xFFFFFFFFu));
    PGW_ASSERT_EQ_U(rd32(&f->bar0[CC_QDEVICE]), (uint32_t)(vq->used_pa & 0xFFFFFFFFu));
    PGW_ASSERT_EQ_U(rd16(&f->bar0[CC_QENABLE]), 1u);

    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_notify_uses_the_offset_multiplier(void)
{
    fake_pci *f = make_pci(PGW_VIRTIO_PCI_DID_9P_MODERN, PGW_VIRTIO_PCI_VENDOR);
    pgw_vio_dev *dev = open_pci(f, PGW_VIRTIO_ID_9P, NULL, false);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1), PGW_OK);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 8u, false, false);
    PGW_ASSERT(vq != NULL);

    PGW_ASSERT(pgw_vq_kick(vq));
    /* queue_notify_off = 2, multiplier = 4: the write lands at 0x100 + 8. */
    PGW_ASSERT_EQ_U(f->notify_hits, 1u);
    PGW_ASSERT_EQ_U(rd16(&f->bar0[0x100u + 8u]), 0u); /* queue 0 */
    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_features_and_status_round_trip(void)
{
    fake_pci *f = make_pci(PGW_VIRTIO_PCI_DID_9P_MODERN, PGW_VIRTIO_PCI_VENDOR);
    pgw_vio_dev *dev = open_pci(f, PGW_VIRTIO_ID_9P, NULL, false);
    PGW_ASSERT(dev != NULL);

    uint64_t want = PGW_VIO_F_VERSION_1 | PGW_VIO_F_RING_INDIRECT_DESC;
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, want), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_vio_get_info(dev)->negotiated_features, want);
    PGW_ASSERT_EQ_U(f->bar0[CC_STATUS], PGW_VIO_STATUS_ACK |
                                           PGW_VIO_STATUS_DRIVER |
                                           PGW_VIO_STATUS_FEATURES |
                                           PGW_VIO_STATUS_DRIVER_OK);
    pgw_vio_destroy(dev);
}

static void test_pci_open_without_a_pci_server_returns_null(void)
{
    /* On a non-QNX build the contract says: NULL, with a logged reason. */
    PGW_ASSERT(pgw_vio_pci_open(PGW_VIRTIO_ID_9P, "hostshare", true) == NULL);
    PGW_ASSERT(pgw_vio_pci_open(PGW_VIRTIO_ID_9P, "hostshare", false) == NULL);
}

PGW_TEST(test_modern_9p_device_is_found);
PGW_TEST(test_transitional_9p_id_is_accepted);
PGW_TEST(test_virtiofs_id_is_accepted);
PGW_TEST(test_wrong_vendor_or_device_is_refused);
PGW_TEST(test_tag_selects_the_device);
PGW_TEST(test_capabilities_are_all_discovered);
PGW_TEST(test_device_without_capability_list_is_refused);
PGW_TEST(test_device_without_common_cfg_is_refused);
PGW_TEST(test_looping_capability_list_is_refused);
PGW_TEST(test_msix_table_is_programmed);
PGW_TEST(test_msix_can_be_declined);
PGW_TEST(test_short_device_region_needs_byte_access);
PGW_TEST(test_queue_registers_are_written);
PGW_TEST(test_notify_uses_the_offset_multiplier);
PGW_TEST(test_features_and_status_round_trip);
PGW_TEST(test_pci_open_without_a_pci_server_returns_null);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}

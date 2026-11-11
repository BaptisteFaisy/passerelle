/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/src/dev.c - device core: status sequence, feature negotiation,
 * device configuration space reads, reset and interrupt teardown.
 *
 * Covers:
 *   VIO-02 - the modern initialisation sequence of virtio 1.2 §3.1:
 *            reset, ACK, DRIVER, feature negotiation, FEATURES_OK check,
 *            DRIVER_OK;
 *   VIO-05 - reading the 9P mount tag (virtio_9p_config) and the virtiofs tag
 *            plus its request queue count (virtio_fs_config), with the tag
 *            treated as untrusted input;
 *   VIO-08 - the negotiated feature set is the intersection of what the device
 *            offers and what the driver asked for; asking for a bit the device
 *            never offered is refused instead of silently ignored;
 *   VIO-09 - reset() detaches the interrupt and zeroes the status register, and
 *            destroy() always goes through it.
 *
 * No allocation happens here except the one that creates the device, and none
 * of the helpers below is called from an interrupt thread.
 */
#include "virtio_priv.h"

#include "pgw/limits.h"
#include "pgw/log.h"

#include <stdlib.h>
#include <string.h>

/* virtio_9p_config: le16 tag_len, then tag_len bytes (virtio 1.2 §5.12.4). */
#define PGW_9P_TAG_MAX 32u
#define PGW_9P_TAG_HDR 2u

/* virtio_fs_config: char tag[36], le32 num_request_queues (§5.11.4). */
#define PGW_FS_TAG_MAX     36u
#define PGW_FS_CFG_BYTES   (PGW_FS_TAG_MAX + 4u)

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* --- default ring allocator (PC, mock, and any non-QNX build) -------------- */

static void *default_vq_alloc(pgw_vio_dev *dev, size_t bytes, size_t align,
                              uint64_t *out_pa)
{
    (void)dev;
    /* aligned_alloc wants a size that is a multiple of the alignment. */
    size_t rounded = (bytes + align - 1u) & ~(align - 1u);
    void *p = aligned_alloc(align, rounded);
    if (p == NULL)
        return NULL;
    memset(p, 0, rounded);
    if (out_pa != NULL)
        *out_pa = (uint64_t)(uintptr_t)p; /* identity mapping on the PC */
    return p;
}

static void default_vq_free(pgw_vio_dev *dev, void *ptr, size_t bytes)
{
    (void)dev;
    (void)bytes;
    free(ptr);
}

/* --- lifecycle ------------------------------------------------------------ */

pgw_vio_dev *pgw_vio_create(const pgw_vio_bus_ops *ops, void *backend,
                            uint16_t device_id, uint32_t bus_kind)
{
    if (ops == NULL)
        return NULL;
    pgw_vio_dev *dev = calloc(1u, sizeof(*dev));
    if (dev == NULL)
        return NULL;
    dev->ops = ops;
    dev->backend = backend;
    dev->info.device_id = device_id;
    dev->info.vendor_id = (uint16_t)PGW_VIRTIO_PCI_VENDOR;
    dev->info.bus_kind = bus_kind;
    dev->vq_alloc = default_vq_alloc;
    dev->vq_free = default_vq_free;
    return dev;
}

void *pgw_vio_backend(pgw_vio_dev *dev)
{
    return dev == NULL ? NULL : dev->backend;
}

const pgw_vio_info *pgw_vio_get_info(const pgw_vio_dev *dev)
{
    return dev == NULL ? NULL : &dev->info;
}

pgw_vio_info *pgw_vio_info_mut(pgw_vio_dev *dev)
{
    return dev == NULL ? NULL : &dev->info;
}

void pgw_vio_destroy(pgw_vio_dev *dev)
{
    if (dev == NULL)
        return;
    /* VIO-09: never leave an interrupt attached behind us. */
    pgw_vio_reset(dev);
    if (dev->ops->destroy != NULL)
        dev->ops->destroy(dev);
    free(dev);
}

/* --- status helpers ------------------------------------------------------- */

static uint8_t dev_status(pgw_vio_dev *dev)
{
    if (dev->ops->get_status == NULL)
        return 0;
    return dev->ops->get_status(dev);
}

static void dev_set_status(pgw_vio_dev *dev, uint8_t status)
{
    if (dev->ops->set_status != NULL)
        dev->ops->set_status(dev, status);
}

void pgw_vio_reset(pgw_vio_dev *dev)
{
    if (dev == NULL)
        return;
    if (dev->ops->teardown_irq != NULL)
        dev->ops->teardown_irq(dev);
    if (dev->irq_release != NULL)
        dev->irq_release(dev);
    dev->irq_attached = false;
    if (dev->ops->reset != NULL)
        dev->ops->reset(dev);
    else
        dev_set_status(dev, 0);
    dev->info.negotiated_features = 0;
    dev->info.msix_active = false;
    dev->host_features = 0;
}

/* --- feature negotiation (VIO-02, VIO-08) --------------------------------- */

/* ACK then DRIVER, checking that the device kept each bit (spec §3.1.1). */
static pgw_err status_ack_driver(pgw_vio_dev *dev)
{
    dev_set_status(dev, PGW_VIO_STATUS_ACK);
    if ((dev_status(dev) & PGW_VIO_STATUS_ACK) == 0)
        return PGW_EIO;
    dev_set_status(dev, (uint8_t)(PGW_VIO_STATUS_ACK | PGW_VIO_STATUS_DRIVER));
    if ((dev_status(dev) & PGW_VIO_STATUS_DRIVER) == 0)
        return PGW_EIO;
    return PGW_OK;
}

/* Write the negotiated set and make the device confirm it can work with it. */
static pgw_err status_features_ok(pgw_vio_dev *dev, uint64_t negotiated)
{
    if (dev->ops->set_features != NULL)
        dev->ops->set_features(dev, negotiated);
    dev_set_status(dev, (uint8_t)(PGW_VIO_STATUS_ACK | PGW_VIO_STATUS_DRIVER |
                                  PGW_VIO_STATUS_FEATURES));
    if ((dev_status(dev) & PGW_VIO_STATUS_FEATURES) == 0) {
        /* The device rejected our feature set: reset and report it. */
        PGW_WARN("virtio: device rejected features 0x%08llx",
                 (unsigned long long)negotiated);
        pgw_vio_reset(dev);
        return PGW_EOPNOTSUPP;
    }
    return PGW_OK;
}

pgw_err pgw_vio_init_device(pgw_vio_dev *dev, uint64_t driver_features)
{
    if (dev == NULL || dev->ops == NULL)
        return PGW_EINVAL;

    pgw_vio_reset(dev);
    if (dev_status(dev) != 0)
        return PGW_EIO; /* the device refused to reset */

    pgw_err e = status_ack_driver(dev);
    if (e != PGW_OK)
        return e;

    dev->host_features = (dev->ops->get_features != NULL)
                             ? dev->ops->get_features(dev)
                             : 0;
    /* Refuse anything the device never offered: silently dropping a feature
     * bit would let the driver build a queue the device cannot read (VIO-08). */
    if ((driver_features & ~dev->host_features) != 0) {
        PGW_WARN("virtio: device 0x%04x does not offer features 0x%08llx",
                 (unsigned)dev->info.device_id,
                 (unsigned long long)(driver_features & ~dev->host_features));
        dev_set_status(dev, (uint8_t)(PGW_VIO_STATUS_ACK |
                                      PGW_VIO_STATUS_DRIVER |
                                      PGW_VIO_STATUS_FEATURES |
                                      PGW_VIO_STATUS_FAILED));
        return PGW_EOPNOTSUPP;
    }

    uint64_t negotiated = driver_features & dev->host_features;
    e = status_features_ok(dev, negotiated);
    if (e != PGW_OK)
        return e;

    dev->info.negotiated_features = negotiated;
    dev_set_status(dev, (uint8_t)(PGW_VIO_STATUS_ACK | PGW_VIO_STATUS_DRIVER |
                                  PGW_VIO_STATUS_FEATURES |
                                  PGW_VIO_STATUS_DRIVER_OK));
    if ((dev_status(dev) & PGW_VIO_STATUS_DRIVER_OK) == 0) {
        pgw_vio_reset(dev);
        return PGW_EIO;
    }
    PGW_INFO("virtio: device 0x%04x ready, features 0x%08llx",
             (unsigned)dev->info.device_id, (unsigned long long)negotiated);
    return PGW_OK;
}

/* --- device configuration space (VIO-05) ---------------------------------- */

static pgw_err read_cfg(pgw_vio_dev *dev, uint32_t off, void *out, size_t len)
{
    if (dev->ops->read_config == NULL)
        return PGW_ENOSYS;
    return dev->ops->read_config(dev, off, out, len);
}

static void store_tag(pgw_vio_dev *dev, const char *tag, size_t len)
{
    size_t n = len < sizeof(dev->info.tag) - 1u ? len : sizeof(dev->info.tag) - 1u;
    memcpy(dev->info.tag, tag, n);
    dev->info.tag[n] = '\0';
}

/* A tag is a NUL-free, NUL-terminated string: anything else is a lying
 * device, and the caller must see an error rather than a truncated name. */
static pgw_err check_tag_bytes(const char *tag, size_t len, size_t cap)
{
    if (len == 0)
        return PGW_EINVAL;
    if (len + 1u > cap)
        return PGW_ERANGE;
    for (size_t i = 0; i < len; i++) {
        if (tag[i] == '\0')
            return PGW_EINVAL;
    }
    return PGW_OK;
}

pgw_err pgw_vio_read_tag_9p(pgw_vio_dev *dev, char *out, size_t cap)
{
    if (dev == NULL || out == NULL || cap < 2u)
        return PGW_EINVAL;

    uint8_t hdr[PGW_9P_TAG_HDR];
    pgw_err e = read_cfg(dev, 0, hdr, sizeof(hdr));
    if (e != PGW_OK)
        return e;
    uint16_t len = rd_le16(hdr);
    if (len > PGW_9P_TAG_MAX) {
        PGW_WARN("virtio-9p: mount tag length %u exceeds %u", (unsigned)len,
                 (unsigned)PGW_9P_TAG_MAX);
        return PGW_EINVAL;
    }
    char tmp[PGW_9P_TAG_MAX + 1u];
    e = read_cfg(dev, PGW_9P_TAG_HDR, tmp, len);
    if (e != PGW_OK)
        return e;
    tmp[len] = '\0';
    e = check_tag_bytes(tmp, len, cap);
    if (e != PGW_OK)
        return e;
    memcpy(out, tmp, (size_t)len + 1u);
    store_tag(dev, tmp, len);
    return PGW_OK;
}

pgw_err pgw_vio_read_tag_fs(pgw_vio_dev *dev, char *out, size_t cap,
                            uint32_t *out_req_queues)
{
    if (dev == NULL || out == NULL || cap < 2u)
        return PGW_EINVAL;

    uint8_t raw[PGW_FS_CFG_BYTES];
    pgw_err e = read_cfg(dev, 0, raw, sizeof(raw));
    if (e != PGW_OK)
        return e;

    size_t len = 0;
    while (len < PGW_FS_TAG_MAX && raw[len] != '\0')
        len++;
    if (len == PGW_FS_TAG_MAX) {
        PGW_WARN("virtiofs: tag is not NUL terminated in %u bytes",
                 (unsigned)PGW_FS_TAG_MAX);
        return PGW_EINVAL;
    }
    const char *tag = (const char *)raw;
    e = check_tag_bytes(tag, len, cap);
    if (e != PGW_OK)
        return e;

    uint32_t req = rd_le32(&raw[PGW_FS_TAG_MAX]);
    if (req == 0) {
        PGW_WARN("virtiofs: num_request_queues is zero");
        return PGW_EINVAL;
    }
    /* One high priority queue plus the request queues must exist. */
    if (dev->info.num_queues != 0 &&
        (uint32_t)dev->info.num_queues < req + 1u) {
        PGW_WARN("virtiofs: %u request queues announced, device has %u queues",
                 (unsigned)req, (unsigned)dev->info.num_queues);
        return PGW_EINVAL;
    }

    memcpy(out, tag, len + 1u);
    store_tag(dev, tag, len);
    if (out_req_queues != NULL)
        *out_req_queues = req;
    return PGW_OK;
}

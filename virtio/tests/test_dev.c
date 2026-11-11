/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/tests/test_dev.c - device core: status sequence, feature negotiation,
 * device configuration space, reset and interrupt teardown.
 *
 * Covers VIO-02 (modern init sequence), VIO-05 (mount tag selection), VIO-08
 * (negotiated set = offer AND request, refusal of an unoffered bit) and VIO-09
 * (reset detaches the interrupt).
 */
#include "pgw/test.h"
#include "pgw/virtio.h"
#include "virtio_priv.h"

#include <string.h>

/* A backend that records what the core asked of it. */
typedef struct fake_bus {
    uint8_t status;
    uint64_t host_features;
    uint64_t driver_features;
    uint32_t resets;
    uint32_t teardowns;
    uint8_t cfg[64];
    size_t cfg_len;
    bool refuse_features;
} fake_bus;

static void fake_destroy(pgw_vio_dev *dev) { (void)dev; }

static void fake_reset(pgw_vio_dev *dev)
{
    fake_bus *b = (fake_bus *)pgw_vio_backend(dev);
    b->status = 0;
    b->driver_features = 0;
    b->resets++;
}

static uint8_t fake_get_status(pgw_vio_dev *dev)
{
    return ((fake_bus *)pgw_vio_backend(dev))->status;
}

static void fake_set_status(pgw_vio_dev *dev, uint8_t status)
{
    fake_bus *b = (fake_bus *)pgw_vio_backend(dev);
    /* A device that rejects the feature set clears FEATURES_OK by itself. */
    if (b->refuse_features && (status & PGW_VIO_STATUS_FEATURES) != 0)
        status = (uint8_t)(status & (uint8_t)~PGW_VIO_STATUS_FEATURES);
    b->status = status;
}

static uint64_t fake_get_features(pgw_vio_dev *dev)
{
    return ((fake_bus *)pgw_vio_backend(dev))->host_features;
}

static void fake_set_features(pgw_vio_dev *dev, uint64_t features)
{
    ((fake_bus *)pgw_vio_backend(dev))->driver_features = features;
}

static pgw_err fake_read_config(pgw_vio_dev *dev, uint32_t off, void *out,
                                size_t len)
{
    fake_bus *b = (fake_bus *)pgw_vio_backend(dev);
    if (off > b->cfg_len || len > b->cfg_len - off)
        return PGW_EINVAL;
    memcpy(out, &b->cfg[off], len);
    return PGW_OK;
}

static void fake_teardown_irq(pgw_vio_dev *dev)
{
    ((fake_bus *)pgw_vio_backend(dev))->teardowns++;
}

static const pgw_vio_bus_ops g_fake_ops = {
    .destroy = fake_destroy,
    .reset = fake_reset,
    .get_status = fake_get_status,
    .set_status = fake_set_status,
    .get_features = fake_get_features,
    .set_features = fake_set_features,
    .read_config = fake_read_config,
    .teardown_irq = fake_teardown_irq,
};

static pgw_vio_dev *make_dev(fake_bus *bus, uint64_t host_features)
{
    memset(bus, 0, sizeof(*bus));
    bus->host_features = host_features;
    return pgw_vio_create(&g_fake_ops, bus, PGW_VIRTIO_ID_9P, 1u);
}

/* --- status sequence ------------------------------------------------------ */

static void test_init_walks_the_status_sequence(void)
{
    fake_bus bus;
    pgw_vio_dev *dev = make_dev(&bus, PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);

    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1), PGW_OK);
    /* The device ended in DRIVER_OK with every earlier bit still set. */
    PGW_ASSERT_EQ_U(bus.status, PGW_VIO_STATUS_ACK | PGW_VIO_STATUS_DRIVER |
                                    PGW_VIO_STATUS_FEATURES |
                                    PGW_VIO_STATUS_DRIVER_OK);
    PGW_ASSERT_EQ_U(pgw_vio_get_info(dev)->negotiated_features,
                    PGW_VIO_F_VERSION_1);
    PGW_ASSERT_EQ_U(bus.driver_features, PGW_VIO_F_VERSION_1);
    pgw_vio_destroy(dev);
}

static void test_init_intersects_features(void)
{
    fake_bus bus;
    uint64_t offered = PGW_VIO_F_VERSION_1 | PGW_VIO_F_RING_EVENT_IDX |
                       PGW_VIO_F_RING_INDIRECT_DESC;
    pgw_vio_dev *dev = make_dev(&bus, offered);
    PGW_ASSERT(dev != NULL);

    /* The driver asks for less than the device offers: only the intersection
     * reaches the device (VIO-08). */
    uint64_t want = PGW_VIO_F_VERSION_1 | PGW_VIO_F_RING_INDIRECT_DESC;
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, want), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_vio_get_info(dev)->negotiated_features, want);
    PGW_ASSERT_EQ_U(bus.driver_features, want);
    pgw_vio_destroy(dev);
}

static void test_init_refuses_unoffered_feature(void)
{
    fake_bus bus;
    pgw_vio_dev *dev = make_dev(&bus, PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);

    /* EVENT_IDX was never offered: the whole initialisation must fail rather
     * than silently drop the bit. */
    PGW_ASSERT_EQ_I(
        pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1 | PGW_VIO_F_RING_EVENT_IDX),
        PGW_EOPNOTSUPP);
    PGW_ASSERT_EQ_U(pgw_vio_get_info(dev)->negotiated_features, 0);
    PGW_ASSERT((bus.status & PGW_VIO_STATUS_FAILED) != 0);
    pgw_vio_destroy(dev);
}

static void test_init_handles_device_rejecting_features(void)
{
    fake_bus bus;
    pgw_vio_dev *dev = make_dev(&bus, PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    bus.refuse_features = true;

    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1),
                    PGW_EOPNOTSUPP);
    PGW_ASSERT_EQ_U(pgw_vio_get_info(dev)->negotiated_features, 0);
    PGW_ASSERT(bus.resets > 0); /* the core reset before reporting */
    pgw_vio_destroy(dev);
}

/* --- reset and teardown (VIO-09) ------------------------------------------ */

static void test_reset_detaches_irq_and_zeroes_status(void)
{
    fake_bus bus;
    pgw_vio_dev *dev = make_dev(&bus, PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1), PGW_OK);

    uint32_t teardowns_before = bus.teardowns;
    pgw_vio_reset(dev);
    PGW_ASSERT_EQ_U(bus.status, 0);
    PGW_ASSERT_EQ_U(bus.teardowns, teardowns_before + 1u);
    PGW_ASSERT_EQ_U(pgw_vio_get_info(dev)->negotiated_features, 0);
    PGW_ASSERT_FALSE(pgw_vio_get_info(dev)->msix_active);
    pgw_vio_destroy(dev);
}

static void test_destroy_resets_first(void)
{
    fake_bus bus;
    pgw_vio_dev *dev = make_dev(&bus, PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1), PGW_OK);

    uint32_t teardowns_before = bus.teardowns;
    pgw_vio_destroy(dev); /* must not leak the interrupt */
    PGW_ASSERT_EQ_U(bus.teardowns, teardowns_before + 1u);
}

/* --- device configuration: 9P tag (VIO-05) -------------------------------- */

static void fill_9p_tag(fake_bus *bus, const char *tag)
{
    size_t len = strlen(tag);
    bus->cfg[0] = (uint8_t)(len & 0xFFu);
    bus->cfg[1] = (uint8_t)(len >> 8);
    memcpy(&bus->cfg[2], tag, len);
    bus->cfg_len = 2u + len;
}

static void test_reads_9p_mount_tag(void)
{
    fake_bus bus;
    pgw_vio_dev *dev = make_dev(&bus, PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    fill_9p_tag(&bus, "hostshare");

    char tag[64];
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_9p(dev, tag, sizeof(tag)), PGW_OK);
    PGW_ASSERT_STR_EQ(tag, "hostshare");
    PGW_ASSERT_STR_EQ(pgw_vio_get_info(dev)->tag, "hostshare");
    pgw_vio_destroy(dev);
}

static void test_rejects_oversized_9p_tag(void)
{
    fake_bus bus;
    pgw_vio_dev *dev = make_dev(&bus, PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    /* A tag length the spec forbids (32 bytes max) is refused, not truncated. */
    bus.cfg[0] = 200u;
    bus.cfg[1] = 0u;
    bus.cfg_len = 2u;
    char tag[64];
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_9p(dev, tag, sizeof(tag)), PGW_EINVAL);
    pgw_vio_destroy(dev);
}

static void test_rejects_tag_that_does_not_fit(void)
{
    fake_bus bus;
    pgw_vio_dev *dev = make_dev(&bus, PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    fill_9p_tag(&bus, "share");
    char small[4];
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_9p(dev, small, sizeof(small)), PGW_ERANGE);
    pgw_vio_destroy(dev);
}

/* --- device configuration: virtiofs (VFS-01) ------------------------------ */

static void fill_fs_cfg(fake_bus *bus, const char *tag, uint32_t queues)
{
    memset(bus->cfg, 0, sizeof(bus->cfg));
    memcpy(bus->cfg, tag, strlen(tag));
    bus->cfg[36] = (uint8_t)(queues & 0xFFu);
    bus->cfg[37] = (uint8_t)((queues >> 8) & 0xFFu);
    bus->cfg[38] = (uint8_t)((queues >> 16) & 0xFFu);
    bus->cfg[39] = (uint8_t)((queues >> 24) & 0xFFu);
    bus->cfg_len = 40u;
}

static void test_reads_virtiofs_tag_and_queue_count(void)
{
    fake_bus bus;
    pgw_vio_dev *dev = make_dev(&bus, PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    pgw_vio_info_mut(dev)->num_queues = 3;
    fill_fs_cfg(&bus, "vfsroot", 2u);

    char tag[64];
    uint32_t req = 0;
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_fs(dev, tag, sizeof(tag), &req), PGW_OK);
    PGW_ASSERT_STR_EQ(tag, "vfsroot");
    PGW_ASSERT_EQ_U(req, 2u);
    PGW_ASSERT_STR_EQ(pgw_vio_get_info(dev)->tag, "vfsroot");
    pgw_vio_destroy(dev);
}

static void test_rejects_virtiofs_without_request_queue(void)
{
    fake_bus bus;
    pgw_vio_dev *dev = make_dev(&bus, PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    pgw_vio_info_mut(dev)->num_queues = 4;
    fill_fs_cfg(&bus, "vfsroot", 0u);

    char tag[64];
    uint32_t req = 0;
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_fs(dev, tag, sizeof(tag), &req),
                    PGW_EINVAL);
    pgw_vio_destroy(dev);
}

static void test_rejects_virtiofs_queue_count_above_device(void)
{
    fake_bus bus;
    pgw_vio_dev *dev = make_dev(&bus, PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    pgw_vio_info_mut(dev)->num_queues = 2;
    fill_fs_cfg(&bus, "vfsroot", 8u); /* 8 + 1 queues on a 2 queue device */

    char tag[64];
    uint32_t req = 0;
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_fs(dev, tag, sizeof(tag), &req),
                    PGW_EINVAL);
    pgw_vio_destroy(dev);
}

static void test_rejects_unterminated_virtiofs_tag(void)
{
    fake_bus bus;
    pgw_vio_dev *dev = make_dev(&bus, PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    pgw_vio_info_mut(dev)->num_queues = 2;
    memset(bus.cfg, 'x', sizeof(bus.cfg)); /* no NUL in the 36 byte tag field */
    bus.cfg_len = 40u;

    char tag[64];
    uint32_t req = 0;
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_fs(dev, tag, sizeof(tag), &req),
                    PGW_EINVAL);
    pgw_vio_destroy(dev);
}

static void test_null_arguments_are_refused(void)
{
    fake_bus bus;
    pgw_vio_dev *dev = make_dev(&bus, PGW_VIO_F_VERSION_1);
    PGW_ASSERT(dev != NULL);
    char tag[64];
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_9p(NULL, tag, sizeof(tag)), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_9p(dev, NULL, sizeof(tag)), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_9p(dev, tag, 1u), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(NULL, 0), PGW_EINVAL);
    PGW_ASSERT(pgw_vio_create(NULL, NULL, 9u, 0u) == NULL);
    pgw_vio_destroy(dev);
}

PGW_TEST(test_init_walks_the_status_sequence);
PGW_TEST(test_init_intersects_features);
PGW_TEST(test_init_refuses_unoffered_feature);
PGW_TEST(test_init_handles_device_rejecting_features);
PGW_TEST(test_reset_detaches_irq_and_zeroes_status);
PGW_TEST(test_destroy_resets_first);
PGW_TEST(test_reads_9p_mount_tag);
PGW_TEST(test_rejects_oversized_9p_tag);
PGW_TEST(test_rejects_tag_that_does_not_fit);
PGW_TEST(test_reads_virtiofs_tag_and_queue_count);
PGW_TEST(test_rejects_virtiofs_without_request_queue);
PGW_TEST(test_rejects_virtiofs_queue_count_above_device);
PGW_TEST(test_rejects_unterminated_virtiofs_tag);
PGW_TEST(test_null_arguments_are_refused);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}

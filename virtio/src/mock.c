/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/src/mock.c - in-memory virtio device (VIO-10, P9-10).
 *
 * A simulated device: real status register, real feature words, real device
 * configuration space, real queues, and a handler callback that plays the part
 * QEMU plays. Nothing here touches hardware, so every test of the transport,
 * of the 9P client and of virtiofs runs on the PC.
 *
 * Two modes, because both exist in real life:
 *   - synchronous: a handler is given to pgw_vio_mock_open() and runs on the
 *     kick, like a device that answers instantly;
 *   - asynchronous: no handler, so the request sits in the ring until the test
 *     calls pgw_vio_mock_step(), like a device that answers later.
 *
 * The mock also records how often it was reset, notified and detached, which is
 * what makes VIO-09 (clean reset, no interrupt left behind) checkable.
 */
#include "virtio_priv.h"

#include "pgw/limits.h"
#include "pgw/log.h"

#include <stdlib.h>
#include <string.h>

#define PGW_MOCK_MAX_QUEUES 32u
#define PGW_MOCK_CFG_BYTES  64u
#define PGW_MOCK_FS_TAG     36u

typedef struct mock_state {
    uint8_t status;
    uint64_t host_features;
    uint64_t driver_features;
    uint8_t cfg[PGW_MOCK_CFG_BYTES];
    size_t cfg_len;
    pgw_vio_mock_fn handler;
    void *user;
    pgw_virtq *queues[PGW_MOCK_MAX_QUEUES];
    uint16_t num_queues;
    uint32_t notify_count;
    uint32_t reset_count;
    uint32_t teardown_count;
    bool sync; /* answer on the kick instead of on pgw_vio_mock_step() */
} mock_state;

static mock_state *state_of(pgw_vio_dev *dev)
{
    if (dev == NULL || dev->info.bus_kind != 0u)
        return NULL;
    return (mock_state *)dev->backend;
}

static void mock_destroy(pgw_vio_dev *dev)
{
    free(state_of(dev));
}

static void mock_reset(pgw_vio_dev *dev)
{
    mock_state *st = state_of(dev);
    if (st == NULL)
        return;
    st->status = 0;
    st->driver_features = 0;
    st->reset_count++;
}

static uint8_t mock_get_status(pgw_vio_dev *dev)
{
    mock_state *st = state_of(dev);
    return st == NULL ? 0u : st->status;
}

static void mock_set_status(pgw_vio_dev *dev, uint8_t status)
{
    mock_state *st = state_of(dev);
    if (st != NULL)
        st->status = status;
}

static uint64_t mock_get_features(pgw_vio_dev *dev)
{
    mock_state *st = state_of(dev);
    return st == NULL ? 0 : st->host_features;
}

static void mock_set_features(pgw_vio_dev *dev, uint64_t features)
{
    mock_state *st = state_of(dev);
    if (st != NULL)
        st->driver_features = features;
}

static pgw_err mock_read_config(pgw_vio_dev *dev, uint32_t offset, void *out,
                                size_t len)
{
    mock_state *st = state_of(dev);
    if (st == NULL || out == NULL)
        return PGW_EINVAL;
    if ((size_t)offset > st->cfg_len || len > st->cfg_len - (size_t)offset)
        return PGW_EINVAL;
    memcpy(out, &st->cfg[offset], len);
    return PGW_OK;
}

static pgw_err mock_setup_queue(pgw_vio_dev *dev, uint16_t qidx, pgw_virtq *vq)
{
    mock_state *st = state_of(dev);
    if (st == NULL || vq == NULL)
        return PGW_EINVAL;
    if (qidx >= st->num_queues || qidx >= (uint16_t)PGW_MOCK_MAX_QUEUES)
        return PGW_EINVAL;
    st->queues[qidx] = vq;
    return PGW_OK;
}

static void mock_notify_queue(pgw_vio_dev *dev, uint16_t qidx)
{
    mock_state *st = state_of(dev);
    if (st == NULL)
        return;
    st->notify_count++;
    if (!st->sync || st->handler == NULL || qidx >= st->num_queues)
        return;
    /* Synchronous device: the answer is in the used ring when we return. */
    (void)pgw_vq_mock_process(st->queues[qidx], st->handler, st->user);
}

static pgw_err mock_wait_irq(pgw_vio_dev *dev, int32_t timeout_ms)
{
    (void)dev;
    (void)timeout_ms;
    return PGW_OK; /* the mock never blocks the caller */
}

static void mock_teardown_irq(pgw_vio_dev *dev)
{
    mock_state *st = state_of(dev);
    if (st != NULL)
        st->teardown_count++;
}

static const pgw_vio_bus_ops g_mock_ops = {
    .destroy = mock_destroy,
    .reset = mock_reset,
    .get_status = mock_get_status,
    .set_status = mock_set_status,
    .get_features = mock_get_features,
    .set_features = mock_set_features,
    .read_config = mock_read_config,
    .setup_queue = mock_setup_queue,
    .notify_queue = mock_notify_queue,
    .wait_irq = mock_wait_irq,
    .teardown_irq = mock_teardown_irq,
};

/* virtio_9p_config: le16 tag_len then the tag, no terminator on the wire. */
static pgw_err fill_9p_cfg(mock_state *st, const char *tag)
{
    size_t len = strlen(tag);
    if (len == 0 || len > 32u)
        return PGW_EINVAL;
    st->cfg[0] = (uint8_t)(len & 0xFFu);
    st->cfg[1] = (uint8_t)((len >> 8) & 0xFFu);
    memcpy(&st->cfg[2], tag, len);
    st->cfg_len = 2u + len;
    return PGW_OK;
}

/* virtio_fs_config: char tag[36] then le32 num_request_queues. */
static pgw_err fill_fs_cfg(mock_state *st, const char *tag, uint16_t num_queues)
{
    size_t len = strlen(tag);
    if (len == 0 || len >= PGW_MOCK_FS_TAG)
        return PGW_EINVAL;
    memcpy(&st->cfg[0], tag, len);
    st->cfg[len] = '\0';
    uint32_t req = num_queues > 1u ? (uint32_t)(num_queues - 1u) : 1u;
    st->cfg[PGW_MOCK_FS_TAG + 0u] = (uint8_t)(req & 0xFFu);
    st->cfg[PGW_MOCK_FS_TAG + 1u] = (uint8_t)((req >> 8) & 0xFFu);
    st->cfg[PGW_MOCK_FS_TAG + 2u] = (uint8_t)((req >> 16) & 0xFFu);
    st->cfg[PGW_MOCK_FS_TAG + 3u] = (uint8_t)((req >> 24) & 0xFFu);
    st->cfg_len = PGW_MOCK_FS_TAG + 4u;
    return PGW_OK;
}

pgw_vio_dev *pgw_vio_mock_open(uint16_t device_id, const char *tag,
                               uint16_t num_queues, uint64_t host_features,
                               pgw_vio_mock_fn handler, void *user)
{
    if (tag == NULL)
        return NULL;
    if (device_id != (uint16_t)PGW_VIRTIO_ID_9P &&
        device_id != (uint16_t)PGW_VIRTIO_ID_FS) {
        PGW_ERR("virtio-mock: unsupported device id %u", (unsigned)device_id);
        return NULL;
    }
    if (num_queues == 0 || num_queues > (uint16_t)PGW_MOCK_MAX_QUEUES) {
        PGW_ERR("virtio-mock: %u queues is out of range", (unsigned)num_queues);
        return NULL;
    }

    mock_state *st = calloc(1u, sizeof(*st));
    if (st == NULL)
        return NULL;
    st->host_features = host_features;
    st->handler = handler;
    st->user = user;
    st->num_queues = num_queues;
    /* A handler given at open time behaves like an instant device. */
    st->sync = handler != NULL;

    pgw_err e = device_id == (uint16_t)PGW_VIRTIO_ID_9P
                    ? fill_9p_cfg(st, tag)
                    : fill_fs_cfg(st, tag, num_queues);
    if (e != PGW_OK) {
        PGW_ERR("virtio-mock: bad mount tag '%s'", tag);
        free(st);
        return NULL;
    }

    pgw_vio_dev *dev = pgw_vio_create(&g_mock_ops, st, device_id, 0u);
    if (dev == NULL) {
        free(st);
        return NULL;
    }
    dev->info.num_queues = num_queues;
    size_t tl = strlen(tag);
    if (tl >= sizeof(dev->info.tag))
        tl = sizeof(dev->info.tag) - 1u;
    memcpy(dev->info.tag, tag, tl);
    dev->info.tag[tl] = '\0';
    return dev;
}

void pgw_vio_mock_forget(pgw_vio_dev *dev, pgw_virtq *vq)
{
    mock_state *st = state_of(dev);
    if (st == NULL)
        return;
    for (uint16_t i = 0; i < st->num_queues && i < (uint16_t)PGW_MOCK_MAX_QUEUES;
         i++) {
        if (st->queues[i] == vq)
            st->queues[i] = NULL;
    }
}

void pgw_vio_mock_set_sync(pgw_vio_dev *dev, bool sync)
{
    mock_state *st = state_of(dev);
    if (st != NULL)
        st->sync = sync;
}

uint32_t pgw_vio_mock_notify_count(const pgw_vio_dev *dev)
{
    if (dev == NULL || dev->info.bus_kind != 0u)
        return 0;
    return ((const mock_state *)dev->backend)->notify_count;
}

uint32_t pgw_vio_mock_reset_count(const pgw_vio_dev *dev)
{
    if (dev == NULL || dev->info.bus_kind != 0u)
        return 0;
    return ((const mock_state *)dev->backend)->reset_count;
}

uint32_t pgw_vio_mock_teardown_count(const pgw_vio_dev *dev)
{
    if (dev == NULL || dev->info.bus_kind != 0u)
        return 0;
    return ((const mock_state *)dev->backend)->teardown_count;
}

pgw_err pgw_vio_mock_queue_flags(pgw_vio_dev *dev, uint16_t qidx, uint16_t flags,
                                 uint16_t avail_event)
{
    mock_state *st = state_of(dev);
    if (st == NULL || qidx >= st->num_queues || qidx >= (uint16_t)PGW_MOCK_MAX_QUEUES)
        return PGW_EINVAL;
    pgw_virtq *vq = st->queues[qidx];
    if (vq == NULL)
        return PGW_EINVAL;
    /* These are the two ways a device asks the driver to stop notifying
     * (virtio 1.2 §2.6.7, VIO-08). */
    *pgw_vq_used_flags(vq) = flags;
    *pgw_vq_avail_event(vq) = avail_event;
    return PGW_OK;
}

size_t pgw_vio_mock_step(pgw_vio_dev *dev, uint16_t qidx)
{
    mock_state *st = state_of(dev);
    if (st == NULL || st->handler == NULL)
        return 0;
    if (qidx >= st->num_queues || qidx >= (uint16_t)PGW_MOCK_MAX_QUEUES)
        return 0;
    return pgw_vq_mock_process(st->queues[qidx], st->handler, st->user);
}

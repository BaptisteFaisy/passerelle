/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fs9p/src/mount_qnx.c - mount and umount of a 9P share.
 *
 * The sequence is the one QNX-07 asks for (the driver is started by mount(8)
 * and stopped by umount(8)) and the one VIO-09 asks for on the way out:
 *
 *   privileges -> device by tag -> virtqueue -> 9P session -> rmfs view ->
 *   resource manager attach -> serve
 *
 * and on the way back, in the reverse order: detach the path, stop the
 * interrupt, reset the device, close the session (which clunks every fid it
 * opened, P9-07), release the device.
 *
 * Covered requirements: QNX-01, QNX-02, QNX-07, VIO-01, VIO-05, VIO-06, VIO-09,
 * P9-01, ROB-03 (a umount under load finishes cleanly), ROB-05.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"
#include "pgw/err.h"
#include "pgw/fs9p.h"
#include "pgw/fs9p_qnx.h"
#include "pgw/limits.h"
#include "pgw/log.h"
#include "pgw/rmfs.h"
#include "pgw/virtio.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

/* Provided by dispatch_qnx.c and irq_qnx.c. */
pgw_err fs9p_dispatch_open(pgw_vio_dev *dev, pgw_virtq *vq, uint32_t timeout_ms);
void fs9p_dispatch_close(void);

/* The mounted share, so the termination handler can undo it. */
static fs9p_dev g_dev;
static pgw_srv g_srv;
static bool g_mounted;

/* Find the device that exports this mount tag (VIO-05) and bring up its queue
 * (VIO-01, VIO-02, VIO-08). */
static pgw_err open_transport(fs9p_dev *dev)
{
    dev->vio = pgw_vio_pci_open(PGW_VIRTIO_ID_9P, dev->opts.tag, true);
    if (dev->vio == NULL) {
        PGW_ERR("fs9p: no virtio-9p device with mount tag '%s'", dev->opts.tag);
        return PGW_ENODEV;
    }
    pgw_err e = pgw_vio_init_device(dev->vio, PGW_VIO_F_VERSION_1 |
                                                 PGW_VIO_F_RING_INDIRECT_DESC |
                                                 PGW_VIO_F_RING_EVENT_IDX);
    if (e != PGW_OK)
        return e;
    const pgw_vio_info *info = pgw_vio_get_info(dev->vio);
    if (info == NULL)
        return PGW_EIO;
    dev->irq = -1;

    /* One queue: 9P2000.L multiplexes everything over a single virtqueue and
     * layer 2 keeps several requests in flight on it (P9-05). */
    dev->queue = pgw_vq_create(dev->vio, 0u, PGW_QUEUE_SIZE_DEFAULT, true, true);
    if (dev->queue == NULL)
        return PGW_ENOMEM;
    return PGW_OK;
}

static void close_transport(fs9p_dev *dev)
{
    fs9p_irq_stop(dev); /* interrupt first: nothing may fire after this */
    if (dev->queue != NULL) {
        pgw_vq_destroy(dev->queue);
        dev->queue = NULL;
    }
    if (dev->vio != NULL) {
        pgw_vio_destroy(dev->vio); /* resets the device (VIO-09) */
        dev->vio = NULL;
    }
}

pgw_err fs9p_mount_and_serve(pgw_mount_opts *opts, const char *mount_point)
{
    if (opts == NULL || mount_point == NULL)
        return PGW_EINVAL;
    memset(&g_dev, 0, sizeof g_dev);
    g_dev.opts = *opts;
    g_dev.irq = -1;

    pgw_err e = fs9p_privdrop(); /* QNX-06 / SEC-01: before any device work */
    if (e != PGW_OK)
        return e;

    e = open_transport(&g_dev);
    if (e != PGW_OK)
        goto fail;
    e = fs9p_dispatch_open(g_dev.vio, g_dev.queue, opts->timeout_ms);
    if (e != PGW_OK)
        goto fail;
    e = fs9p_irq_start(&g_dev); /* VIO-04 */
    if (e != PGW_OK)
        goto fail;

    void *impl = fs9p_backend_init(fs9p_session());
    if (impl == NULL) {
        e = PGW_ENOMEM;
        goto fail;
    }
    g_dev.fs = pgw_rmfs_create(fs9p_backend_ops(), impl, &opts->fs);
    if (g_dev.fs == NULL) {
        e = PGW_ENOMEM;
        goto fail;
    }

    e = pgw_srv_init(&g_srv, g_dev.fs, opts, "fs9p");
    if (e != PGW_OK)
        goto fail;
    e = pgw_srv_attach(&g_srv, mount_point); /* QNX-01 */
    if (e != PGW_OK)
        goto fail;
    g_mounted = true;

    /* A thread pool wider than the request pipeline, so a client blocked on the
     * device never pins the only thread (QNX-05). */
    uint32_t threads = opts->fs.max_inflight + 2u;
    return pgw_srv_serve(&g_srv, threads); /* returns only on error */

fail:
    fs9p_unmount();
    return e;
}

void fs9p_unmount(void)
{
    if (g_mounted) {
        (void)pgw_srv_detach(&g_srv);
        g_mounted = false;
    }
    fs9p_dispatch_close(); /* clunks every fid the session opened (P9-07) */
    if (g_dev.fs != NULL) {
        pgw_rmfs_destroy(g_dev.fs);
        g_dev.fs = NULL;
    }
    close_transport(&g_dev);
}

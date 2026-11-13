/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fsfuse/src/mount_qnx.c - mount and umount of a virtiofs share.
 *
 * The sequence mirrors the 9P driver (QNX-07), with the virtiofs specifics:
 *
 *   privileges -> device id 26 (VIO-01, VFS-01) -> its tag and its number of
 *   request queues -> the hiprio queue and the request queues (VFS-02) ->
 *   FUSE session -> rmfs view -> resource manager attach -> serve
 *
 * and on the way back: detach the path, stop the interrupt, reset the device,
 * close the session (which returns every reference it took, VFS-04), release
 * the device.
 *
 * Covered requirements: QNX-01, QNX-07, VIO-01, VIO-09, VFS-01, VFS-02, VFS-04,
 * ROB-03.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"
#include "pgw/err.h"
#include "pgw/fsfuse.h"
#include "pgw/fsfuse_qnx.h"
#include "pgw/limits.h"
#include "pgw/log.h"
#include "pgw/rmfs.h"
#include "pgw/virtio.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Maximum number of request queues the driver sets up. The device may offer
 * more; the driver uses what it can serve threads for (VFS-01). */
#define FSFUSE_MAX_QUEUES 8u

static fsfuse_dev g_dev;
static pgw_srv g_srv;
static bool g_mounted;

/* Open the virtiofs device and read its tag and its queue count (VFS-01). */
static pgw_err open_transport(fsfuse_dev *dev)
{
    dev->vio = pgw_vio_pci_open(PGW_VIRTIO_ID_FS, dev->opts.tag, true);
    if (dev->vio == NULL) {
        PGW_ERR("fsfuse: no virtiofs device with mount tag '%s'", dev->opts.tag);
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
    dev->irq = info->irq; /* the interrupt line the transport attached (VIO-04) */
    /* The device says how many request queues it has (VFS-01); queue 0 is the
     * hiprio lane, so the request queues are 1..num_queues-1. */
    uint16_t total = info->num_queues;
    if (total < 2u) {
        PGW_ERR("fsfuse: the device offers %u queue(s), one hiprio and one "
                "request queue are needed", total);
        return PGW_ENODEV;
    }
    uint16_t requests = (uint16_t)(total - 1u);
    if (requests > FSFUSE_MAX_QUEUES)
        requests = FSFUSE_MAX_QUEUES;
    dev->num_requests = requests;

    dev->hiprio = pgw_vq_create(dev->vio, 0u, PGW_QUEUE_SIZE_DEFAULT, true, true);
    if (dev->hiprio == NULL)
        return PGW_ENOMEM;
    dev->requests = calloc(requests, sizeof *dev->requests);
    if (dev->requests == NULL)
        return PGW_ENOMEM;
    for (uint16_t i = 0; i < requests; i++) {
        dev->requests[i] =
            pgw_vq_create(dev->vio, (uint16_t)(i + 1u), PGW_QUEUE_SIZE_DEFAULT,
                          true, true);
        if (dev->requests[i] == NULL)
            return PGW_ENOMEM;
    }
    PGW_INFO("fsfuse: device '%s', %u request queues + 1 hiprio queue",
             info->tag, requests);
    return PGW_OK;
}

static void close_transport(fsfuse_dev *dev)
{
    if (dev->requests != NULL) {
        for (uint16_t i = 0; i < dev->num_requests; i++) {
            if (dev->requests[i] != NULL)
                pgw_vq_destroy(dev->requests[i]);
        }
        free(dev->requests);
        dev->requests = NULL;
        dev->num_requests = 0;
    }
    if (dev->hiprio != NULL) {
        pgw_vq_destroy(dev->hiprio);
        dev->hiprio = NULL;
    }
    if (dev->vio != NULL) {
        pgw_vio_destroy(dev->vio); /* resets the device (VIO-09) */
        dev->vio = NULL;
    }
}

pgw_err fsfuse_mount_and_serve(pgw_mount_opts *opts, const char *mount_point)
{
    if (opts == NULL || mount_point == NULL)
        return PGW_EINVAL;
    memset(&g_dev, 0, sizeof g_dev);
    g_dev.opts = *opts;
    g_dev.irq = -1;

    /* The PCI attach needs the PCI server's shared database, readable by root
     * only: the device is opened first and the privileges are dropped
     * afterwards, with the abilities the serving phase needs granted and
     * locked before the drop (QNX-06, SEC-01). */
    pgw_err e = open_transport(&g_dev);
    if (e != PGW_OK)
        goto fail;
    e = fsfuse_privdrop(); /* QNX-06 / SEC-01: after the device work */
    if (e != PGW_OK)
        goto fail;
    e = fsfuse_dispatch_open(&g_dev); /* VFS-02 */
    if (e != PGW_OK)
        goto fail;
    e = fsfuse_irq_start(&g_dev); /* VIO-04 */
    if (e != PGW_OK)
        goto fail;

    void *impl = fsfuse_backend_init(fsfuse_session());
    if (impl == NULL) {
        e = PGW_ENOMEM;
        goto fail;
    }
    g_dev.fs = pgw_rmfs_create(fsfuse_backend_ops(), impl, &opts->fs);
    if (g_dev.fs == NULL) {
        e = PGW_ENOMEM;
        goto fail;
    }
    e = pgw_srv_init(&g_srv, g_dev.fs, opts, "fsfuse");
    if (e != PGW_OK)
        goto fail;
    e = pgw_srv_attach(&g_srv, mount_point); /* QNX-01 */
    if (e != PGW_OK)
        goto fail;
    g_mounted = true;

    uint32_t threads = opts->fs.max_inflight + 2u;
    return pgw_srv_serve(&g_srv, threads);

fail:
    fsfuse_unmount();
    return e;
}

void fsfuse_unmount(void)
{
    if (g_mounted) {
        (void)pgw_srv_detach(&g_srv);
        g_mounted = false;
    }
    fsfuse_dispatch_close(); /* returns every reference (VFS-04) */
    if (g_dev.fs != NULL) {
        pgw_rmfs_destroy(g_dev.fs);
        g_dev.fs = NULL;
    }
    fsfuse_irq_stop(&g_dev);
    close_transport(&g_dev);
}

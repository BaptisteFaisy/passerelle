/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fs9p/src/irq_qnx.c - interrupt of the 9P device, QNX 8.0 style.
 *
 * QNX 8.0 removed InterruptAttach(): a handler can no longer run in interrupt
 * context. The supported shape is an Interrupt Service Thread attached with
 * InterruptAttachThread(), which is what pgw_ist_start() in drivers/common
 * provides. This file finds the right interrupt line and wires it to the queue:
 *
 *   1. MSI-X, when the device exposes the capability and the mount did not
 *      forbid it: the PCI server gives us the line the vector was bound to;
 *   2. the classic INTx line the PCI server reports, which is also what a
 *      `-o irq=N` mount option forces (useful when the PCI server's mapping is
 *      wrong, and the documented fallback of VIO-04).
 *
 * Nothing is allocated on the interrupt path: the callback only bumps the
 * transport's counter, and the owning thread drains the used ring.
 *
 * Covered requirements: VIO-04 (MSI-X with an INTx fallback, through the QNX 8.0
 * interrupt interfaces), VIO-09 (nothing stays attached after umount), SEC-02
 * (only this device's line is ever touched).
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"
#include "pgw/fs9p.h"
#include "pgw/fs9p_qnx.h"
#include "pgw/log.h"
#include "pgw/virtio.h"

#include <errno.h>
#include <stdint.h>

/* Provided by dispatch_qnx.c: what the interrupt has to wake. */
void fs9p_xport_irq(void *user);

/* The interrupt service thread of this driver (one per mount). */
static pgw_ist g_ist;
static bool g_started;

int fs9p_irq_override(const pgw_mount_opts *opts)
{
    if (opts == NULL || opts->irq < 0)
        return -1; /* let layer 1 pick, MSI-X first */
    return (int)opts->irq;
}

pgw_err fs9p_irq_start(fs9p_dev *dev)
{
    if (dev == NULL || dev->vio == NULL)
        return PGW_EINVAL;
    if (g_started)
        return PGW_OK;

    /* Layer 1 owns the PCI capability walk: it reports whether MSI-X was
     * negotiated and which line the device ends up on (VIO-02, VIO-04). */
    const pgw_vio_info *info = pgw_vio_get_info(dev->vio);
    int irq = fs9p_irq_override(&dev->opts);
    if (irq < 0) {
        if (info == NULL) {
            PGW_ERR("fs9p: no device info, cannot pick an interrupt");
            return PGW_EINVAL;
        }
        if (!info->msix_active)
            PGW_WARN("fs9p: MSI-X unavailable, falling back to INTx (VIO-04)");
        irq = dev->irq;
        if (irq < 0) {
            PGW_ERR("fs9p: the PCI server reported no interrupt line");
            return PGW_ENODEV;
        }
    }
    dev->irq = irq;

    pgw_err e = pgw_ist_start(&g_ist, irq, fs9p_xport_irq, NULL);
    if (e != PGW_OK)
        return e;
    g_started = true;
    PGW_INFO("fs9p: interrupt %d attached (%s)", irq,
             info != NULL && info->msix_active ? "msix" : "intx");
    return PGW_OK;
}

void fs9p_irq_stop(fs9p_dev *dev)
{
    if (dev == NULL)
        return;
    if (g_started) {
        pgw_ist_stop(&g_ist); /* detaches, so nothing stays attached (VIO-09) */
        g_started = false;
    }
    if (dev->vio != NULL)
        pgw_vio_reset(dev->vio); /* quiesce the device before the memory goes */
    dev->irq = -1;
}

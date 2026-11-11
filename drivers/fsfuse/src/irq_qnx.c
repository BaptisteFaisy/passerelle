/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fsfuse/src/irq_qnx.c - interrupt of the virtiofs device, QNX 8.0
 * style.
 *
 * Same shape as the 9P driver: QNX 8.0 removed InterruptAttach(), so the only
 * supported form is an Interrupt Service Thread attached with
 * InterruptAttachThread() (pgw_ist_start() in drivers/common). The line comes
 * from the PCI server: the MSI-X vector when the device offers it, the classic
 * INTx line otherwise, or the one a `-o irq=N` mount option forces (VIO-04).
 *
 * Nothing is allocated on the interrupt path: the callback only bumps the
 * transport's counter, and the owning thread drains the used ring.
 *
 * Covered requirements: VIO-04, VIO-09, SEC-02.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"
#include "pgw/fsfuse.h"
#include "pgw/fsfuse_qnx.h"
#include "pgw/log.h"
#include "pgw/virtio.h"

#include <errno.h>
#include <stdint.h>

static pgw_ist g_ist;
static bool g_started;

int fsfuse_irq_override(const pgw_mount_opts *opts)
{
    if (opts == NULL || opts->irq < 0)
        return -1;
    return (int)opts->irq;
}

pgw_err fsfuse_irq_start(fsfuse_dev *dev)
{
    if (dev == NULL || dev->vio == NULL)
        return PGW_EINVAL;
    if (g_started)
        return PGW_OK;

    const pgw_vio_info *info = pgw_vio_get_info(dev->vio);
    int irq = fsfuse_irq_override(&dev->opts);
    if (irq < 0) {
        if (info == NULL)
            return PGW_EINVAL;
        if (!info->msix_active)
            PGW_WARN("fsfuse: MSI-X unavailable, falling back to INTx (VIO-04)");
        irq = dev->irq;
        if (irq < 0) {
            PGW_ERR("fsfuse: the PCI server reported no interrupt line");
            return PGW_ENODEV;
        }
    }
    dev->irq = irq;

    pgw_err e = pgw_ist_start(&g_ist, irq, fsfuse_xport_irq, NULL);
    if (e != PGW_OK)
        return e;
    g_started = true;
    PGW_INFO("fsfuse: interrupt %d attached (%s)", irq,
             info != NULL && info->msix_active ? "msix" : "intx");
    return PGW_OK;
}

void fsfuse_irq_stop(fsfuse_dev *dev)
{
    if (dev == NULL)
        return;
    if (g_started) {
        pgw_ist_stop(&g_ist); /* detaches, so nothing stays attached (VIO-09) */
        g_started = false;
    }
    if (dev->vio != NULL)
        pgw_vio_reset(dev->vio);
    dev->irq = -1;
}

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/src/pci_msix_qnx.c - QNX 8.0 MSI-X, allocated by the PCI server.
 *
 * On QNX the driver does not write the MSI-X table. The documented sequence is
 * (PCI Server API, "Capability ID 0x11 (MSI-X)"):
 *
 *   1. read the capability module (pci_device_read_cap);
 *   2. choose the disposition of the entries the driver wants
 *      (cap_msix_set_irq_entry: entry i, disposition i = its own vector);
 *   3. pci_device_cfg_cap_enable() - the server allocates the vector and
 *      programs the table entry, including the APIC message;
 *   4. pci_device_read_irq() - the line the vector was bound to, which is what
 *      InterruptAttachThread() takes.
 *
 * Step 3 is why the capability must be disabled again when the allocation
 * fails: a device left with MSI-X enabled signals a vector nobody listens to,
 * and the INTx fallback would never fire. That is not theoretical - the first
 * version of this file left the capability on, and the guest mount stopped
 * answering (measured 14 November 2026).
 *
 * pci_msix_get_irq() is not used: the SDP headers put it under "The following
 * APIs are for ICI support", and this guest has no ICI configuration.
 *
 * Covered requirements: VIO-04 (MSI-X first, INTx as the fallback), VIO-09
 * (nothing stays enabled after a failed attempt).
 */
#include "virtio_priv.h"
#include "pgw/virtio_qnx.h"

#if defined(__QNX__)

#include "pgw/log.h"

#include <pci/cap_msix.h>
#include <stdlib.h>

/* One vector is enough: this driver uses a single virtqueue per device. */
#define PGW_MSIX_ENTRIES 1u

/* How many lines to ask for. pci_device_read_irq() returns an ARBITRARY entry's
 * line when the array holds fewer IRQs than the device has, and nirq comes back
 * as the 2's complement of the number that did not fit. The array therefore has
 * to be at least as large as the device's vector count; 8 covers every virtio
 * device QEMU builds (this one reports 2: config change and the queue). */
#define PGW_MSIX_MAX 8u

static pci_cap_t open_msix(pci_bdf_t bdf)
{
    int_t idx = pci_device_find_capid(bdf, (pci_capid_t)CAPID_MSIX);
    if (idx < 0) {
        PGW_WARN("virtio-pci: msix: no capability on the bus (%d)", (int)idx);
        return NULL;
    }
    pci_cap_t cap = NULL;
    if (pci_device_read_cap(bdf, &cap, (uint_t)idx) != PCI_ERR_OK) {
        PGW_WARN("virtio-pci: msix: the capability module did not load");
        return NULL;
    }
    PGW_INFO("virtio-pci: msix: capability at index %d, %u vectors", (int)idx,
             (unsigned)cap_msix_get_nirq(cap));
    return cap;
}

/* Ask the server for the vector of entry 0 and report the line it was bound to. */
static pgw_err allocate(pci_devhdl_t hdl, pci_cap_t cap, int32_t *out_irq)
{
    uint_t nirq_dev = cap_msix_get_nirq(cap);
    if (nirq_dev < PGW_MSIX_ENTRIES) {
        PGW_WARN("virtio-pci: msix: the device has no vector (%u)", (unsigned)nirq_dev);
        return PGW_ENOENT;
    }
    /* disposition == entry: this source gets its own vector and its own IRQ. */
    pci_err_t e = cap_msix_set_irq_entry(hdl, cap, 0u, 0u);
    if (e != PCI_ERR_OK) {
        PGW_WARN("virtio-pci: msix: disposition refused: %s", pci_strerror(e));
        return PGW_EIO;
    }
    e = pci_device_cfg_cap_enable(hdl, pci_reqType_e_ADVISORY, cap);
    if (e != PCI_ERR_OK) {
        PGW_WARN("virtio-pci: msix: the server did not enable the capability: %s",
                 pci_strerror(e));
        return PGW_EIO;
    }

    pci_irq_t irqs[PGW_MSIX_MAX] = {0};
    int_t nirq = (int_t)PGW_MSIX_MAX;
    e = pci_device_read_irq(hdl, &nirq, irqs);
    if (e != PCI_ERR_OK || nirq < 1) {
        PGW_WARN("virtio-pci: msix: no line after enable (%s, nirq %d)", pci_strerror(e),
                 (int)nirq);
        /* Restore the device, or INTx can never signal again. */
        (void)pci_device_cfg_cap_disable(hdl, pci_reqType_e_ADVISORY, cap);
        return PGW_EIO;
    }
    /* Enabling the capability leaves the entry masked (the server masks it to
     * apply the disposition), so an unmasked vector is what has to be asked for
     * before the interrupt service thread can be woken by it. */
    e = cap_msix_unmask_irq_entry(hdl, cap, 0u);
    if (e != PCI_ERR_OK && e != PCI_ERR_EALREADY) {
        PGW_WARN("virtio-pci: msix: the vector stayed masked: %s", pci_strerror(e));
        (void)pci_device_cfg_cap_disable(hdl, pci_reqType_e_ADVISORY, cap);
        return PGW_EIO;
    }
    *out_irq = (int32_t)irqs[0];
    PGW_INFO("virtio-pci: msix: vector bound to line %d", (int)irqs[0]);
    return PGW_OK;
}

/* Disable the capability the server allocated, so that nothing is left enabled
 * when the mount goes away (VIO-09). */
void pgw_vio_pci_qnx_msix_release(pci_bdf_t bdf, pci_devhdl_t hdl);

/* Disable the capability the server allocated, so that nothing is left enabled
 * when the mount goes away (VIO-09). */
void pgw_vio_pci_qnx_msix_release(pci_bdf_t bdf, pci_devhdl_t hdl)
{
    pci_cap_t cap = open_msix(bdf);
    if (cap == NULL)
        return;
    if (pci_device_cfg_cap_isenabled(hdl, cap))
        (void)pci_device_cfg_cap_disable(hdl, pci_reqType_e_ADVISORY, cap);
    (void)free(cap);
}

pgw_err pgw_vio_pci_qnx_msix(pgw_vio_dev *dev, pci_bdf_t bdf, pci_devhdl_t hdl,
                             int32_t *out_irq)
{
    if (dev == NULL || out_irq == NULL)
        return PGW_EINVAL;

    pci_cap_t cap = open_msix(bdf);
    if (cap == NULL)
        return PGW_ENOENT;

    int32_t irq = -1;
    pgw_err e = allocate(hdl, cap, &irq);
    (void)free(cap);
    if (e != PGW_OK)
        return e;

    dev->info.msix_active = true;
    dev->info.irq = irq;
    *out_irq = irq;
    PGW_INFO("virtio-pci: MSI-X vector allocated by the PCI server (irq %d)", (int)irq);
    return PGW_OK;
}

#endif /* __QNX__ */

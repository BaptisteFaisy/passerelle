/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/include/pgw/virtio_qnx.h - QNX-only declarations of the virtio PCI
 * binding. The portable header deliberately does not include <pci/pci.h>.
 */
#ifndef PGW_VIRTIO_QNX_H
#define PGW_VIRTIO_QNX_H

#if defined(__QNX__)

#include "pgw/virtio.h"

#include <pci/pci.h>
#include <stdint.h>

/* Ask the QNX PCI server to allocate one MSI-X vector for this device and to
 * bind it to an interrupt line. Returns PGW_ENOENT when the device has no MSI-X
 * capability or the server cannot give a vector; the caller must then fall back
 * to INTx. The server programs the table entry itself - the driver never writes
 * the APIC message (see the file comment of pci_msix_qnx.c). */
pgw_err pgw_vio_pci_qnx_msix(pgw_vio_dev *dev, pci_bdf_t bdf, pci_devhdl_t hdl,
                             int32_t *out_irq);

/* Disable the capability the server allocated, so that nothing is left enabled
 * when the mount goes away (VIO-09). */
void pgw_vio_pci_qnx_msix_release(pci_bdf_t bdf, pci_devhdl_t hdl);

#endif /* __QNX__ */
#endif /* PGW_VIRTIO_QNX_H */

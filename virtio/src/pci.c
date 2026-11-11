/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/src/pci.c - PCI probe entry point for builds without a PCI server.
 *
 * The real probe (libpci, BAR mapping, MSI-X wiring) lives in pci_qnx.c and is
 * only compiled when the target is QNX 8.0. Everywhere else - the PC test
 * build, the CI - this file provides the same symbol so that a caller does not
 * need an #ifdef: it logs why it cannot work and returns NULL (ENODEV), as the
 * contract in pgw/virtio.h states.
 *
 * The discovery and register programming logic itself is *not* here: it lives
 * in pci_caps.c and pci_modern.c, behind a configuration-space accessor, and is
 * therefore fully tested on the PC (VIO-02).
 */
#include "virtio_priv.h"

#include "pgw/log.h"

#if !defined(__QNX__)

pgw_vio_dev *pgw_vio_pci_open(uint16_t device_id, const char *expected_tag,
                              bool allow_msix)
{
    (void)expected_tag;
    (void)allow_msix;
    PGW_WARN("virtio-pci: no PCI server in this build (needs QNX 8.0 libpci); "
             "device %u cannot be probed, returning ENODEV",
             (unsigned)device_id);
    return NULL;
}

#endif /* !__QNX__ */

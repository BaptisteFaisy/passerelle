/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Minimal stand-in for <pci/pci.h> of the QNX SDP 8.0, used only by
 * syntax-check.sh to compile the virtio-pci transport on a PC that has no
 * SDP installed.
 *
 * It declares exactly what src/transport_virtio_pci.c calls, with the
 * signatures the QNX 8.0 PCI guide documents. It is NOT a reimplementation:
 * nothing here runs, the goal is to catch typos, wrong argument counts and
 * type errors before an SDP build. Numbers and structure layouts are not
 * used by the check.
 */
#ifndef FS9P_STUB_PCI_PCI_H
#define FS9P_STUB_PCI_PCI_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t pci_bdf_t;
typedef void *pci_devhdl_t;
typedef int pci_err_t;
typedef uint16_t pci_cmd_t;
typedef uint16_t pci_irq_t;
typedef uint32_t pci_cap_t;
typedef unsigned int int_t;
typedef unsigned int uint_t;

#define PCI_BDF_NONE 0xFFFFFFFFu
#define PCI_CCODE_ANY 0xFFFFFFFFu
#define PCI_ERR_OK 0
#define PCI_ERR_NO_MODULE 6

typedef enum {
    pci_attachFlags_DEFAULT = 0,
    pci_attachFlags_OWNER = 1
} pci_attachFlags_e;

/* QNX 8.0 spells the enumerator pci_attachFlags_e_OWNER; both spellings are
 * accepted by the real header's compatibility macro, so the stand-in defines
 * the one the transport uses. */
#define pci_attachFlags_e_OWNER pci_attachFlags_OWNER

typedef enum {
    pci_reqType_e_UNSPECIFIED = 0,
    pci_reqType_e_MANDATORY = 1
} pci_reqType_e;

typedef struct {
    uint_t bar_num;
    uint64_t addr;
    uint64_t size;
    uint_t type;
} pci_ba_t;

pci_bdf_t pci_device_find(uint_t index, uint16_t vid, uint16_t did, uint32_t ccode);
pci_devhdl_t pci_device_attach(pci_bdf_t bdf, pci_attachFlags_e flags, pci_err_t *err);
void pci_device_detach(pci_devhdl_t hdl);
pci_err_t pci_device_read_cmd(pci_bdf_t bdf, pci_cmd_t *cmd);
pci_err_t pci_device_write_cmd(pci_devhdl_t hdl, pci_cmd_t cmd, pci_cmd_t *after);
pci_err_t pci_device_read_irq(pci_devhdl_t hdl, int_t *nirq, pci_irq_t *irqs);
pci_err_t pci_device_read_ba(pci_devhdl_t hdl, int_t *nba, pci_ba_t *bars, pci_reqType_e req);
pci_err_t pci_device_cfg_rd32(pci_devhdl_t hdl, uint32_t off, uint32_t *dst);
pci_err_t pci_device_cfg_rd16(pci_devhdl_t hdl, uint32_t off, uint16_t *dst);
pci_err_t pci_device_cfg_rd8(pci_devhdl_t hdl, uint32_t off, uint8_t *dst);
pci_err_t pci_device_cfg_wr16(pci_devhdl_t hdl, uint32_t off, uint16_t val, uint16_t *after);
int pci_device_find_capid(pci_bdf_t bdf, uint8_t capid);
pci_err_t pci_device_read_cap(pci_bdf_t bdf, pci_cap_t *cap, int index);
const char *pci_strerror(pci_err_t err);

#endif /* FS9P_STUB_PCI_PCI_H */

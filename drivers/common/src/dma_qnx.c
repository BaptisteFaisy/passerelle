/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/dma_qnx.c - the physical address of a buffer, for DMA.
 *
 * A virtio descriptor carries the address the *device* will read or write.
 * On the PC that address is the virtual one; on QNX the device addresses
 * physical memory, so a descriptor built from a virtual pointer makes the
 * device read the wrong pages and the request is never answered (VIO-07).
 *
 * One helper for both drivers, because both hand their request and reply
 * buffers to the same kind of transport.
 *
 * Covered requirements: VIO-07 (the buffers are the caller's), SEC-02 (no
 * extra privilege is needed: the physical address of memory the process owns
 * is always readable).
 */
#define _POSIX_C_SOURCE 200809L
#define _QNX_SOURCE 1
#define _LARGEFILE64_SOURCE 1 /* mem_offset64() lives behind __EXT_LF64SRC */

#include "pgw/drvsrv.h"
#include "pgw/log.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>

/* Page size used to round the allocations. */
#define PGW_QNX_PAGE 4096u

/* Memory a device can read and write: physically contiguous, non-cached.
 * MAP_ANON|MAP_PHYS asks QNX for a contiguous physical range, which is what a
 * single descriptor needs (VIO-07). */
void *pgw_qnx_alloc_dma(size_t bytes, void *user)
{
    (void)user;
    size_t len = (bytes + PGW_QNX_PAGE - 1u) & ~(size_t)(PGW_QNX_PAGE - 1u);
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                   MAP_ANON | MAP_PHYS | MAP_SHARED, NOFD, 0);
    if (p == MAP_FAILED) {
        PGW_ERR("dma: cannot map %zu contiguous bytes: %s", len,
                strerror(errno));
        return NULL;
    }
    memset(p, 0, len);
    return p;
}

void pgw_qnx_free_dma(void *ptr, size_t bytes, void *user)
{
    (void)user;
    if (ptr == NULL)
        return;
    munmap(ptr, (bytes + PGW_QNX_PAGE - 1u) & ~(size_t)(PGW_QNX_PAGE - 1u));
}

/* Physical address of `len` bytes at `addr`.
 *
 * Returns PGW_OK and fills *out_pa when the whole range is physically
 * contiguous, which is what a single descriptor needs. A range that spans
 * non-contiguous pages is refused rather than silently truncated: the device
 * would read the wrong page half way through. */
pgw_err pgw_qnx_phys(void *addr, size_t len, uint64_t *out_pa)
{
    if (addr == NULL || out_pa == NULL || len == 0u)
        return PGW_EINVAL;

    off64_t phys = 0;
    size_t contig = 0;
    /* mem_offset64() with NOFD gives the physical offset of the page that
     * holds `addr` and how far the mapping stays contiguous from there. */
    if (mem_offset64(addr, NOFD, len, &phys, &contig) != 0) {
        PGW_ERR("dma: cannot map the buffer to a physical address: %s",
                strerror(errno));
        return PGW_EIO;
    }
    if (contig < len) {
        PGW_ERR("dma: %zu bytes are not physically contiguous (%zu are)",
                len, contig);
        return PGW_EIO;
    }
    *out_pa = (uint64_t)phys;
    return PGW_OK;
}

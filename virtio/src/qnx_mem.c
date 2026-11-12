/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/src/qnx_mem.c - QNX 8.0 helpers shared by the PCI and MMIO bindings.
 *
 * Two things every QNX virtio transport needs and neither may duplicate:
 *   - physically contiguous, non-cached ring memory, because the device reads
 *     the descriptor table and the rings by DMA address (VIO-03, VIO-07);
 *   - interrupt attach/detach through the QNX 8.0 thread-based interface
 *     (VIO-04), with a detach that always runs on reset (VIO-09).
 *
 * Compiled only when the target is QNX.
 */
#include "virtio_priv.h"

#if defined(__QNX__)

#include "pgw/log.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sched.h>
#include <sys/neutrino.h>

#define PGW_QNX_PAGE 4096u

void *pgw_qnx_vq_alloc(pgw_vio_dev *dev, size_t bytes, size_t align,
                       uint64_t *out_pa)
{
    (void)dev;
    (void)align;
    size_t len = (bytes + PGW_QNX_PAGE - 1u) & ~(size_t)(PGW_QNX_PAGE - 1u);
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                   MAP_PHYS | MAP_ANON | MAP_SHARED, NOFD, 0);
    if (p == MAP_FAILED) {
        PGW_ERR("virtio: cannot map %zu contiguous bytes: %s", len,
                strerror(errno));
        return NULL;
    }
    off64_t phys = 0;
    size_t contig = 0;
    if (mem_offset64(p, NOFD, len, &phys, &contig) != 0 || contig < len) {
        PGW_ERR("virtio: ring memory is not physically contiguous");
        munmap(p, len);
        return NULL;
    }
    memset(p, 0, len);
    if (out_pa != NULL)
        *out_pa = (uint64_t)phys;
    return p;
}

void pgw_qnx_vq_free(pgw_vio_dev *dev, void *ptr, size_t bytes)
{
    (void)dev;
    if (ptr == NULL)
        return;
    munmap(ptr, (bytes + PGW_QNX_PAGE - 1u) & ~(size_t)(PGW_QNX_PAGE - 1u));
}

int32_t pgw_qnx_irq_attach(int32_t irq, int32_t prio)
{
    if (ThreadCtl(_NTO_TCTL_IO, NULL) == -1) {
        PGW_ERR("virtio: ThreadCtl(_NTO_TCTL_IO) failed: %s", strerror(errno));
        return -1;
    }
    /* QNX 8: InterruptAttachThread() takes tracking flags, not a priority (the
     * priority-free IST API arrived in the 8.0 SDK). The priority is set on
     * the calling thread first, which becomes the interrupt thread. */
    if (prio > 0) {
        struct sched_param sp;
        memset(&sp, 0, sizeof sp);
        sp.sched_priority = prio;
        (void)SchedSet(0, 0, SCHED_FIFO, &sp);
    }
    int32_t id = InterruptAttachThread(irq, 0);
    if (id < 0)
        PGW_ERR("virtio: InterruptAttachThread(%d) failed: %s", (int)irq,
                strerror(errno));
    return id;
}

void pgw_qnx_irq_detach(int32_t *irq_id)
{
    if (irq_id != NULL && *irq_id > 0) {
        InterruptDetach(*irq_id);
        *irq_id = -1;
    }
}

#endif /* __QNX__ */

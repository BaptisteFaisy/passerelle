/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/src/mmio_qnx.c - QNX 8.0 binding for virtio-mmio (VIO-03, VIO-04).
 *
 * Compiled only when the target is QNX. It maps a physical MMIO window into the
 * process with mmap_device_io(), wraps it in the portable register accessor of
 * pgw_mmio_io, and lets mmio.c - which is unit-tested on the PC against a
 * RAM-backed window - do the whole v1/v2 discovery and queue programming.
 *
 * Interrupts come through the thread-based interface of QNX 8.0, attached once
 * and detached on reset (VIO-09).
 */
#include "virtio_priv.h"

#if defined(__QNX__)

#include "pgw/log.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>

#define PGW_QNX_MMIO_WINDOW 0x1000u
#define PGW_QNX_IRQ_PRIO    21

typedef struct qnx_mmio {
    void *va;
    size_t len;
    int32_t irq;
    int32_t irq_id;
} qnx_mmio;

static uint32_t qnx_mmio_r32(void *ctx, uint32_t off)
{
    qnx_mmio *m = (qnx_mmio *)ctx;
    if (m == NULL || m->va == NULL || off + 4u > m->len)
        return 0;
    return *(volatile uint32_t *)((uint8_t *)m->va + off);
}

static void qnx_mmio_w32(void *ctx, uint32_t off, uint32_t val)
{
    qnx_mmio *m = (qnx_mmio *)ctx;
    if (m == NULL || m->va == NULL || off + 4u > m->len)
        return;
    *(volatile uint32_t *)((uint8_t *)m->va + off) = val;
}

static void qnx_mmio_release(pgw_vio_dev *dev)
{
    qnx_mmio *m = (qnx_mmio *)dev->backend;
    if (m == NULL)
        return;
    pgw_qnx_irq_detach(&m->irq_id);
}

pgw_vio_dev *pgw_vio_mmio_open_phys(uint64_t paddr, int32_t irq,
                                    const char *expected_tag)
{
    void *va = (void *)(uintptr_t)mmap_device_io(PGW_QNX_MMIO_WINDOW, paddr);
    if (va == MAP_FAILED) {
        PGW_ERR("virtio-mmio: cannot map 0x%llx: %s",
                (unsigned long long)paddr, strerror(errno));
        return NULL;
    }

    qnx_mmio *m = calloc(1u, sizeof(*m));
    if (m == NULL) {
        munmap_device_io((uintptr_t)va, PGW_QNX_MMIO_WINDOW);
        return NULL;
    }
    m->va = va;
    m->len = PGW_QNX_MMIO_WINDOW;
    m->irq = irq;
    m->irq_id = -1;

    pgw_mmio_io io = {qnx_mmio_r32, qnx_mmio_w32, m};
    pgw_vio_dev *dev = pgw_vio_mmio_open(io, expected_tag);
    if (dev == NULL) {
        munmap_device_io((uintptr_t)va, PGW_QNX_MMIO_WINDOW);
        free(m);
        return NULL;
    }

    /* A legacy queue is addressed by page frame number: the rings must be
     * physically contiguous (VIO-03). */
    dev->vq_alloc = pgw_qnx_vq_alloc;
    dev->vq_free = pgw_qnx_vq_free;
    dev->irq_release = qnx_mmio_release;

    if (irq >= 0) {
        m->irq_id = pgw_qnx_irq_attach(irq, PGW_QNX_IRQ_PRIO);
        if (m->irq_id <= 0) {
            pgw_vio_destroy(dev);
            return NULL;
        }
        dev->irq_attached = true;
    }
    return dev;
}

#endif /* __QNX__ */

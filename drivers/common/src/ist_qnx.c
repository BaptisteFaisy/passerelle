/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/ist_qnx.c - interrupt service thread (QNX 8.0 style).
 *
 * QNX 8.0 removed InterruptAttach(): a handler can no longer run in interrupt
 * context. The supported shape is an Interrupt Service Thread, created with
 * InterruptAttachThread(), that waits with InterruptWait() and unmasks the line
 * when it is done. That is the only form used here, on both the MSI-X path and
 * the INTx fallback, so the driver never depends on a removed kernel call
 * (VIO-04).
 *
 * No allocation happens in the loop: the callback drains the used ring of the
 * virtqueue, which layer 1 owns.
 */
/* The project compiles with -std=c11 (no GNU extensions). QNX ignores these
 * feature macros: its own headers always expose S_IF*, strnlen and the POSIX
 * realtime interfaces. The host needs them declared to build this file against
 * the stubs in tests/qnx-stub, so both are spelled out here once. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "pgw/drvsrv.h"

#include "pgw/log.h"

#include <errno.h>
#include <string.h>
#include <stddef.h>

/* -2: the attach has not happened yet (pgw_ist_stop() waits for it). */
#define IST_PENDING (-2)

#define IST_STACK 8192u

static void *ist_main(void *arg)
{
    pgw_ist *ist = arg;
    ist->id = InterruptAttachThread(ist->irq, _NTO_INTR_FLAGS_NO_UNMASK);
    if (ist->id == -1) {
        PGW_ERR("irq %d: attach failed: %s", ist->irq, strerror(errno));
        ist->running = false;
        return NULL;
    }
    PGW_INFO("irq %d: interrupt service thread attached", ist->irq);

    while (ist->running) {
        if (InterruptWait(0, NULL) == -1)
            break; /* detached by pgw_ist_stop() */
        if (!ist->running)
            break;
        ist->on_irq(ist->user);
        (void)InterruptUnmask(ist->irq, ist->id);
    }
    return NULL;
}

pgw_err pgw_ist_start(pgw_ist *ist, int irq, void (*on_irq)(void *user),
                      void *user)
{
    if (ist == NULL || on_irq == NULL || irq < 0)
        return PGW_EINVAL;
    ist->irq = irq;
    ist->id = IST_PENDING;
    ist->thread = -1;
    ist->on_irq = on_irq;
    ist->user = user;
    ist->running = true;
    ist->thread = ThreadCreate(ist_main, ist, IST_STACK);
    if (ist->thread < 0) {
        ist->running = false;
        return pgw_code_from_host(errno);
    }
    /* Wait for the attach so a caller that mounts and immediately unmounts
     * never detaches an interrupt that does not exist yet. */
    for (unsigned i = 0; i < 1000u && ist->id == IST_PENDING; i++)
        (void)delay(1);
    if (ist->id == IST_PENDING || !ist->running)
        return PGW_EIO;
    return PGW_OK;
}

void pgw_ist_stop(pgw_ist *ist)
{
    if (ist == NULL)
        return;
    ist->running = false;
    if (ist->id >= 0) {
        (void)InterruptDetach(ist->id); /* wakes InterruptWait() */
        ist->id = -1;
    }
    ist->thread = -1;
}

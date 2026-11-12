/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/ist_qnx.c - interrupt service thread (QNX 8.0 style).
 *
 * QNX 8.0 removed InterruptAttach(): a handler can no longer run in interrupt
 * context. The supported shape is an Interrupt Service Thread, created as a
 * detached pthread, that waits with InterruptWait() and unmasks the line when
 * it is done. That is the only form used here, on both the MSI-X path and the
 * INTx fallback, so the driver never depends on a removed kernel call (VIO-04).
 *
 * QNX 8's ThreadCreate() takes an opaque struct _thread_attr that no public
 * header defines, and the C Library Reference says not to call it directly; the
 * detached pthread here is the same shape the drivers already use for the
 * shutdown thread (drivers/fs9p/src/main.c).
 *
 * No allocation happens in the loop: the callback drains the used ring of the
 * virtqueue, which layer 1 owns.
 */
/* The project compiles with -std=c11 (no GNU extensions). The feature macros
 * below make the POSIX realtime interfaces visible on the host build (which
 * compiles this file against tests/qnx-stub) and fix the rest of the POSIX
 * surface on QNX. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "pgw/drvsrv.h"

#include "pgw/log.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

/* -2: the attach has not happened yet (pgw_ist_stop() waits for it). */
#define IST_PENDING (-2)

#define IST_STACK 8192u

/* Wait 1 ms. delay() is a QNX extension gated behind _QNX_SOURCE; nanosleep()
 * is POSIX and is available on both the target and the host build. */
static void ist_yield(void)
{
    struct timespec ts = { 0, 1000000L };
    (void)nanosleep(&ts, NULL);
}

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

/* A detached pthread with an explicit stack size (see the file comment for why
 * this is a pthread and not the raw ThreadCreate()). Returns the pthread error
 * code, 0 on success. */
static int ist_thread_start(void *(*fn)(void *), void *arg)
{
    pthread_attr_t tattr;
    pthread_t tid;
    int rc = pthread_attr_init(&tattr);
    if (rc == 0)
        rc = pthread_attr_setstacksize(&tattr, IST_STACK);
    if (rc == 0)
        rc = pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
    if (rc == 0)
        rc = pthread_create(&tid, &tattr, fn, arg);
    (void)pthread_attr_destroy(&tattr);
    return rc;
}

pgw_err pgw_ist_start(pgw_ist *ist, int irq, void (*on_irq)(void *user),
                      void *user)
{
    if (ist == NULL || on_irq == NULL || irq < 0)
        return PGW_EINVAL;
    ist->irq = irq;
    ist->id = IST_PENDING;
    ist->on_irq = on_irq;
    ist->user = user;
    ist->running = true;
    int rc = ist_thread_start(ist_main, ist);
    if (rc != 0) {
        ist->running = false;
        PGW_ERR("ist: cannot create the interrupt thread: %s", strerror(rc));
        return pgw_code_from_host(rc);
    }
    /* Wait for the attach so a caller that mounts and immediately unmounts
     * never detaches an interrupt that does not exist yet. */
    for (unsigned i = 0; i < 1000u && ist->id == IST_PENDING; i++)
        ist_yield();
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
}
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * clab_irq.h - chapter 6's exercise: which interrupt mechanism to use, and the
 * order of operations around it.
 *
 * QNX 8.0 removed InterruptAttach(): a handler is no longer a function the
 * kernel calls in interrupt context, it is a *thread* the kernel wakes. The two
 * remaining forms are InterruptAttachThread() (the calling thread becomes the
 * interrupt thread) and InterruptAttachEvent() (the kernel sends an event, and
 * some other thread receives it).
 *
 * A virtio driver must use the thread form, for a reason that has nothing to do
 * with style: requests are submitted from resource-manager threads, so the
 * thread that attaches is not the thread that waits, and the interrupt thread
 * must be able to take a mutex and wake the waiters. That is exactly what
 * InterruptAttachThread() allows and an interrupt-context handler does not.
 *
 * This file is the decision logic, with no QNX header, so it can be tested: it
 * takes the outcomes of the calls as inputs and says what to do. The call sites
 * themselves (chapter 6's listing) are the three lines it wraps.
 */
#ifndef CLAB_IRQ_H
#define CLAB_IRQ_H

#include <stdbool.h>
#include <stdint.h>

/* The interrupt strategies, in the order a driver should try them. */
typedef enum {
    CLAB_IRQ_MSIX = 0, /* one vector per queue, programmed through PCI */
    CLAB_IRQ_INTX = 1, /* the classic line, shared and level-triggered */
    CLAB_IRQ_POLL = 2  /* no interrupt at all: the used ring is polled */
} clab_irq_mode;

/* Why a mode was refused, for the log line that explains the fallback. */
typedef enum {
    CLAB_IRQ_OK = 0,
    CLAB_IRQ_ERR_NO_CAP = -1,     /* the device has no MSI-X capability */
    CLAB_IRQ_ERR_NO_VECTOR = -2,  /* the kernel had no MSI vector to give */
    CLAB_IRQ_ERR_NO_TABLE = -3,   /* the MSI-X table BAR could not be mapped */
    CLAB_IRQ_ERR_REJECTED = -4,   /* the device refused the vector */
    CLAB_IRQ_ERR_NO_LINE = -5,    /* no INTx line was assigned */
    CLAB_IRQ_ERR_ATTACH = -6      /* InterruptAttachThread() failed */
} clab_irq_err;

/* What the driver learned while trying a mode. */
typedef struct {
    bool capability_present; /* an MSI-X capability exists */
    bool table_mapped;       /* the table BAR is mapped */
    bool vector_allocated;   /* rsrcdbmgr gave us an IRQ */
    bool device_accepted;    /* queue_msix_vector reads back what we wrote */
    bool line_assigned;      /* the firmware assigned an INTx line */
    int attach_errno;        /* 0 when InterruptAttachThread() succeeded */
} clab_irq_facts;

/* Pick the mode: MSI-X when every fact allows it, INTx next, polling last. */
clab_irq_mode clab_irq_choose(const clab_irq_facts *facts);

/* Why `mode` was not usable, for the log. CLAB_IRQ_OK when it was. */
clab_irq_err clab_irq_why(const clab_irq_facts *facts, clab_irq_mode mode);

/* The order the interrupt thread must follow, as a small state machine.
 *
 *   ATTACH -> WAIT -> ACK -> REAP -> (WAIT ...)
 *
 * ACK comes before REAP on purpose: a level-triggered line stays asserted until
 * the device is acknowledged, and reaping first would let the line be re-armed
 * while the device still holds it, which is how interrupt storms start.
 */
typedef enum {
    CLAB_IST_ATTACH = 0,
    CLAB_IST_WAIT = 1,
    CLAB_IST_ACK = 2,
    CLAB_IST_REAP = 3,
    CLAB_IST_DONE = 4
} clab_ist_step;

/* True when `from -> to` is a legal transition. */
bool clab_ist_transition_ok(clab_ist_step from, clab_ist_step to);

/* Name of a step or an error, for the exercise's output. */
const char *clab_irq_mode_name(clab_irq_mode mode);
const char *clab_irq_err_name(clab_irq_err err);

#endif /* CLAB_IRQ_H */

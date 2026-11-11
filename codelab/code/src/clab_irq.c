/* SPDX-License-Identifier: Apache-2.0 */
/*
 * clab_irq.c - chapter 6's exercise, implementation.
 *
 * The choice itself is four lines; what is worth reading is *why* each fact
 * matters, because each one is a way the MSI-X path fails on a real machine and
 * each one has to degrade to INTx rather than to a hang.
 */
#include "clab_irq.h"

#include <stddef.h>

clab_irq_err clab_irq_why(const clab_irq_facts *facts, clab_irq_mode mode)
{
    if (facts == NULL)
        return CLAB_IRQ_ERR_ATTACH;

    switch (mode) {
    case CLAB_IRQ_MSIX:
        if (!facts->capability_present)
            return CLAB_IRQ_ERR_NO_CAP;
        if (!facts->table_mapped)
            return CLAB_IRQ_ERR_NO_TABLE;
        if (!facts->vector_allocated)
            return CLAB_IRQ_ERR_NO_VECTOR;
        if (!facts->device_accepted)
            return CLAB_IRQ_ERR_REJECTED;
        return CLAB_IRQ_OK;
    case CLAB_IRQ_INTX:
        if (!facts->line_assigned)
            return CLAB_IRQ_ERR_NO_LINE;
        return facts->attach_errno == 0 ? CLAB_IRQ_OK : CLAB_IRQ_ERR_ATTACH;
    case CLAB_IRQ_POLL:
        return CLAB_IRQ_OK; /* always possible: it costs CPU, nothing else */
    default:
        return CLAB_IRQ_ERR_ATTACH;
    }
}

clab_irq_mode clab_irq_choose(const clab_irq_facts *facts)
{
    if (clab_irq_why(facts, CLAB_IRQ_MSIX) == CLAB_IRQ_OK)
        return CLAB_IRQ_MSIX;
    if (clab_irq_why(facts, CLAB_IRQ_INTX) == CLAB_IRQ_OK)
        return CLAB_IRQ_INTX;
    return CLAB_IRQ_POLL;
}

bool clab_ist_transition_ok(clab_ist_step from, clab_ist_step to)
{
    switch (from) {
    case CLAB_IST_ATTACH:
        return to == CLAB_IST_WAIT || to == CLAB_IST_DONE;
    case CLAB_IST_WAIT:
        /* A wait can return because of the device (ACK next) or because the
         * driver asked the thread to stop (DONE next). */
        return to == CLAB_IST_ACK || to == CLAB_IST_DONE;
    case CLAB_IST_ACK:
        return to == CLAB_IST_REAP;
    case CLAB_IST_REAP:
        return to == CLAB_IST_WAIT;
    case CLAB_IST_DONE:
        return false;
    default:
        return false;
    }
}

const char *clab_irq_mode_name(clab_irq_mode mode)
{
    switch (mode) {
    case CLAB_IRQ_MSIX:
        return "MSI-X";
    case CLAB_IRQ_INTX:
        return "INTx";
    case CLAB_IRQ_POLL:
        return "polling";
    default:
        return "?";
    }
}

const char *clab_irq_err_name(clab_irq_err err)
{
    switch (err) {
    case CLAB_IRQ_OK:
        return "ok";
    case CLAB_IRQ_ERR_NO_CAP:
        return "no MSI-X capability";
    case CLAB_IRQ_ERR_NO_VECTOR:
        return "no MSI vector available";
    case CLAB_IRQ_ERR_NO_TABLE:
        return "MSI-X table BAR not mapped";
    case CLAB_IRQ_ERR_REJECTED:
        return "device rejected the vector";
    case CLAB_IRQ_ERR_NO_LINE:
        return "no INTx line assigned";
    case CLAB_IRQ_ERR_ATTACH:
        return "InterruptAttachThread failed";
    default:
        return "?";
    }
}

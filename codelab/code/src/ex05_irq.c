/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ex05_irq.c - chapter 6's exercise: pick an interrupt mode from what the
 * machine allows, and check the order the interrupt thread must follow.
 *
 * The point of the exercise is the fallback ladder. On a QNX x86_64 image with
 * DO_BUS_CONFIG=no, the PCI server allocates no MSI vectors, so MSI-X either
 * works by being programmed by hand or does not work at all; a driver that
 * treats "MSI-X failed" as fatal is a driver that does not start on that image.
 */
#include <stdio.h>

#include "clab_check.h"
#include "clab_irq.h"

/* Print the mode the facts lead to, and why. */
static void evaluate(const char *label, const clab_irq_facts *facts)
{
    const clab_irq_mode mode = clab_irq_choose(facts);

    printf("  %-38s -> %-8s (MSI-X refused: %s)\n", label,
           clab_irq_mode_name(mode), clab_irq_err_name(clab_irq_why(facts, CLAB_IRQ_MSIX)));
    CLAB_CHECK(clab_irq_why(facts, mode) == CLAB_IRQ_OK);
}

/* The facts of a machine where everything works. */
static clab_irq_facts working(void)
{
    clab_irq_facts f = {.capability_present = true,
                        .table_mapped = true,
                        .vector_allocated = true,
                        .device_accepted = true,
                        .line_assigned = true,
                        .attach_errno = 0};
    return f;
}

/* Each way the MSI-X path can fail, and the mode it must degrade to. */
static void check_fallbacks(void)
{
    clab_irq_facts f;

    f = working();
    evaluate("everything works", &f);
    CLAB_CHECK_EQ(clab_irq_choose(&f), CLAB_IRQ_MSIX);

    /* The QNX case the fallback exists for: no MSI vector to be had. */
    f = working();
    f.vector_allocated = false;
    evaluate("no MSI vector (DO_BUS_CONFIG=no)", &f);
    CLAB_CHECK_EQ(clab_irq_choose(&f), CLAB_IRQ_INTX);
    CLAB_CHECK_EQ(clab_irq_why(&f, CLAB_IRQ_MSIX), CLAB_IRQ_ERR_NO_VECTOR);

    f = working();
    f.table_mapped = false;
    evaluate("MSI-X table BAR not mapped", &f);
    CLAB_CHECK_EQ(clab_irq_choose(&f), CLAB_IRQ_INTX);

    f = working();
    f.device_accepted = false;
    evaluate("device rejected the vector", &f);
    CLAB_CHECK_EQ(clab_irq_choose(&f), CLAB_IRQ_INTX);

    f = working();
    f.capability_present = false;
    evaluate("device has no MSI-X capability", &f);
    CLAB_CHECK_EQ(clab_irq_choose(&f), CLAB_IRQ_INTX);
}

/* When nothing works at all: polling is slow, but it is not a failure. */
static void check_last_resort(void)
{
    clab_irq_facts nothing = {0};

    evaluate("no vector, no line", &nothing);
    CLAB_CHECK_EQ(clab_irq_choose(&nothing), CLAB_IRQ_POLL);

    /* An INTx line exists but attaching failed: still polling. */
    clab_irq_facts attach_failed = nothing;
    attach_failed.line_assigned = true;
    attach_failed.attach_errno = 1;
    evaluate("INTx line, attach failed", &attach_failed);
    CLAB_CHECK_EQ(clab_irq_choose(&attach_failed), CLAB_IRQ_POLL);
}

/* The order the interrupt thread must follow, and the two that are bugs. */
static void check_thread_order(void)
{
    CLAB_CHECK(clab_ist_transition_ok(CLAB_IST_ATTACH, CLAB_IST_WAIT));
    CLAB_CHECK(clab_ist_transition_ok(CLAB_IST_WAIT, CLAB_IST_ACK));
    CLAB_CHECK(clab_ist_transition_ok(CLAB_IST_ACK, CLAB_IST_REAP));
    CLAB_CHECK(clab_ist_transition_ok(CLAB_IST_REAP, CLAB_IST_WAIT));
    CLAB_CHECK(clab_ist_transition_ok(CLAB_IST_WAIT, CLAB_IST_DONE));
    /* The two orders that cause interrupt storms or lost completions. */
    CLAB_CHECK(!clab_ist_transition_ok(CLAB_IST_WAIT, CLAB_IST_REAP));
    CLAB_CHECK(!clab_ist_transition_ok(CLAB_IST_ACK, CLAB_IST_WAIT));
    CLAB_CHECK(!clab_ist_transition_ok(CLAB_IST_DONE, CLAB_IST_WAIT));
    printf("interrupt thread order:\n");
    printf("  ATTACH -> WAIT -> ACK -> REAP -> WAIT ... -> DONE\n");
}

int main(void)
{
    printf("chapter 6 - interrupts\n");
    check_fallbacks();
    check_last_resort();
    check_thread_order();
    return clab_result();
}

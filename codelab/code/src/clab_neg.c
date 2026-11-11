/* SPDX-License-Identifier: Apache-2.0 */
/*
 * clab_neg.c - chapter 4's exercise, implementation.
 *
 * The three rules this file exists to demonstrate:
 *
 *  - every status write is cumulative, and the order is fixed: ACK, DRIVER,
 *    features, FEATURES_OK, DRIVER_OK. Writing DRIVER without ACK is a bug that
 *    QEMU tolerates and the specification does not;
 *  - the driver offers a *subset* of what the device advertises. Offering a bit
 *    the device did not advertise is undefined behaviour; the AND is not
 *    politeness, it is the protocol;
 *  - FEATURES_OK must be read back. A device that cannot live with the driver's
 *    selection clears it, and the driver must give up (or offer less), not
 *    carry on.
 */
#include "clab_neg.h"

#include <stdio.h>

void clab_neg_init(clab_neg_dev *dev, uint64_t offered)
{
    dev->offered = offered;
    dev->accepted = 0;
    dev->status = 0;
    dev->reject_features = false;
    dev->needs_reset = false;
    dev->resets = 0;
}

clab_neg_err clab_neg_reset(clab_neg_dev *dev)
{
    dev->status = 0;
    dev->accepted = 0;
    dev->resets++;
    /* 3.1.1: the driver must see 0 before it goes on. A real device may take a
     * moment to quiesce; here the model is instant. */
    return dev->status == 0 ? CLAB_NEG_OK : CLAB_NEG_ERR_ORDER;
}

clab_neg_err clab_neg_acknowledge(clab_neg_dev *dev)
{
    dev->status = CLAB_STATUS_ACK;
    dev->status = (uint8_t)(dev->status | CLAB_STATUS_DRIVER);
    return CLAB_NEG_OK;
}

clab_neg_err clab_neg_features(clab_neg_dev *dev, uint64_t wanted,
                               uint64_t *out_negotiated)
{
    if ((dev->status & (CLAB_STATUS_ACK | CLAB_STATUS_DRIVER)) !=
        (CLAB_STATUS_ACK | CLAB_STATUS_DRIVER))
        return CLAB_NEG_ERR_ORDER;
    /* A modern device must offer VIRTIO_F_VERSION_1; without it the driver
     * would be speaking the legacy interface, which this exercise does not
     * model. */
    if ((dev->offered & CLAB_F_VERSION_1) == 0u)
        return CLAB_NEG_ERR_UNSUPPORTED;

    const uint64_t negotiated = wanted & dev->offered; /* never offer more */
    dev->accepted = negotiated;
    if (out_negotiated != NULL)
        *out_negotiated = negotiated;

    dev->status = (uint8_t)(dev->status | CLAB_STATUS_FEATURES_OK);
    if (dev->reject_features) {
        dev->status = (uint8_t)(dev->status & ~CLAB_STATUS_FEATURES_OK);
        return CLAB_NEG_ERR_FEATURES;
    }
    return CLAB_NEG_OK;
}

clab_neg_err clab_neg_ready(clab_neg_dev *dev)
{
    if ((dev->status & CLAB_STATUS_FEATURES_OK) == 0u)
        return CLAB_NEG_ERR_ORDER;
    dev->status = (uint8_t)(dev->status | CLAB_STATUS_DRIVER_OK);
    return CLAB_NEG_OK;
}

clab_neg_err clab_neg_check_health(const clab_neg_dev *dev)
{
    if ((dev->status & CLAB_STATUS_FAILED) != 0u)
        return CLAB_NEG_ERR_ORDER;
    if ((dev->status & CLAB_STATUS_NEEDS_RESET) != 0u || dev->needs_reset)
        return CLAB_NEG_ERR_NEEDS_RESET;
    return CLAB_NEG_OK;
}

const char *clab_neg_status_str(uint8_t status, char *buf, unsigned cap)
{
    static const struct {
        uint8_t bit;
        const char *name;
    } names[] = {
        {CLAB_STATUS_ACK, "ACK"},
        {CLAB_STATUS_DRIVER, "DRIVER"},
        {CLAB_STATUS_FEATURES_OK, "FEATURES_OK"},
        {CLAB_STATUS_DRIVER_OK, "DRIVER_OK"},
        {CLAB_STATUS_NEEDS_RESET, "NEEDS_RESET"},
        {CLAB_STATUS_FAILED, "FAILED"},
    };

    buf[0] = '\0';
    unsigned used = 0;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        if ((status & names[i].bit) == 0u)
            continue;
        const int n = snprintf(buf + used, cap - used, "%s%s",
                               used == 0u ? "" : "|", names[i].name);
        if (n < 0 || (unsigned)n >= cap - used)
            break;
        used += (unsigned)n;
    }
    if (used == 0u)
        snprintf(buf, cap, "0");
    return buf;
}

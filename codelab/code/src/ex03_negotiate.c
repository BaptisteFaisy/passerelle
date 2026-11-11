/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ex03_negotiate.c - chapter 4's exercise: run the status sequence, print the
 * status byte after every step, and watch what happens when the device refuses
 * the driver's feature selection.
 *
 * Three runs, on purpose:
 *   1. a device that offers everything the driver wants;
 *   2. a device that offers only some of it: the driver must keep the
 *      intersection, not what it asked for;
 *   3. a device that clears FEATURES_OK: the driver must stop and reset, not
 *      carry on to DRIVER_OK.
 */
#include <stdint.h>
#include <stdio.h>

#include "clab_check.h"
#include "clab_neg.h"

static void show(const char *what, const clab_neg_dev *dev)
{
    char buf[64];

    printf("  %-22s status 0x%02x  %s\n", what, dev->status,
           clab_neg_status_str(dev->status, buf, sizeof buf));
}

/* The wanted set: version 1 (mandatory on the modern interface), the 9P mount
 * tag, indirect descriptors and event index. */
#define WANTED (CLAB_F_VERSION_1 | CLAB_F_MOUNT_TAG | CLAB_F_INDIRECT | CLAB_F_EVENT_IDX)

static int run(const char *title, uint64_t offered, int reject)
{
    clab_neg_dev dev;
    uint64_t got = 0;
    char buf[64];

    printf("%s\n", title);
    clab_neg_init(&dev, offered);
    dev.reject_features = reject != 0;
    show("fresh", &dev);
    CLAB_CHECK_EQ(clab_neg_reset(&dev), CLAB_NEG_OK);
    show("after reset", &dev);
    CLAB_CHECK_EQ(clab_neg_acknowledge(&dev), CLAB_NEG_OK);
    show("after ACK|DRIVER", &dev);

    const clab_neg_err rc = clab_neg_features(&dev, WANTED, &got);
    show("after features", &dev);
    printf("  negotiated 0x%016llx\n", (unsigned long long)got);

    if (rc == CLAB_NEG_OK) {
        CLAB_CHECK_EQ(clab_neg_ready(&dev), CLAB_NEG_OK);
        show("after DRIVER_OK", &dev);
        CLAB_CHECK_EQ(clab_neg_check_health(&dev), CLAB_NEG_OK);
    } else {
        /* The only correct reaction: FAILED, then reset, then report. */
        printf("  refused: %d, resetting\n", (int)rc);
        CLAB_CHECK(rc == CLAB_NEG_ERR_FEATURES);
        CLAB_CHECK_EQ(clab_neg_ready(&dev), CLAB_NEG_ERR_ORDER);
        CLAB_CHECK_EQ(clab_neg_reset(&dev), CLAB_NEG_OK);
        show("after reset", &dev);
    }
    printf("  status reads back as \"%s\"\n",
           clab_neg_status_str(dev.status, buf, sizeof buf));
    return 0;
}

int main(void)
{
    printf("chapter 4 - feature negotiation and the status sequence\n");

    run("1. device offers everything", WANTED, 0);
    /* Only the version and the mount tag: the driver asked for indirect and
     * event index too, and must not claim them. */
    run("2. device offers less than we asked", CLAB_F_VERSION_1 | CLAB_F_MOUNT_TAG, 0);
    run("3. device refuses our selection", WANTED, 1);

    printf("4. a device without VIRTIO_F_VERSION_1 is not a modern device\n");
    clab_neg_dev old;
    clab_neg_init(&old, CLAB_F_MOUNT_TAG); /* no VERSION_1 */
    CLAB_CHECK_EQ(clab_neg_acknowledge(&old), CLAB_NEG_OK);
    uint64_t got = 0;
    CLAB_CHECK_EQ(clab_neg_features(&old, WANTED, &got),
                  CLAB_NEG_ERR_UNSUPPORTED);

    printf("5. NEEDS_RESET after DRIVER_OK is a device error, not ours\n");
    clab_neg_dev sick;
    clab_neg_init(&sick, WANTED);
    CLAB_CHECK_EQ(clab_neg_acknowledge(&sick), CLAB_NEG_OK);
    CLAB_CHECK_EQ(clab_neg_features(&sick, WANTED, &got), CLAB_NEG_OK);
    CLAB_CHECK_EQ(clab_neg_ready(&sick), CLAB_NEG_OK);
    sick.status = (uint8_t)(sick.status | CLAB_STATUS_NEEDS_RESET);
    CLAB_CHECK_EQ(clab_neg_check_health(&sick), CLAB_NEG_ERR_NEEDS_RESET);

    return clab_result();
}

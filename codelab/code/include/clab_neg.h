/* SPDX-License-Identifier: Apache-2.0 */
/*
 * clab_neg.h - chapter 4's exercise: the status sequence and feature
 * negotiation, over a pretend device.
 *
 * virtio 1.2 section 3.1 defines an exact order of writes to the device status
 * byte, and the device is allowed to fail at two points: it can clear
 * FEATURES_OK if it cannot work with the features the driver picked, and it can
 * set NEEDS_RESET at any time afterwards. Drivers that get this wrong work on
 * QEMU and fail on something stricter, so the sequence is worth writing once
 * against a state machine you can test.
 *
 * This file is that state machine, with a "device" whose behaviour you choose:
 * which features it offers, and whether it accepts the driver's selection. No
 * QNX header, so it runs on the PC.
 */
#ifndef CLAB_NEG_H
#define CLAB_NEG_H

#include <stdbool.h>
#include <stdint.h>

/* Device status bits (virtio 1.2, 2.1). */
#define CLAB_STATUS_ACK        0x01u
#define CLAB_STATUS_DRIVER     0x02u
#define CLAB_STATUS_DRIVER_OK  0x04u
#define CLAB_STATUS_FEATURES_OK 0x08u
#define CLAB_STATUS_NEEDS_RESET 0x40u
#define CLAB_STATUS_FAILED     0x80u

/* Feature bits this exercise cares about. */
#define CLAB_F_MOUNT_TAG (1ull << 0)  /* device-specific, 9P */
#define CLAB_F_INDIRECT  (1ull << 28)
#define CLAB_F_EVENT_IDX (1ull << 29)
#define CLAB_F_VERSION_1 (1ull << 32)

/* The device as far as negotiation is concerned. */
typedef struct {
    uint64_t offered;      /* what the device advertises */
    uint64_t accepted;     /* what the driver wrote to driver_feature */
    uint8_t status;        /* the status byte */
    bool reject_features;  /* when true, the device clears FEATURES_OK */
    bool needs_reset;      /* when true, the device sets NEEDS_RESET */
    unsigned resets;       /* how many times the driver reset the device */
} clab_neg_dev;

/* Error codes, 0 is success. */
typedef enum {
    CLAB_NEG_OK = 0,
    CLAB_NEG_ERR_ORDER = -1,   /* a status bit written out of order */
    CLAB_NEG_ERR_FEATURES = -2, /* the device cleared FEATURES_OK */
    CLAB_NEG_ERR_NEEDS_RESET = -3,
    CLAB_NEG_ERR_UNSUPPORTED = -4 /* VIRTIO_F_VERSION_1 not offered */
} clab_neg_err;

/* A fresh device offering `offered`. */
void clab_neg_init(clab_neg_dev *dev, uint64_t offered);

/* Reset: status to 0 and back to 0, which the driver must verify (3.1.1). */
clab_neg_err clab_neg_reset(clab_neg_dev *dev);

/* ACK, then DRIVER. */
clab_neg_err clab_neg_acknowledge(clab_neg_dev *dev);

/* Offer `wanted`; keep only the bits the device actually offered, write them,
 * then set FEATURES_OK and read it back. A modern device that does not offer
 * VIRTIO_F_VERSION_1 is refused before anything is written. */
clab_neg_err clab_neg_features(clab_neg_dev *dev, uint64_t wanted,
                               uint64_t *out_negotiated);

/* DRIVER_OK, the last step before the first request. */
clab_neg_err clab_neg_ready(clab_neg_dev *dev);

/* What a driver must do when it finds the device in an unexpected state. */
clab_neg_err clab_neg_check_health(const clab_neg_dev *dev);

/* Name of a status byte, for the exercise's output ("ACK|DRIVER"). */
const char *clab_neg_status_str(uint8_t status, char *buf, unsigned cap);

#endif /* CLAB_NEG_H */

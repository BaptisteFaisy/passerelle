# Chapter 4 — Features and the status sequence

**Goal.** Reset the device, acknowledge it, negotiate the features both sides
can live with, and reach `DRIVER_OK` — in exactly the order the virtio
specification prescribes, and with a state machine you can test without a
device.

**Time.** 30 minutes.

## 4.1 The status byte

The device status is a single byte at offset `0x14` of the common
configuration structure. It is both a handshake and a health report:

| Bit | Name | Meaning |
|---|---|---|
| 0 | ACK | "I know this is a virtio device" |
| 1 | DRIVER | "I have read the specification" |
| 2 | DRIVER_OK | "the device is ready to be driven" |
| 3 | FEATURES_OK | "the feature set is agreed" |
| 6 | NEEDS_RESET | "the device is broken, reset me" |
| 7 | FAILED | "the driver gave up" |

The driver sets bits; the device may clear `FEATURES_OK` and may set
`NEEDS_RESET`. **The driver never clears a bit the device set**, except by
resetting the whole device.

## 4.2 The sequence

This is virtio 1.2, §3.1, and the order is not a suggestion:

```
 1. reset:  write 0 to the status byte, and read it back until it reads 0
 2. write ACK
 3. write DRIVER                       (the status is now ACK|DRIVER)
 4. read the device features
 5. write the driver's feature selection
 6. write FEATURES_OK, and read it back
 7. set up the queues
 8. write DRIVER_OK
```

Four of those steps have a trap attached.

**Step 1: the reset must be observed.** Writing 0 is not enough; the device
must *read back* 0 before the driver goes on, because a device that is still
processing a request may take a moment. QEMU's reset waits for the requests it
is still serving, so the read-back succeeds after a short delay. A driver that
does not wait for it will start writing into a device that is still using the
old state.

**Step 4 and 5: features are 64 bits, in two 32-bit halves.** The device
features are read by writing a selector (`device_feature_select`) and reading
`device_feature`; the driver writes its selection the same way with
`driver_feature_select` and `driver_feature`. The selector is 0 for the low
half and 1 for the high half, which is where bit 32 (`VIRTIO_F_VERSION_1`)
lives.

**Step 5: never offer a bit the device did not offer.** The negotiated set is
the *intersection*. A driver that writes a bit the device did not advertise is
undefined behaviour, and the symptom is a device that works until the day it
does not.

**Step 6: read `FEATURES_OK` back.** This is the device's veto. A device that
cannot live with the driver's selection clears the bit, and the driver must
stop: set `FAILED`, reset, and either give up or offer fewer features. A
driver that ignores the veto and goes on to `DRIVER_OK` is writing to a device
that has already decided it does not want to talk.

## 4.3 The features a 9P driver needs

| Bit | Name | Needed? |
|---|---|---|
| 0 | `VIRTIO_9P_F_MOUNT_TAG` | yes: the device tells the driver the name of the share |
| 28 | `VIRTIO_F_RING_INDIRECT_DESC` | useful: one descriptor instead of N for a scatter-gather list |
| 29 | `VIRTIO_F_RING_EVENT_IDX` | useful: the driver can be told which event to signal |
| 32 | `VIRTIO_F_VERSION_1` | **mandatory on the modern interface** |

`VIRTIO_F_VERSION_1` deserves a word. A device that offers it speaks virtio
1.0 or later. A device that does not offer it is a legacy device, which uses
a completely different register layout (I/O BARs, a different common
configuration structure). On the modern PCI interface, the specification says
the device must offer `VERSION_1`, so a driver that requires it is safe; the
cost is that legacy devices are out of reach, which is the right trade for a
new driver.

The device-specific feature `MOUNT_TAG` is what makes requirement VIO-05 of
this project's specification possible: without it, the driver cannot know which
share it is talking to.

## 4.4 The state machine, testable

The exercise turns the sequence into a small state machine with no QNX header,
so it runs on the PC and can be fed a device that misbehaves on purpose.

```c
/* clab_neg.h */
typedef struct {
    uint64_t offered;      /* what the device advertises */
    uint64_t accepted;     /* what the driver wrote to driver_feature */
    uint8_t status;        /* the status byte */
    bool reject_features;  /* when true, the device clears FEATURES_OK */
    bool needs_reset;      /* when true, the device sets NEEDS_RESET */
    unsigned resets;       /* how many times the driver reset the device */
} clab_neg_dev;

typedef enum {
    CLAB_NEG_OK = 0,
    CLAB_NEG_ERR_ORDER = -1,    /* a status bit written out of order */
    CLAB_NEG_ERR_FEATURES = -2, /* the device cleared FEATURES_OK */
    CLAB_NEG_ERR_NEEDS_RESET = -3,
    CLAB_NEG_ERR_UNSUPPORTED = -4 /* VIRTIO_F_VERSION_1 not offered */
} clab_neg_err;
```

The two functions that matter:

```c
clab_neg_err clab_neg_features(clab_neg_dev *dev, uint64_t wanted,
                               uint64_t *out_negotiated)
{
    if ((dev->status & (CLAB_STATUS_ACK | CLAB_STATUS_DRIVER)) !=
        (CLAB_STATUS_ACK | CLAB_STATUS_DRIVER))
        return CLAB_NEG_ERR_ORDER;
    /* A modern device must offer VIRTIO_F_VERSION_1. */
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
```

and the last step:

```c
clab_neg_err clab_neg_ready(clab_neg_dev *dev)
{
    if ((dev->status & CLAB_STATUS_FEATURES_OK) == 0u)
        return CLAB_NEG_ERR_ORDER;   /* DRIVER_OK without FEATURES_OK is a bug */
    dev->status = (uint8_t)(dev->status | CLAB_STATUS_DRIVER_OK);
    return CLAB_NEG_OK;
}
```

Note what `clab_neg_ready()` refuses: `DRIVER_OK` when `FEATURES_OK` is not
set. That check is what stops a driver from talking to a device that has
already refused the feature set.

## 4.5 The real thing

In the driver, the same four steps, with the COMMON structure's fields:

```c
/* 1. reset, and wait for the device to read back 0 */
set8(v, VPCI_COMMON_DEVICE_STATUS, 0);
for (int i = 0; cfg8(v, VPCI_COMMON_DEVICE_STATUS) != 0; i++) {
    if (i == 1000)
        return -EIO;          /* a second is plenty */
    usleep(1000);
}

/* 2 and 3. ACK, then DRIVER */
set8(v, VPCI_COMMON_DEVICE_STATUS, VSTAT_ACK);
set8(v, VPCI_COMMON_DEVICE_STATUS, VSTAT_ACK | VSTAT_DRIVER);

/* 4. read the device features, both halves */
set32(v, VPCI_COMMON_DEVICE_FEATURE_SELECT, 0);
uint64_t dev = cfg32(v, VPCI_COMMON_DEVICE_FEATURE);
set32(v, VPCI_COMMON_DEVICE_FEATURE_SELECT, 1);
dev |= (uint64_t)cfg32(v, VPCI_COMMON_DEVICE_FEATURE) << 32u;

/* 5. write the driver's selection: the intersection, and no more */
uint64_t want = VPCI_9P_F_MOUNT_TAG | VPCI_F_VERSION_1;
uint64_t drv = dev & want;
set32(v, VPCI_COMMON_DRIVER_FEATURE_SELECT, 0);
set32(v, VPCI_COMMON_DRIVER_FEATURE, (uint32_t)drv);
set32(v, VPCI_COMMON_DRIVER_FEATURE_SELECT, 1);
set32(v, VPCI_COMMON_DRIVER_FEATURE, (uint32_t)(drv >> 32u));

/* 6. FEATURES_OK, and read it back */
set8(v, VPCI_COMMON_DEVICE_STATUS, VSTAT_ACK | VSTAT_DRIVER | VSTAT_FEATURES_OK);
if ((cfg8(v, VPCI_COMMON_DEVICE_STATUS) & VSTAT_FEATURES_OK) == 0u)
    return -EIO;

/* ... queues, chapter 5 ... */

/* 8. DRIVER_OK */
set8(v, VPCI_COMMON_DEVICE_STATUS,
     (uint8_t)(cfg8(v, VPCI_COMMON_DEVICE_STATUS) | VSTAT_DRIVER_OK));
```

Two details in that listing are easy to get wrong:

- **The feature bits are written as two separate 32-bit halves.** Writing the
  high half requires the selector to be 1 at that moment; forgetting to switch
  the selector back to 0 writes the high half twice and the low half never.
- **The status is written as a whole byte, not as "or this bit".** The device
  status is not a set of independent flags: writing `ACK` alone would clear
  `DRIVER`. Always read the current status, add your bit, and write the whole
  byte back.

## 4.6 What to observe

Running the exercise:

```sh
/tmp/codelab-build/ex03_negotiate
```

```
1. device offers everything
  fresh                status 0x00  0
  after reset          status 0x00  0
  after ACK|DRIVER     status 0x03  ACK|DRIVER
  after features       status 0x0b  ACK|DRIVER|FEATURES_OK
  after DRIVER_OK      status 0x0f  ACK|DRIVER|FEATURES_OK|DRIVER_OK
  negotiated 0x0000000100000009

2. device offers less than we asked
  ...
  negotiated 0x0000000100000001     <- only what was offered
```

The three runs are: a device that offers everything, one that offers less than
the driver asked for, and one that refuses the driver's selection.

## 4.7 Traps

- **Writing the status byte instead of OR-ing into it.** `set8(v, STATUS,
  VSTAT_ACK)` after `DRIVER` has been set clears `DRIVER`. Read, OR, write.
- **Not checking `FEATURES_OK` after writing it.** The device may veto.
- **Offering features the device did not offer.** The intersection, always.
- **Forgetting `VIRTIO_F_VERSION_1`.** On the modern interface it is
  mandatory; a driver that negotiates without it may end up speaking the
  legacy layout to a modern device.
- **Skipping the reset's read-back.** The device may still be busy.
- **Not handling `NEEDS_RESET`.** A device that sets it after `DRIVER_OK` is
  telling the driver to reset and start over. The transport must check the
  status after every completion, not only at startup.

## Exercise

1. Run `ex03_negotiate` and read the four status lines of each run. Explain
   why the status after `fresh` and after `after reset` are both `0x00`, and
   why that is not a bug.
2. Change the device's offered set in run 2 to include `INDIRECT` but not
   `EVENT_IDX`, and check that the negotiated set matches. The rule is the
   intersection, always.
3. In `clab_neg_ready()`, remove the `FEATURES_OK` check and run the tests.
   The test `test_ch4_failures` must fail. That check is the only thing
   standing between a driver and a device that has already refused it.
4. Add a case where the device sets `NEEDS_RESET` after `DRIVER_OK`, and
   check `clab_neg_check_health()` reports it.

## Solution

Exercise 1: the status is 0 after a reset because a reset *is* "the status is
0"; the two lines show the state before and after the reset, and they are the
same because the driver had not set anything yet.

Exercise 2: the negotiated set becomes `VERSION_1 | MOUNT_TAG | INDIRECT`,
and the driver must not claim `EVENT_IDX`.

Exercise 3: without the check, `clab_neg_ready()` returns `CLAB_OK` after the
device cleared `FEATURES_OK`, and `test_ch4_failures` fails on
`CLAB_CHECK_EQ(clab_neg_ready(&dev), CLAB_NEG_ERR_ORDER)`.

Exercise 4:

```c
sick.status = (uint8_t)(sick.status | CLAB_STATUS_NEEDS_RESET);
CLAB_CHECK_EQ(clab_neg_check_health(&sick), CLAB_NEG_ERR_NEEDS_RESET);
```

## Where we are

The device is initialised and the driver is allowed to talk to it. Chapter 5
builds the thing they talk through: the split virtqueue.
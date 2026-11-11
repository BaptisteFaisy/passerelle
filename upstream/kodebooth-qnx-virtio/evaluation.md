# kodebooth/qnx-virtio — reuse evaluation (decision D3)

Evaluated on 11 November 2026 against the reference tree in
`passerelle-refs/qnx-virtio`. MIT licence, copyright Abe Kohandel. Read, not
built: it needs the QNX SDP, which the development machine does not have.

**Decision D3 in the specification** is: reuse this transport library, or write
our own. The rule it sets is *"evaluate it during the first week; write our own
if reusing it costs more than a day."* This document is that evaluation, and the
conclusion below is what `docs/decisions.md` records.

## What the library is

A virtio implementation for QNX 8, in C11, with a sample driver:

```
src/virtio/        core: device abstraction, status, features, queue creation
  virtio.h         414 lines, the public API
  virtio.c         401 lines
  virtq.c          592 lines, split virtqueue, legacy and modern layouts
src/virtio_pci/    PCI transport, 734 lines + header
src/virtio_mmio/   MMIO transport, 643 lines + header
src/gpio/          a complete virtio-gpio resource manager, as a worked example
utils/, docker/, images/, tests/gpio/   build images and pytest-based tests
```

About 2 400 lines of transport plus a working driver on top of it. CMake with
presets for `x86_64` and `aarch64le`, a Docker image, and a pytest suite that
drives a QEMU guest over a serial line.

**This is a real library, not a sketch.** The points that matter for us:

- **The device abstraction is transport-agnostic and vtable-based**
  (`struct virtio_ops` with `read_device_features`, `set_device_status`,
  `set_queue_addr`, `notify_queue`, `enable_queue`, `max_queue_size`,
  `set_queue_size`, `virtq_callback`, and more). PCI and MMIO both implement it.
  A third transport would fit without touching the core.
- **Both virtio layouts are handled**: legacy (single contiguous block, page
  aligned, `QueuePfn`) and modern (separate descriptor, available and used
  allocations with 64-bit addresses), for both buses.
- **The virtqueue has a real descriptor allocator** (`fifo`-based free list),
  per-descriptor context pointers, and a chain API
  (`virtq_alloc_desc`/`virtq_put_desc_chain`/`virtq_get_desc_chain`) rather than
  a hard-coded two-descriptor pattern.
- **The interrupt path is right for QNX 8.0**: a dedicated thread calling
  `InterruptAttachThread()` then looping on `InterruptWait()`, which is the API
  that replaced `InterruptAttach()`. It uses `InterruptUnmask()` before waiting,
  which is what a level-triggered line needs.
- **The GPIO driver on top is complete** — resource manager, `/dev/gpioN.M`,
  `devctl`, a semaphore-based synchronous wrapper around an asynchronous
  virtqueue — and therefore doubles as documentation of how the library is meant
  to be used.
- **Device reset is handled properly**: `virtio_reset_device()` polls the status
  register with a timeout and a poll interval, and returns `ETIMEDOUT` rather
  than spinning forever. The queue-reset feature (virtio 1.2, `RING_RESET`) is
  wired through `set_queue_reset`/`get_queue_reset` even though the GPIO driver
  does not use it.
- **PCI capabilities are read through libpci's own helpers**
  (`pci_device_find_capid`, `cap_vend_read_bytes`), not by poking config space
  by hand — cleaner than what we ended up doing, and it is the approach to try
  first on any machine where the `cap_vend` module loads.

## What it does not have, for our need

Four gaps, in the order they hurt.

### 1. No bus-access abstraction, so nothing can be tested on the PC

This is the decisive one. Every layer of Passerelle must build and run its tests
on a Linux PC: that is requirement P9-10 for the protocol library, and it is
the property that makes the conformance bench meaningful, because the same
assertions run on the host and in the guest.

This library cannot do that. `virtq_create()` calls `mmap64(MAP_PHYS|MAP_ANON)`
and `mem_offset64()` directly; the transports call `mmap_device_memory()`,
`pci_device_*()` and `InterruptAttachThread()` directly. There is no seam where
a test could substitute RAM for device memory, or a function table for a bus.
Building it on the PC is not a matter of a flag: the QNX headers and the QNX
memory model are baked into the implementation files, not just the transports.

Passerelle's own transport puts a `pgw_vio_bus_ops` function table between the
core and the bus (see `virtio/include/pgw/virtio.h`), and the MMIO register
access is itself a callback (`pgw_mmio_io`), which is what lets a full split
virtqueue, feature negotiation and completion be exercised in a unit test with
no hardware at all — and, on the PCI side, lets the capability decoder be tested
against a synthetic config space.

Adding that seam to this library is not a small patch: it means changing the
signature of every entry point in `virtio.h`, or introducing a parallel
implementation. That is more than the one day D3 allows.

### 2. No indirect descriptors, no notification suppression

`virtq.c` sets `VIRTQ_DESC_F_NEXT` and `VIRTQ_DESC_F_WRITE` and nothing else.
There is no `VIRTQ_DESC_F_INDIRECT`, so a request with more scatter-gather
elements than the ring has descriptors cannot be expressed. Nor is there any
handling of `VIRTQ_AVAIL_F_NO_INTERRUPT` / `used_event` / `avail_event`: the
available ring is allocated with the `used_event` word and the used ring with
the `avail_event` word, but no code ever reads or writes them, and
`virtq_put_desc_chain()` bumps `avail->idx` and then the caller notifies
unconditionally.

Passerelle needs both. VIO-08 in the specification makes indirect descriptors
and unnecessary-notification suppression a *complete*-tier requirement with a
measurement attached, and a 9P client with a 128 KiB message size builds
scatter-gather lists of a dozen elements per request routinely. Adding indirect
support is a contained change (one descriptor type, one flag, one
`vring_desc`-aligned table allocation), but the notification suppression has to
be threaded through the notify path, the interrupt handler and the queue state,
which is exactly where the library's lack of a test seam makes a change risky.

### 3. No sharing tag concept, no device selection by tag

The library finds a device by PCI vendor/device id and index, or by an MMIO
physical address given on the command line. It never reads the device-specific
configuration space.

For a filesystem this is not a detail: VIO-05 requires selecting the device by
its mount tag, because a guest can have several shares mounted at once
(VIO-06), and the tag is the only thing that distinguishes them. The GPIO
driver has the same shape — device id 41, index 0 — and does not need it. So
there is no code to reuse here; it would be written from scratch, and it would
have to be written *inside* the library, because that is where the config-space
mapping lives.

### 4. Two defects worth knowing about before relying on it

These are not reasons to reject the library, but they are reasons to read it
rather than trust it:

- **`virtio_pci_read_device_config()` indexes a `uint32_t *` with a byte
  offset**: `((uint32_t *)dst)[i] = ((uint32_t *)pdev->device_cfg)[offset + i]`
  inside a loop that advances `i` by 4. With `offset = 0` it writes four times
  the requested length; with any other offset it reads the wrong words. It is
  unused by the GPIO driver (which never reads device config), so it has
  probably never been called.
- **`virtio_pci_notify_queue()` writes a byte, not a 16-bit word**:
  `((uint8_t *)pdev->notify_cfg)[off * multiplier] = index`. virtio 1.2 §4.1.4.4
  specifies a 16-bit write of the queue index. It happens to work on QEMU when
  the index is 0 and the value fits in a byte, which is the only case the GPIO
  driver exercises.

Neither would block us — they are two lines each — but they are evidence that
the PCI path has been exercised with one queue, one device and one driver, and
that the parts we would depend on are the parts that have been least used.

## Weighing it

| Criterion | This library | What Passerelle needs | Verdict |
|---|---|---|---|
| Testable on the PC | no seam at all | every layer, requirement P9-10 | blocking |
| Indirect descriptors | absent | VIO-08, complete tier, measured | must add |
| Notification suppression | absent (rings sized for it, unused) | VIO-08, measured | must add |
| Device tag / device config | absent, and the config accessor is broken | VIO-05, VIO-06 | must write |
| MSI-X | libpci helpers, one vector | VIO-04, MSI-X with INTx fallback | partial; the helper path is the one to try first |
| MMIO v1/v2 | yes, both, complete | VIO-03 | reusable as a reference |
| Split virtqueue | yes, with an allocator and context pointers | VIO-07, VIO-08 | good design to follow |
| Interrupt thread (QNX 8.0 API) | yes, `InterruptAttachThread` + `InterruptWait` + `InterruptUnmask` | VIO-04, VIO-09 | reusable as a reference |
| Device reset with timeout | yes, polled | VIO-09 | reusable as a reference |
| Licence | MIT, compatible | Apache-2.0 project | fine, with attribution |

## Decision

**Write our own transport; do not reuse this one.** Not because it is poor — it
is the best-organised QNX virtio code in the open, and reading it improved ours
— but because the one property Passerelle cannot give up is that the transport
core is exercisable on a PC without an SDP, and this library has no seam for
that. Adding the seam, the indirect descriptors, the notification suppression
and the tag plumbing is more than the one day D3 budgets, and it would be a
rewrite of the parts we would then rely on anyway.

What we did take from it, and say so in `NOTICE`:

- the shape of a transport-agnostic `virtio_ops` vtable, which our
  `pgw_vio_bus_ops` follows;
- the confirmation that `InterruptAttachThread()` + `InterruptWait()` with an
  `InterruptUnmask()` before each wait is the pattern that works on QNX 8.0;
- the reminder that device reset must be polled with a timeout rather than
  assumed immediate;
- the two PCI defects above, which told us to validate the device-configuration
  read and to write the notify word as 16 bits.

No line of this project is copied into Passerelle. The concepts are the
virtio specification's; the code around them is written independently.

## For `docs/decisions.md`

> **D3 — Reuse `kodebooth/qnx-virtio` or write our own transport: write our
> own.** The library (MIT, ~2 400 lines of transport plus a virtio-gpio driver)
> is well structured and handles both buses, both MMIO layouts, legacy and
> modern rings, and the QNX 8.0 interrupt-thread API correctly. It fails on one
> requirement we cannot relax: every layer must build and run its tests on the
> PC, and its virtqueue and transports call QNX memory and PCI APIs directly,
> with no seam for a test double. It also lacks indirect descriptors and
> notification suppression (both required by VIO-08 with a measurement), has no
> mount-tag handling (VIO-05), and its PCI device-config accessor is incorrect.
> Reusing it would therefore mean adding a bus abstraction across its whole
> public API, plus the missing features, in more than the one day the
> specification budgets. Our transport keeps a function-table seam
> (`pgw_vio_bus_ops`) and a register-access callback (`pgw_mmio_io`), which is
> what makes a full split virtqueue, feature negotiation and completion testable
> on the PC. Its interrupt pattern, its vtable shape and its polled reset were
> taken from this library as design references, with attribution in `NOTICE`.

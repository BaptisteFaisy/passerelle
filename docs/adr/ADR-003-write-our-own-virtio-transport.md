<!--
SPDX-License-Identifier: Apache-2.0
-->

# ADR-003 — write our own layer 1 instead of vendoring `kodebooth/qnx-virtio`

- **Status**: Accepted (11 November 2026) — this is the code-level outcome of
  decision **D3** of the specification
- **Requirements**: VIO-01 (transitional *and* modern PCI ids), VIO-07 (zero-copy
  scatter-gather), VIO-08 (indirect descriptors, `EVENT_IDX`), VIO-10 (a
  standalone, documented transport), P9-10 (the layers build and are tested on
  the host PC with no QNX toolchain)
- **Evidence**: [`upstream/kodebooth-qnx-virtio/evaluation.md`](../../upstream/kodebooth-qnx-virtio/evaluation.md),
  [`docs/decisions.md`](../decisions.md) (D3), [`docs/state-of-the-art.md`](../state-of-the-art.md)
- **Code**: [`virtio/`](../../virtio) — `pgw/virtio.h`, a bus-ops table, PCI
  modern + transitional, MMIO v1/v2, and a mock backend for PC tests

## Context

`kodebooth/qnx-virtio` (April 2026, MIT, ~2 400 lines of transport plus a
complete virtio-gpio resource manager) is a real library, not a sketch: the
device abstraction is transport-agnostic and vtable-based, both virtqueue
layouts are handled, and its interrupt path is the right one for QNX 8.0
(`InterruptAttachThread()` + `InterruptWait()`).

The specification's rule for D3 was: *evaluate it during week 1; write our own
if adapting costs more than a day.* The evaluation was done by reading the tree
line by line (it cannot be built here: it needs the SDP).

## Decision

**Do not vendor it. Write our own portable layer 1.** Five findings drove the
call, in order of weight:

1. **It cannot be compiled or unit-tested off QNX.** `virtio.c` includes
   `<sys/neutrino.h>` and calls `InterruptAttachThread()`; `virtio_pci.c` calls
   `libpci` directly. That contradicts P9-10 and the design rule that layers 1
   and 2 are tested on the host PC, in CI, with no SDP.
2. **Modern-only PCI ids.** `VIRTIO_PCI_DID(type)` is `0x1040 + type` and the
   device is forced to `legacy = false`, so the transitional id `0x1009` that
   VIO-01 requires — and that stock QEMU configurations expose — is not
   recognised. (Our own PCI test sees `1af4/1009` reported by a real QNX 7.1
   guest.)
3. **No indirect descriptors, no `EVENT_IDX`.** Chains are allocated descriptor
   by descriptor; the rings reserve the event-index bytes without using them.
   VIO-08 would have to be written from scratch anyway.
4. **Concurrency and reset shape.** One coarse `pthread_spinlock_t` for the
   transport, and a reset that polls in a `delay()` loop, which blocks the
   caller instead of fitting an asynchronous event loop.
5. **Adaptation cost.** Splitting it into an OS-independent core plus OS bus
   accessors means rewriting roughly 80 % of `src/virtio/` and
   `src/virtio_pci/`, then maintaining a fork whose API has diverged.

The project is cited with gratitude in [`NOTICE`](../../NOTICE) — its use of the
QNX 8.0 `libpci` MSI-X calls is what confirmed our own interrupt design.

## Consequences

- Layer 1 is OS-independent behind `pgw_vio_bus_ops`, so `virtio/src/pci_qnx.c`
  and `mmio_qnx.c` are the only QNX files; everything else runs on the PC, which
  is what makes the whole test strategy of this repository possible
  (`test_virtio_pci`, `test_virtio_mmio`, `test_virtio_vq`, `test_virtio_dev`,
  `test_virtio_mock`).
- Both PCI id families are supported, indirect descriptors and notification
  suppression are negotiated, and the transport is zero-copy by construction
  (descriptors carry the caller's addresses).
- No MIT code is vendored, so there is no second licence in the tree and no
  fork to track upstream.
- The cost is ours: ~2 000 lines of transport to maintain, and a QNX-side bus
  backend that is only compile-checked (`qnx-syntax`) until an SDP is available.

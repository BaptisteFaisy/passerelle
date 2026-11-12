<!--
SPDX-License-Identifier: Apache-2.0
-->

# ADR-006 — MSI-X is the interrupt path; INTx is the fallback, and the driver never depends on the fragment

- **Status**: Accepted (12 November 2026) — the measurements are from the QNX 7.1
  guest booted in CI, not from a guess
- **Scope**: the interrupt path of the QNX driver (VIO-04), the role of
  [`image/pci_hw.cfg`](../../image/pci_hw.cfg), and the order in which the two
  mechanisms are tried
- **Requirements**: VIO-04 (the driver obtains the PCI interrupt)
- **Evidence**: [`docs/qnx71-guest.md`](../qnx71-guest.md) §5 (the measurements and
  how to reproduce them), [`tests/qemu/qnx71/EVIDENCE.md`](../../tests/qemu/qnx71/EVIDENCE.md),
  the CI artefact `qnx71-guest-probe` of
  [`.github/workflows/qnx71-guest.yml`](../../.github/workflows/qnx71-guest.yml)
- **Code**: [`virtio/src/pci_modern.c`](../../virtio/src/pci_modern.c) (MSI-X table
  programming, INTx fallback through the ISR capability),
  [`virtio/src/pci_qnx.c`](../../virtio/src/pci_qnx.c) (the QNX binding),
  [`drivers/common/src/ist_qnx.c`](../../drivers/common/src/ist_qnx.c) (the interrupt
  service thread, used by both paths)
- **Proof**: `test_msix_table_is_programmed`, `test_msix_can_be_declined`,
  `test_intx_path_uses_the_isr_capability` (PC test suite of `virtio/`)

## Context

QNX 8.0 has no `InterruptAttach()`: a handler can no longer run in interrupt
context, and the only supported shape is an interrupt service thread attached
with `InterruptAttachThread()`. A PCI device can still deliver its interrupt
two ways, and the project has to choose an order:

- **MSI-X** — the capability `0x11`, a table of vectors the device writes to
  instead of raising an INTx line;
- **INTx** — the classic interrupt pin, which the firmware and the machine
  wire to a legacy interrupt request line (GSI) that the operating system
  hands to a driver.

Until now the choice was *measured but not decided* (gap 77): the code tried
MSI-X and fell back to INTx, and the image carried an interrupt fragment for
the INTx line, without a record saying which one is the intended path. Here is
what is actually measured.

**Measured, guest side (QNX Neutrino 7.1.0, the CI guest):**

1. QEMU assigns our device `B0:D5:F0`, `IRQ 10`, `pin A` — read back from the
   host side of the same bus with `info pci` at the QEMU monitor, and captured
   in the CI artefact `host-qemu-info-pci.txt`.
2. The guest's own `/proc/boot/pci_hw.cfg` routes **no** interrupt for
   `B0:D05:F00`: `pci-tool` reports `Interrupt list - 0 assigned` for our
   device, while the image routes `B0:D17:F0` and `B3:D0:F0` on IRQs 16 to 19.
3. The hardware module of that image is `pci_hw-Intel_x86.so` 2.4 (2020-06-14),
   and two of its own strings, read out of the binary, bound what the fragment
   can do there: `HW config file IRQ %d for B%u:D%u:F%u is invalid. Must >= 16`
   and `No chipset support found for 0x%x`. Every record the image ships uses
   16 to 19.
4. QEMU only ever routes PCI INTx to a **legacy GSI 0–15**: `IRQ 10` for our
   device on the `qshare` options, `IRQ 11` on both a bare `pc` and a bare
   `q35` machine. No QEMU machine type whose INTx line reaches 16 exists.
5. The same guest proves QNX does allocate MSI-X vectors: the `vmxnet3` device
   next to ours reports `Interrupt list - 3 assigned` on `IRQ 256`, `257`,
   `258`, and the image ships the MSI-X capability module `pci_cap-0x11.so`.
6. Our device advertises MSI-X as its **first** capability (the capability
   list of the artefact: MSI-X `0x11`, then the five virtio vendor
   capabilities `0x09`), followed by the three assigned address spaces.

**Measured, host side:** the QEMU 7.1 guest reports the QNX numbering *is*
the GSI: in the same guest the two USB controllers report `Interrupt 0 on
IRQ 11`, exactly what QEMU wired. So "must be >= 16" is a policy of that
Intel hardware module — it targets ITSS/PIRQ chipsets (Skylake, Denverton,
Apollo Lake, Coffeelake) and knows nothing of the PIIX3 QEMU emulates — not a
QNX-wide rule.

**To be measured under the SDP** (nothing below is claimed today):

- whether the QNX 8.0 `pci-server` allocates MSI-X vectors for *our* device,
  and with which interrupt numbers;
- whether an INTx interrupt is delivered at all on a QNX 8.0 image whose
  hardware module accepts a GSI below 16;
- whether the `pci_hw.cfg` fragment is honoured by the QNX 8.0 hardware
  module, and with which module version;
- the latency and the throughput difference between the two paths.

## Decision

**MSI-X is the primary interrupt path. INTx is the fallback. The driver never
depends on `image/pci_hw.cfg` being honoured.**

Concretely:

1. The driver asks for MSI-X first and programs the table and the capability
   itself (`pci_msix_program` in `virtio/src/pci_modern.c`). If the
   capability refuses to enable, the driver logs `MSI-X unavailable, using
   INTx` and attaches the classic line with `InterruptAttachThread()` —
   the same interrupt service thread on both paths.
2. `image/pci_hw.cfg` stays in the tree, but its role is redefined: it
   **records** the INTx wiring of our device (`B0:D5:F0 A 10`) for the images
   whose hardware module can declare it, and it is what the CI job
   cross-checks against what QEMU really assigns. It is *not* the mechanism
   that delivers interrupts on the QNX 7.1 image we can boot, because
   measurements 3 and 4 above show that module refuses that IRQ.
3. No code path is allowed to depend on the fragment: the driver must work on
   an image where the fragment is absent, ignored or refused. The fragment is
   an aid for a configuration we cannot boot yet, not a dependency.

## Consequences

- The order of the two mechanisms is now written down, so a reader of
  `virtio/src/pci_qnx.c` does not have to guess it, and so the fallback
  cannot be quietly removed.
- The risk of the decision is on the fallback side, and it is a real one: on a
  QNX 8.0 image where MSI-X cannot be allocated *and* the hardware module
  refuses a GSI below 16, our device would have no interrupt at all. That
  case is unmeasurable without the SDP, so it is accepted, written down, and
  logged loudly (`MSI-X unavailable, using INTx`) rather than hidden.
- The MSI-X message address and vector are **not** final: `pci_qnx.c` programs
  `0xFEE00000` with vector `0` as a placeholder, with the intent to take the
  vector from the PCI server once the QNX 8.0 API is available. This is the
  part that the SDP has to settle, and it is listed above as "to be measured".
- `image/pci_hw.cfg` keeps its cross-check in CI: the job prints the measured
  assignment and the fragment next to each other and says `DRIFT:` when they
  disagree, so a change of the QEMU command line cannot silently invalidate
  the record.
- What this ADR does **not** claim: that an interrupt is delivered to our
  device on any image. No interrupt delivery has been measured: the guest
  boots, enumerates the device, and stops there — there is no client in the
  image to mount the share with, and no driver of ours runs in it.

## Revisit when

- the SDP is available and the QNX 8.0 `pci-server` has been seen to allocate
  MSI-X vectors for our device (that measurement decides the primary path
  definitively);
- a QNX 8.0 image is booted whose hardware module accepts the fragment, which
  would make the INTx path measurable and comparable;
- QEMU ever exposes an INTx line at 16 or above, which would change what
  `pci_hw.cfg` can declare;
- QNX 8.0 drops or changes the MSI-X capability module (`pci_cap-0x11.so`),
  which would move the fallback from a safety net to the normal path.

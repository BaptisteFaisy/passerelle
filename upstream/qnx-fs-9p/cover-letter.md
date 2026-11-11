# Cover letter — virtio-pci transport and the x86_64 target for fs-9p

To the author of `gvergine/qnx-fs-9p`,

First: thank you for publishing fs-9p. It is the most complete 9P2000.L client
for QNX that exists in the open, and it is the reference this work was measured
against. The resource manager, the symlink redirect behaviour, the tag claim
with `name_attach()`, the slot interface in `transport.h`, the quarantine of fids
after a timeout, the `lib9p` codec with its sticky-failure cursors — all of that
is yours, and none of it is touched here. What follows is one addition and one
build change.

## What your project does, as we understand it

fs-9p mounts a host directory exported by QEMU's virtio-9p device as a QNX
resource manager: `open`, `read`, `write`, `stat`, `readdir`, `rename`,
symlinks, hard links, `fsync`, `df`, execution straight from the share, POSIX
permissions enforced in the guest, an optional metadata cache, no data cache,
clean unmount on a signal. It runs unprivileged with three abilities. The
transport is virtio-mmio on QEMU's aarch64 `virt` machine, and
`docs/dev/virtio-notes.md` explains why: on `virt` there is no aarch64 PCI
hardware module to install, QNX's own drivers use virtio-mmio there, and
virtio-pci would drag in a PCI server, the `PCI_HW_MODULE` variable and, on
machines without ECAM, I/O privilege.

That reasoning is sound, and the notes end with the sentence this patch series
acts on: *"A virtio-pci transport could be added later behind `transport.h`,
reusing the virtio core."*

## What we add

Four patches, in `patches/`, that do exactly that and nothing else:

1. **`src/virtio_pci_caps.{c,h}`** — decoding of the virtio PCI capabilities
   (common configuration, notification area with its multiplier, ISR byte,
   device configuration). It takes a config-space read callback rather than
   calling libpci, so it has no QNX header at all and is unit-tested on a Linux
   host against a synthetic config space, in the same spirit as `lib9p`. Two
   scans are offered: the capability chain from offset `0x34`, and a
   32-bit-only sweep of `0x40..0xFF` for hosts where `pci_device_cfg_rd8()`
   returns 0 at unaligned offsets.

2. **`src/transport_virtio_pci.c`** and **`src/virtio_pci_ring.c`** — a second
   backend for `transport.h`, with the same `fs9p_transport_ops`. Discovery
   through libpci, BAR mapping, the same status sequence and feature
   negotiation, the same split ring, the same request slots, the same
   interrupt-service-thread pattern, the same stale-slot handling on timeout.
   Only four things differ from your MMIO backend: how the device is found, how
   its registers are reached, how the queue is notified, and how the interrupt
   is obtained (MSI-X, with the firmware's INTx line as the fallback).

3. **`cmake/qnx8-x86_64.cmake`** and the matching change in `CMakeLists.txt` —
   the x86_64 target. The configure step no longer refuses every processor but
   `aarch64`; it picks the target from `CMAKE_SYSTEM_PROCESSOR` and still fails
   hard on anything else. The aarch64 build, its flags and its artifacts are
   unchanged.

4. **`-o transport=mmio|pci`**, defaulting to the architecture it was built for,
   plus the documentation: `docs/dev/virtio-pci-notes.md` for the developer
   side, the x86_64 QEMU configuration in `docs/user/qemu.md`, and the usual
   entries in the use message, the reference page and `CHANGELOG.md` (the
   `consistency` test passes).

## Why it is in the project's interest

- **It removes the one limitation your README and your architecture page both
  state plainly:** x86_64 is "not supported and never tested". x86_64 is what
  the stock `mkqnximage` images are, and what most people run QEMU on. A user
  who builds fs-9p for an x86_64 image today gets `no virtio-9p device with this
  mount tag`, because `-virtfs` creates a `virtio-9p-pci` device their driver
  cannot see. With these patches the same command line works.
- **It is small in the parts that matter.** The 9P client, the resource
  manager, the cache, the permission checks and `lib9p` are untouched, so the
  behaviour you verified in a guest cannot regress. The new code is behind the
  interface you built for exactly this purpose.
- **It brings a second bus to the same tests.** The decoder's unit tests run on
  the host in CI, which is the same property that makes `lib9p` testable
  without an SDP; the transport itself uses the same slot interface, so the
  conformance and differential bench of the project this comes from can drive
  either backend unchanged.

## What we are not claiming

**It has not been run on an x86_64 QNX guest.** We have no SDP on the machine
this was written on. What is proven: the series applies to your tree at
`868a718` (`git apply --check` and `git am`), the host build compiles with your
`-Wall -Wextra -Wpedantic -Werror` set, all six host tests pass (the five
existing ones plus the decoder's), and the QNX-only files pass a syntax check
against declarations-only stand-ins for the QNX headers. `validation.md` in this
directory lists exactly what is proven and what a first guest run should look
at, including the two numbers in the MSI-X path that we took from a kernel boot
log rather than from a header (the MSI IRQ base `0x100` and its first APIC
vector, 78) and that are the most likely thing to need adjusting on your
machine.

If MSI-X turns out to be fragile on a given image, the INTx path is what runs,
and it is the path your MMIO driver already exercises; making `msix_setup()`
return `-1` is enough to force it.

## Licensing and attribution

Both projects are Apache-2.0, so this is a straight contribution under the same
terms; the patches carry your licence header convention and no new dependency.
No file of yours is copied into the project these patches come from, and no
file of ours is copied into yours: the series only adds files and edits the four
you would expect (`CMakeLists.txt`, `src/transport.h`, `src/main.c`, and the
documentation). `NOTICE` in our repository records fs-9p as prior art.

## What we would like

Review, and if it holds up, a merge. We are happy to split the series further
(the decoder alone is a self-contained, test-covered addition that could land
first), to rename anything to match your conventions, or to drop the `-o
transport=` option in favour of an autodetect if you prefer the CLI to stay as
it is. We will also run whatever you want in a guest as soon as we have an SDP,
and report the results back rather than leaving the claim unverified.

Thank you for reading.

— The Passerelle contributors

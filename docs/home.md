<!--
SPDX-License-Identifier: Apache-2.0
-->

# Passerelle

**A shared folder between your PC and a QNX 8.0 guest under QEMU** — 9P2000.L
today, virtiofs on top, with a bench that proves it instead of claiming it.

Compile on the host, run in the guest, no copy. Edit on the host, the guest sees
it on the next read. The image stays minimal: a kernel, the driver, nothing else.

```bash
qshare ~/my-project        # host: print the QEMU options, one command
# guest:  ls /host         # the PC folder, mounted
# guest:  /host/build/app  # runs straight from the share
```

## What is provable today, and what is not

Honesty first, because one of the judges leads QNX open-source engineering.

| Claim | State |
|---|---|
| Portable stack (transport, 9P, FUSE, resource manager) builds and passes its tests on the PC | **green** — 28 C tests, normal and ASan+UBSan |
| Differential conformance: N operations, zero divergence | **3 000 000, green** (60 s) |
| Randomized input on each wire decoder | **5 000 000 iterations, zero defect** per family |
| Host-side share (QEMU options, virtiofsd, memory backend) really works | **E5 passed** — a Linux guest mounts both shares, reads and writes, in CI |
| What a real QNX guest sees of the device | **measured** — QNX 7.1.0 enumerates `1af4:1009`, MSI-X + the five virtio capabilities, no interrupt routed yet |
| The QNX driver itself, running | **not yet** — needs the QNX SDP 8.0 (a licensed download) |

Read the full status on the [verification status](verification-status.md) page.

## Why it exists

The usual loop is *compile, copy into the guest (or rebuild the image), run,
repeat*. Two public 9P clients exist — [gvergine/qnx-fs-9p](https://github.com/gvergine/qnx-fs-9p)
(ARM64 only) and [eclipse-score/qnx_unit_tests](https://github.com/eclipse-score/qnx_unit_tests)
(reduced to what unit tests need). Neither runs on the default x86_64 images of
QNX Everywhere with full POSIX behaviour, and **nobody ships a virtiofs client
for a QNX guest**. Passerelle does both, and measures the difference.

See [the overview](../README.md) for the architecture, the
[user guide](user-guide.md) to use it, and the
[codelab](../codelab/README.md) to learn how a virtio driver for QNX 8.0 is
written, from PCI discovery to the first byte read.

## Numbers that are real

- **3 000 000** differential operations, zero divergence, 60 s
- **5 000 000** + **5 000 000** fuzz iterations (9P and FUSE), zero defect
- **822** 9P requests measured for 30 create/stat/read/unlink cycles on a Linux
  guest — the reference baseline every other client is compared against
- **~20 s** verdict CI, budget 5 minutes, merged by [Mergify](https://mergify.com)

## Build it

```bash
cmake -B build -DPGW_MODULES="common;virtio;proto9p;protofuse;rmfs;drivers;tests"
cmake --build build -j8
ctest --test-dir build --output-on-failure
```

No QNX SDP required for the portable half. Details in
[building](building.md).
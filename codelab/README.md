# Writing a virtio driver for QNX 8.0, from PCI discovery to the first byte read

A hands-on tutorial. By the end of it you will have a QNX 8.0 guest that
finds a virtio-9p device on the PCI bus, negotiates the protocol with QEMU,
exchanges its first 9P message, and reads the first byte of a file.

**Audience.** Someone who knows C, has heard of QNX resource managers, and wants
to write a driver for QNX 8.0 rather than for 7.1. No prior virtio knowledge
is assumed; every structure a driver touches is introduced before it is used.

**Prerequisites.**

| | |
|---|---|
| QNX SDP 8.0 | the toolchain (`qcc`) and `mkqnximage`; a licence is needed, the tutorial does not need a QNX licence key for the parts that run in the guest |
| QEMU 8.2 or later | the hypervisor; `qemu-system-x86_64` and `qemu-system-aarch64` |
| Linux | the host; a recent distribution is fine |
| CMake 3.16 or later | for the exercises |
| A C11 compiler | for the exercises, which run on the PC |

**Time.** About four hours end to end, and the four hours are not equal: the
first two chapters are quick, chapters 3 to 5 are where the time goes, and
chapter 6 is the one that can eat an afternoon if the interrupt does not work.

**What you will have at the end.**

- A guest image built with `mkqnximage` that boots under QEMU.
- A program that finds the virtio-9p PCI device, reads its capabilities,
  negotiates `9P2000.L`, walks to a file, and reads its first byte.
- A resource manager skeleton that turns the prototype into something a
  program can `open()`.
- A checklist of the places where this goes wrong, and how to tell which one
  you are looking at.

**The code.** Every listing in this tutorial is either a complete, compilable
program in [`code/`](code/) — all of them run on your PC, none of them needs a
hypervisor — or a fragment of the real driver, marked as such, taken from
[`virtio/`](../../virtio/) and [`proto9p/](../../proto9p/) in this repository.
The fragments and the exercises use the same names, so the transition from
"tutorial" to "real driver" is a matter of swapping the simulated device for
the real one.

**Conventions.** The little-endian wire format is assumed throughout, which is
true of both QNX 8.0 targets (x86_64 and aarch64le). Code shown is C11. The
`pgw_` prefix belongs to this project's public API; the tutorial's own
portable modules use the `clab_` prefix so you can tell them apart at a
glance.

## Contents

| Chapter | Title | What you build |
|---|---|---|
| [01](01-environment-and-first-image.md) | The environment and your first image | an image that boots, and a check of the assumptions you are about to make |
| [02](02-the-pci-bus-under-qnx-8.md) | The PCI bus under QNX 8.0 | `pci_device_find()`, `pci_devhdl_t`, reading configuration space |
| [03](03-finding-a-modern-virtio-device.md) | Finding a modern virtio device | the capability table, and where every register really is |
| [04](04-features-and-the-status-sequence.md) | Features and the status sequence | a negotiation that never lies about what the device offered |
| [05](05-the-split-virtqueue.md) | The split virtqueue | descriptors, the available ring, the used ring, step by step |
| [06](06-interrupts.md) | Interrupts | `InterruptAttachThread()`, MSI-X with an INTx fallback |
| [07](07-the-first-9p-exchange.md) | The first 9P exchange | `Tversion`/`Rversion`, `Tattach`, `Twalk`, and the first byte read |
| [08](08-from-prototype-to-resource-manager.md) | From prototype to resource manager | the same code, behind a QNX resource manager |

## How to use the exercises

Each chapter ends with an exercise and its solution. The exercises live in
[`code/`](code/) and build on their own, with nothing from the rest of the
repository:

```sh
cmake -B /tmp/codelab-build -S codelab/code
cmake --build /tmp/codelab-build
ctest --test-dir /tmp/codelab-build --output-on-failure
```

The exercise programs are also runnable by hand, and they print what they see,
which is the point: the split virtqueue chapter prints the descriptor table
between every step, because on a real device you cannot do that.

| Exercise | Chapter | What it demonstrates |
|---|---|---|
| `ex01_env` | 1 | the identity numbers and the byte order the rest of the tutorial relies on |
| `ex02_pci_caps` | 3 | decoding a capability table, and the fallback when `0x34` cannot be read |
| `ex03_negotiate` | 4 | the status sequence, the intersection rule, and what to do when the device says no |
| `ex04_vring` | 5 | a request on a split virtqueue, and the reply coming back |
| `ex05_irq` | 6 | the fallback ladder, and the order the interrupt thread must follow |
| `ex06_first_byte` | 7 | the 9P messages of the first exchange, byte by byte |
| `test_clab` | all | the solutions to every exercise, as a test suite |

## A word on what is verified

The exercises run on the PC and are tested by the repository's CI. The QNX
parts — the PCI calls, the interrupt thread, the resource manager — are shown as
listings and are the parts you will have to run yourself in a guest; the
tutorial says exactly which lines to add to which image, and where the output
that proves each step happened should appear.

## Further reading

- The virtio specification, version 1.2 (OASIS): §2.1 device status, §2.7
  split virtqueues, §3.1 device initialization, §4.1 the PCI transport, §5.3
  the 9P device.
- The 9P2000.L protocol description (the `diod` project's `protocol.md` is the
  usual reference).
- QNX 8.0 documentation: the Resource Manager guide, the Interrupt Service
  guide, the PCI chapter, and the `mkqnximage` utility page.
- QEMU's documentation of the 9p filesystem device and of the virtiofs
  device.

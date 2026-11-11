# Passerelle

**Passerelle** is a high-performance host-guest shared filesystem driver for **QNX® Neutrino® 8.0** virtual machines under QEMU, providing seamless directory sharing via **9P2000.L** and **virtiofs** over modern virtio-PCI and virtio-MMIO.

Submitted to the **QNX Global Hackathon 2026** under the [Apache-2.0](LICENSE) license.

---

## Why Passerelle?

Developing software for QNX virtual machines today usually means suffering a slow iteration loop:
```
[Host] cross-compile  ──>  [SCP / rebuild disk image]  ──>  [Guest] boot / transfer  ──>  [Guest] test run
```
Every change pays a "copy tax". If the VM image is read-only or minimal, you must regenerate the entire image just to test one binary.

With Passerelle, your build directory on the host PC is mounted directly inside the QNX guest:
```
[Host] cross-compile  ──>  [Guest] run immediately from /host
```
Binaries execute straight from the share, shared libraries load on demand, and files created or modified by QNX are instantly available on the host.

---

## Prior Art & What Passerelle Adds

In accordance with open-source engineering ethics and hackathon standards, Passerelle explicitly acknowledges prior art. Two public 9P clients and one virtio transport library exist for QNX:

1. **`gvergine/qnx-fs-9p`** (October 2026, Apache-2.0):
   A feature-rich 9P2000.L client designed for everyday development, but **strictly restricted to ARM64 guests** over virtio-mmio. It does not support x86_64 and has no PCI transport.
2. **`eclipse-score/qnx_unit_tests` (`virtio9p`)** (March 2026, Apache-2.0):
   Supports both x86_64 (PCI) and ARM64 (MMIO), but is **intentionally stripped down to test runner needs**: no symbolic links, no permission or ownership changes (`chmod`/`chown`), no timestamp updates, no file locking.
3. **`kodebooth/qnx-virtio`** (April 2026, MIT):
   A transport library for virtio on QNX 8.0 with a sample GPIO driver, but no filesystem implementation and no host-side testability.

### What Passerelle Adds

| Capability | `qnx-fs-9p` (A) | `qnx_unit_tests` (B) | **Passerelle** |
|---|:---:|:---:|:---:|
| **Full x86_64 Guest Support** (PCI modern, MSI-X) | ❌ *ARM64 only* | ⚠️ *Minimal ops only* | **Target: complete POSIX share** |
| **Full ARM64 Guest Support** (MMIO v1 & v2) | ✅ Yes | ⚠️ *Minimal ops only* | **Target: complete POSIX share** |
| **POSIX Operations** (symlinks, chmod, chown, utime, df) | ✅ Yes | ❌ No | **Target: yes (redirect symlinks, atomic renames)** |
| **virtiofs for QNX Guests** (FUSE over virtio) | ❌ No | ❌ No | **✅ Target (X tier)** — no public implementation found |
| **Real-Time Priority Scheduling** | ❌ FIFO | ❌ Unspecified | **Target: priority-ordered dispatch** |
| **Host PC Testability without QNX SDP** | ❌ Needs SDP | ❌ Needs Bazel/SDP | **✅ Portable L1/L2/L3, PC unit tests** |
| **Differential Conformance Testbed** | ❌ Trace-only | ❌ None | **✅ Harness written** (`host/fsproof`, 1M-op target) |
| **One-Command Setup CLI** (`qshare`) | ❌ Manual QEMU | ❌ Manual script | **✅ `qshare` tool written** |

**Reading this table honestly.** The "Passerelle" column states what the project
*provides by design and targets*; the columns for projects A and B state what their
code and documentation show today. Implementation status per requirement — including
what is not yet done — is in [docs/verification-status.md](docs/verification-status.md).
The absence of a public QNX virtiofs guest driver is a **search result, not a proof**
(spec §14).

See [docs/state-of-the-art.md](docs/state-of-the-art.md) for detailed code-level analysis and [NOTICE](NOTICE) for project attributions.

---

## Architecture

Passerelle follows a modular three-layer design:

```
  QNX Applications: open, read, write, stat, readdir, execve...
        │  QNX Messages handled at caller priority (QNX-03)
 ┌──────▼────────────────────────────────────────────────────────┐
 │ Layer 3 — Filesystem Resource Manager (`rmfs/`)               │
 │   Path namespace, fids/inodes, permission checks (SEC-01),     │
 │   guest-local range locks (FS-14), priority scheduler (QNX-04)│
 ├──────────────────────────────┬────────────────────────────────┤
 │ Layer 2 — 9P2000.L Protocol  │ Layer 2 — FUSE Protocol        │
 │ (`proto9p/`)                 │ (`protofuse/` - virtiofs)      │
 │ Pure C11, strict decoders    │ Pure C11, node refcounts       │
 ├──────────────────────────────┴────────────────────────────────┤
 │ Layer 1 — virtio Transport (`virtio/`)                        │
 │   Split virtqueue, indirect descs (VIO-08), zero-copy (VIO-07) │
 │   Backends: PCI Modern (x86_64), MMIO v1/v2 (ARM64), PC Mock  │
 └──────┬────────────────────────────────────────────────────────┘
        │
   QEMU: `virtio-9p-pci` / `virtio-9p-device`  or  `virtiofsd`
        │
   Host Workstation Directory
```

See [docs/architecture.md](docs/architecture.md) for full design documentation.

---

## One-Command Demo

On the host workstation (tooling status: see [verification-status.md](docs/verification-status.md)):

```bash
# 1. Share your project directory and start the VM
qshare ~/my-project

# 2. Inside the QNX VM, the host folder is mounted at /host
ls -la /host

# 3. Cross-compile on the host, run in QNX with zero copying
make -C ~/my-project
/host/bin/my_program
```

See [docs/user-guide.md](docs/user-guide.md) for mount options, troubleshooting, and CMake/Rust workflows.

---

## Building

Passerelle targets **QNX SDP 8.0** (`x86_64le` and `aarch64le`) and builds natively on Linux for PC testing.

### 1. Build on Host PC (Portable Layers & Unit Tests, No SDP Needed)

The transport abstraction, protocol codecs, and resource manager core are 100% portable C11 and run directly on your Linux host:

```bash
# Standard build and CTest execution
cmake -B build -DPGW_MODULES="common;virtio;proto9p;protofuse;rmfs"
cmake --build build -j8
ctest --test-dir build --output-on-failure

# Build with AddressSanitizer and UBSan
cmake -B build-asan -DPGW_SANITIZERS=ON
cmake --build build-asan -j8
ctest --test-dir build-asan --output-on-failure
```

### 2. Cross-Compile for QNX 8.0

Requires QNX SDP 8.0. Source the SDP environment, then use the toolchain file shipped with the SDP (no repository-local toolchain wrapper exists yet — see [docs/building.md](docs/building.md)):

```bash
source ~/qnx800/qnxsdp-env.sh

# For an x86_64 guest
cmake -B build-qnx-x86 \
      -DCMAKE_TOOLCHAIN_FILE="$QNX_HOST/usr/cmake/QNXToolchain.cmake" \
      -DCMAKE_SYSTEM_PROCESSOR=x86_64
cmake --build build-qnx-x86 -j8

# For an aarch64le guest
cmake -B build-qnx-arm \
      -DCMAKE_TOOLCHAIN_FILE="$QNX_HOST/usr/cmake/QNXToolchain.cmake" \
      -DCMAKE_SYSTEM_PROCESSOR=aarch64le
cmake --build build-qnx-arm -j8
```

Detailed step-by-step instructions and test level runner commands are available in [docs/building.md](docs/building.md).

---

## Honest Verification Status

Every requirement is tracked against automated testing, and this README only
claims what a command in this repository actually proves. What still needs a QNX
SDP or a QNX guest is said so explicitly.

- **Unit & codec layers (level 0)** — implemented and green on the host PC for
  every portable layer: `common`, `virtio`, `proto9p`, `protofuse`, `rmfs` and
  the portable half of `drivers` (`ctest --test-dir build`). The QNX-only glue
  is compiled on the PC against `tests/qnx-stub` (job `qnx-syntax`), which
  proves it parses and type-checks, not that it runs.
- **Randomized input (level 1)** — `tests/fuzz` runs deterministic seeded
  campaigns against every wire decoder, in CI, under ASan+UBSan
  (`fuzz_p9`, `fuzz_fuse`). The long campaigns (`fuzz-*-long`, millions of
  iterations) are registered but disabled: they run outside pull requests.
- **Differential conformance (level 2)** — `tests/e2e` replays seeded random
  operation suites through `pgw_rmfs` and compares every result with the PC
  folder, which is the truth (`test_diff`, plus the directed cases of spec 7.4
  in `test_directed`). A divergence is automatically reduced to the shortest
  prefix that reproduces it. `host/fsproof` runs the same method against a real
  guest. The 1 000 000-operation campaign of spec 7.6 is `e2e-long`, out of PR.
- **Performance (level 3) and robustness under QEMU (level 4)** — the harnesses
  are written (`host/bench`, `host/fsproof`); the numbers require a QNX guest
  and are **not** published yet. Only the round-trip *counting* method is
  implemented and unit-tested against recorded QEMU traces.
- **Feasibility tests E1–E5** — all blocked on the QNX SDP except E5, which only
  needs a Linux guest. Tracked as issues #2 to #6.
- **No QNX-side requirement is claimed as verified**: no QNX SDP is installed on
  the machine that produced this tree.

See [docs/verification-status.md](docs/verification-status.md) for the full matrix.

---

## Documentation Index

- [docs/spec-en.md](docs/spec-en.md) — Complete English Specification (Requirements VIO, P9, FS, QNX, PERF, ROB, SEC, DX, VFS, SRV)
- [docs/spec-fr.md](docs/spec-fr.md) — Original French Specification (authoritative text)
- [docs/architecture.md](docs/architecture.md) — Detailed Three-Layer Design & Tricky Issues Resolution
- [docs/user-guide.md](docs/user-guide.md) — User Guide, Mounting Options, CMake and Rust Workflows
- [docs/state-of-the-art.md](docs/state-of-the-art.md) — Comprehensive Analysis of Existing Projects
- [docs/decisions.md](docs/decisions.md) — Architectural Decisions Log (D1–D4)
- [docs/verification-status.md](docs/verification-status.md) — Honest Requirements & Feasibility (E1–E5) Status
- [docs/building.md](docs/building.md) — Compilation, Toolchain, and Test Runner Guide
- [docs/video-script.md](docs/video-script.md) — 3-Minute Hackathon Video Script
- [docs/ci.md](docs/ci.md) — Self-Hosted CI Infrastructure & 5-Minute Budget Guard

---

## Repository CI

This repository uses a self-hosted CI pipeline on dedicated Dell PowerEdge R630 workers enforcing strict C11 compliance, AddressSanitizer verification, a **5-minute maximum runtime budget per job**, and automated merge queues via Mergify. Details are documented in [docs/ci.md](docs/ci.md).

---

## License & Attribution

Passerelle is distributed under the **Apache License, Version 2.0**.  
See [LICENSE](LICENSE) for full legal text and [NOTICE](NOTICE) for third-party project attributions.

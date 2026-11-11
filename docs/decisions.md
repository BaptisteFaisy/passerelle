# Architecture & Strategy Decisions Log

This document records the four key architectural and organizational decisions (D1–D4) identified in Section 3.2 of the [specification](spec-en.md). In particular, Decision **D3** records the **actual code-level evaluation** of `kodebooth/qnx-virtio` performed on 10–11 November 2026.

---

## Summary of Decisions

| ID | Title | Options | Recommendation | Status |
|---|---|---|---|---|
| **D1** | Submission Category | Low-Level Software vs Developer Tools vs Open Source Contribution | Ask Discord organizers; default to Developer Tools | **Pending (awaiting Discord feedback)** |
| **D2** | 9P Approach: Rewrite vs Upstream Contribution | (1) Rewrite from scratch, (2) Contribute to Project A (`qnx-fs-9p`), (3) virtiofs first | Combine 2 & 3: build PCI transport, offer upstream to A, implement in new repo as fallback | **Decided** |
| **D3** | Transport: Reuse `kodebooth/qnx-virtio` or Write Ours | (1) Vendor `qnx-virtio`, (2) Write clean portable Layer 1 (`virtio/`) | **Write our own portable Layer 1** | **Decided (backed by code evaluation)** |
| **D4** | Caching Policy | (1) Aggressive page + attr cache, (2) Bounded attr cache only, (3) Zero data cache by default | **Zero data cache by default**; optional bounded attr cache | **Decided** |

---

## Detailed Decisions

### Decision D1: Hackathon Submission Category

- **Context**: The QNX Global Hackathon 2026 defines three eligible categories:
  1. *Low-Level Software*: defined by the QNX Driver Development Kit (DDK), covering serial, I2C, SPI, HID, touch, network, and CAN.
  2. *Developer Tools*: development utilities, test frameworks, automation, and workflow accelerators.
  3. *Open Source Contributions & Ports*: porting existing OSS or contributing major upstream enhancements.
- **The Dilemma**: A virtio-based shared filesystem driver is fundamentally low-level kernel/driver software (it programmes PCI BARs, registers virtqueues, configures MSI-X, and registers a filesystem resource manager with `procnto`). However, it uses **none** of the official QNX DDK frameworks (the DDK has no class for virtio devices or filesystem drivers).
- **Options**:
  - *Option 1*: Submit under "Low-Level Software", highlighting PCI, interrupts, and hardware interaction. Risk: judges disqualify based on strict DDK definition.
  - *Option 2*: Submit under "Developer Tools", presenting Passerelle as the ultimate tool accelerating the edit-compile-test loop. Risk: stiffest competition, but zero qualification ambiguity.
  - *Option 3*: Submit under "Open Source Contributions & Ports" by virtue of our upstream patch to `gvergine/qnx-fs-9p` and Linux 9P/FUSE standards.
- **Recommendation**: Inquire directly with hackathon organizers on the official Discord server during Week 1. Unless organizers explicitly confirm that non-DDK virtio drivers qualify for "Low-Level Software", **submit under "Developer Tools"**.
- **Status**: **Pending** (inquiry queued for Discord; non-blocking for development).

---

### Decision D2: 9P Client — Rewrite vs Upstream Contribution

- **Context**: `gvergine/qnx-fs-9p` (Project A) already implements a rich 9P2000.L client for QNX 8.0, but supports **only ARM64** over virtio-mmio.
- **Options**:
  - *Option 1 (Rewrite everything)*: Write a third 9P client from scratch. Duplicates existing effort without novelty.
  - *Option 2 (Contribute upstream)*: Add PCI transport and x86_64 target to Project A, offering patches via `upstream/qnx-fs-9p/`. Highly ethical and collaborative.
  - *Option 3 (virtiofs first)*: Skip 9P entirely and focus on virtiofs. Leaves x86_64 users without the simplest out-of-the-box sharing mechanism.
- **Recommendation**: **Combine Options 2 and 3**. Implement a common Layer 1 (PCI modern transport) that serves both 9P and virtiofs. Offer the PCI transport and x86_64 support back to Project A as a clean pull request. If Project A's maintainer is unresponsive, Passerelle maintains the complete 9P client in its own repository, reusing protocol concepts with full attribution (per [NOTICE](../NOTICE)).
- **Status**: **Decided**.

---

### Decision D3: Transport Layer — Detailed Code Evaluation of `kodebooth/qnx-virtio`

- **Context**: `kodebooth/qnx-virtio` (April 2026, MIT license) is a public virtio library for QNX 8.0 containing core virtqueue, PCI, and MMIO implementations, plus a sample GPIO driver.
- **Code Inspection Findings**: On 10–11 November 2026, we performed a thorough line-by-line inspection of the repository in `passerelle-refs/qnx-virtio/`:
  1. **Tight Coupling to QNX Neutrino APIs**:
     - `src/virtio/virtio.c` includes `<sys/neutrino.h>` and calls `InterruptAttachThread()` and `InterruptUnmask()` directly.
     - `src/virtio_pci/virtio_pci.c` directly calls QNX `libpci` functions (`pci_device_attach`, `pci_device_read_cap`, `cap_msix_set_nirq`, etc.).
     - **Impact**: It is impossible to compile or unit-test `qnx-virtio` on a Linux host PC without the QNX SDP. This directly violates requirement **P9-10** and the foundational design rule that Layers 1 and 2 must build and run tests natively on the host workstation.
  2. **Modern-Only PCI Addressing**:
     - In `src/virtio_pci/virtio_pci.c`, line 16 defines:
       ```c
       #define VIRTIO_PCI_DID(type) ((uint16_t)(0x1040 + (uint16_t)(type)))
       ```
     - Line 616 sets `dev->leagcy = false`.
     - It exclusively searches for modern device IDs (`0x1049` for 9P) and does **not** recognize transitional device IDs (`0x1009`, required by VIO-01 and standard QEMU configurations).
  3. **Lack of Advanced Virtqueue Features**:
     - In `src/virtio/virtq.h` and `virtq.c`, descriptor chains are allocated one by one via `virtq_alloc_desc()` and linked via `virtq_put_desc_chain()`.
     - **No indirect descriptor tables** (`VIRTIO_F_INDIRECT_DESC`, requirement **VIO-08**) are supported.
     - **No event index notification suppression** (`VIRTIO_F_EVENT_IDX`, requirement **VIO-08**) is negotiated; rings merely reserve bytes for event fields without using them.
  4. **Allocation and Concurrency Patterns**:
     - Transport operations lock a single coarse `pthread_spinlock_t`.
     - Device reset polls in a `delay()` loop (`virtio_reset_device`), which blocks the calling thread instead of integrating cleanly with asynchronous event loops.
     - No scatter-gather zero-copy abstraction matching filesystem payloads (`pgw_vio_sg`, requirement **VIO-07**).
  5. **Adaptation Cost**:
     - Decoupling `qnx-virtio` into an OS-independent core plus OS-specific bus accessors would require rewriting ~80% of `src/virtio/` and `src/virtio_pci/`.
     - The maintenance burden of maintaining an internal fork with diverged APIs exceeds writing a purpose-built, clean-room C11 Layer 1.
- **Conclusion**:
  - We do **NOT** vendor or depend on `kodebooth/qnx-virtio`.
  - Passerelle implements its own Layer 1 (`virtio/`, header `pgw/virtio.h`), designed from day one with an abstract bus operations table (`pgw_vio_bus_ops`), supporting PC mock backends (`pgw_vio_mock_open`), indirect descriptors, zero-copy scatter-gather, and both modern/transitional PCI IDs.
  - `kodebooth/qnx-virtio` is cited with gratitude in [NOTICE](../NOTICE) and [state-of-the-art.md](state-of-the-art.md) for illustrating the QNX 8.0 `libpci` MSI-X API calls.
- **Status**: **Decided (Evaluated & Rejected)**.

---

### Decision D4: Caching Strategy

- **Context**: Traditional remote filesystems rely on aggressive data caching to mask round-trip latency.
- **The Problem**: In a software development workflow (the primary use case for Passerelle), aggressive data caching is actively dangerous. When a developer edits or recompiles source files on the host PC, the guest must observe the updated file contents **immediately**. The QEMU 9P documentation explicitly highlights that Linux guest data caches do not revalidate changes made on the host, leading to stale reads and phantom bugs.
- **Options**:
  - *Option 1 (Aggressive Cache)*: Cache both file data and directory attributes. High benchmark numbers, broken developer experience.
  - *Option 2 (Zero Cache)*: Read and write directly through to the hypervisor on every call. Guarantees strict consistency; lower synthetic metadata benchmark numbers.
  - *Option 3 (Hybrid / Bounded Attribute Cache)*: **Zero data cache by default**; optional, bounded-TTL attribute cache for metadata-heavy builds (e.g. `attr_ttl=1000ms`, ceiling 10 s).
- **Recommendation**: **Adopt Option 3**.
  - Requirement **FS-12** is paramount: a file modified on the host PC must be visible and updated on the guest's very next `read()` without requiring a remount.
  - Data caching is disabled by default (`PGW_DATA_CACHE_DEFAULT = 0`).
  - Metadata attribute caching is off by default (`PGW_ATTR_CACHE_NS_DEFAULT = 0`), with an explicit command-line opt-in (`attr_ttl=<ms>`) capped at 10 seconds (`PGW_ATTR_CACHE_NS_MAX`).
- **Status**: **Decided**.

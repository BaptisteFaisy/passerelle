# State of the Art

This document extends Section 2 of the [specification](spec-en.md) with **code-level
verification** of the three existing public projects, performed on 10–11 November 2026
by reading the repositories vendored read-only for this study.

> **Honesty rule (spec Section 14).** For every fact below we state its source:
> **[code]** = read directly in the project's source files (file paths given);
> **[docs]** = read only in the project's documentation, not verified by execution;
> **[unverified]** = claimed by us but not yet confirmed. **None of the three
> projects has been executed** — the host PC has no QNX SDP.

---

## 1. Comparative Table

| | **A — `gvergine/qnx-fs-9p`** | **B — `eclipse-score/qnx_unit_tests`** (folder `src/common/virtio9p`) | **C — `kodebooth/qnx-virtio`** |
|---|---|---|---|
| Date / activity | Created 3 Oct 2026, single commit **[docs]** | Created Mar 2026, active **[docs]** | April 2026 **[docs]** |
| License | Apache-2.0 **[code: `LICENSE`]** | Apache-2.0, Eclipse Foundation headers **[code: file headers]** | MIT, © 2026 Abe Kohandel **[code: `LICENSE.md`, SPDX headers]** |
| Language | C **[code: `src/*.c`, `lib9p/`]** | C++ (namespaces, `iofunc_*` C++ wrappers) **[code]** | C **[code: `src/virtio*/`]** |
| Size (measured) | ~5,500 lines (`src/` + `lib9p/`) **[code: `wc -l`]** | ~1,200 lines (`virtio9p/`) **[code: `wc -l`]** | ~2,700 lines (`src/virtio*`) **[code: `wc -l`]** |
| Stated goal | Everyday development; image stays minimal, programs and libraries run straight from the share **[docs: README]** | Share test binaries and data; "test infrastructure only" **[docs: README]** | Generic virtio transport library + sample GPIO driver **[docs: README]** |
| Guest architectures | **aarch64 only**; "x86_64 guests are not supported and were never tested" **[docs: README — confirmed by code: only a `virtio-mmio` backend exists in `src/transport_virtio.c`, no PCI file]** | x86_64 (PCI) and ARM64 (MMIO) **[code: `src/transport/pci_transport.cpp`, `mmio_transport.cpp`]** | x86_64 and aarch64le **[code: `cmake/toolchains/qnx_x86_64.cmake`, `qnx_aarch64.cmake`; README presets]** |
| Filesystem protocol | Full 9P2000.L: message enum incl. `Tsymlink`, `Tstatfs`, setattr path **[code: `lib9p/include/lib9p/p9.h`]** | Reduced 9P2000.L subset: `Tversion, Tattach, Twalk, Tlopen, Tlcreate, Tread, Twrite, Tclunk, Tgetattr, Treaddir, Tmkdir, Tunlinkat, Trenameat` — **no** setattr/symlink/readlink/link/fsync/statfs/flush **[code: `include/protocol/nine_p_types.h`]** | None — transport only **[code: no filesystem protocol in `src/`]** |
| Transport | virtio-mmio only, QEMU `virt` machine **[code: `src/transport_virtio.c` header comment]** | PCI modern (vendor 0x1AF4, device 0x1009) + MMIO v1 and v2 **[docs: README; code: both transport files exist]** | PCI **modern-only** (`0x1040 + type` device IDs, `dev->leagcy = false`) + MMIO v1 & v2 registers incl. queue reset **[code: `src/virtio_pci/virtio_pci.c` L16/L616, `src/virtio_mmio/virtio_mmio.c`]** |
| Interrupts | Not examined in depth | MSI-X programmed manually via raw config-space reads (`kCapIdMsix = 0x11`, table BIR/offset, APIC vector arithmetic) with INTx fallback **[code: `pci_transport.cpp` L147–361]** | MSI-X via QNX `libpci` (`cap_msix_set_nirq`, `cap_msix_set_irq_entry`, `pci_device_cfg_cap_enable`, `cap_msix_unmask_irq_entry`), one IST per device using `InterruptAttachThread()` + `InterruptWait()` **[code: `virtio_pci.c` L655–690, `virtio.c` L37–48]** |
| Linux↔QNX errno mapping | Explicit table, "never a cast", every asm-generic value mapped, fallback `EIO` **[code: `src/errno_map.c` header + switch]** | Linux open-flag constants present **[code: `nine_p_types.h` L109]**; errno mapping not examined | N/A (transport only) |
| Operations coverage | read, write, create, truncate, chmod/chown/utime, fsync, mkdir/rmdir, rename, symlinks, hard links, `df`, exec from share **[docs: README — consistent with the message enum in `lib9p/p9.h`, but not executed]** | read, write, create, dirs, unlink, rename **[code: message enum]** | N/A |
| Privileges | Non-root with three abilities: `mem_phys`, `interrupt`, `pathspace` **[docs: README + `docs/user/integrating.md`; exact `on -u ... -A` command line shown there]** | Unspecified | Sample uses `ThreadCtl(_NTO_TCTL_IO)` (root or IO privilege) **[code: `src/gpio/main.c`]** |
| Queue features | Unspecified | Unspecified | Descriptor chains via `virtq_put_desc_chain`; ring layout reserves `avail_event`/`used_event` slots; **no indirect-descriptor support and no `EVENT_IDX` negotiation found** **[code: `src/virtio/virtq.h`, `virtq.c`]** |
| Concurrency model | Multiple in-flight requests, signal interruption, clean unmount, timeout recovery, optional metadata cache **[docs: README — `src/cache.c` exists in code]** | Unspecified; global OCB state (`g_last_ext_ocb`) suggests a simplified single-threaded model **[code: `fs_virtio9p.cpp` L108–214]** | pthread spinlock around device ops; reset with polling timeout **[code: `virtio.c`]** |
| Testing | Comparison against QEMU 9P server trace events **[docs: README Credits]**; `tests/unit` + `tests/integration` directories exist **[code: file listing]** | Unspecified; part of a Bazel-based QEMU test runner **[docs: top-level README]** | pytest integration tests driving QEMU over serial (`tests/gpio/test_gpio.py`); **requires SDP + QEMU, no PC unit tests** **[code: test sources]** |
| PC buildability (no SDP) | No — requires QNX SDP 8.0 **[docs: README "Requires QNX SDP 8.0 to build"]** | No — Bazel + QNX cross-toolchain **[docs]** | No — `src/virtio/virtio.c` includes `<sys/neutrino.h>` directly **[code]** |

---

## 2. What We Read in the Code vs Only in the Documentation

### Read in the code (paths are inside the vendored copies)

- **A**: `lib9p/include/lib9p/p9.h` (full 9P2000.L message enum incl. `Tsymlink=16`, `Tstatfs=8`), `src/errno_map.c` (Linux→QNX errno table with explicit switch, `EIO` fallback), `src/transport_virtio.c` (mmio-only backend), `LICENSE` (Apache-2.0).
- **B**: `include/protocol/nine_p_types.h` (exact 13-message subset), `src/transport/pci_transport.cpp` (manual MSI-X programming with INTx fallback; device ID 0x1009 per README), `src/resmgr/fs_virtio9p.cpp` (resmgr/iofunc integration), Eclipse Apache-2.0 headers.
- **C**: `src/virtio/virtio.c` (`InterruptAttachThread` IST loop, spinlocks, polled device reset), `src/virtio/virtq.h/.c` (descriptor allocation/chaining API, no indirect descriptors, no EVENT_IDX feature negotiation), `src/virtio_pci/virtio_pci.c` (modern-only PCI IDs `0x1040+type`, full `libpci` MSI-X sequence, capability parsing incl. `SHARED_MEMORY_CFG=8`), `src/virtio_mmio/virtio_mmio.c` (v1 and v2 register maps incl. per-queue reset), `tests/gpio/` (QEMU/serial integration tests), `cmake/toolchains/` (x86_64 + aarch64le), `LICENSE.md` (MIT).

### Read only in documentation (not verified by execution)

- **A**: all functional claims (exec-from-share, symlink behavior, timeout recovery, cache semantics, ability set); its "gaps" list in the user guide was **not read**.
- **B**: claimed PCI auto-discovery details beyond what the code inspection showed; performance characteristics; ARM64 MMIO base addresses (`smem=0xa003600,irq=75` in README examples).
- **C**: README build/test workflow; the claim that the library is reusable "for other virtio drivers".
- **All three**: runtime behavior under QEMU. Nothing was executed — feasibility tests E1–E5 cover this (see [verification-status.md](verification-status.md)).

---

## 3. Gaps Passerelle Fills

| Gap (spec Section 2) | Evidence from this study |
|---|---|
| **Complete share on x86_64 guest** | A has no PCI backend in code; B's x86_64 client implements only 13 of the ~25 messages needed for POSIX parity (no setattr, symlink, fsync, statfs) |
| **virtiofs for a QNX guest** | None of A/B/C contains any FUSE protocol code; QNX Hypervisor's virtio-fs is host-side only **[docs: QNX hypervisor page, spec §2]** |
| **Conformance + benchmark testbed applicable to all clients** | A tests against server traces; B has none documented; C tests only its GPIO sample — none provides a differential harness runnable against a third-party client |
| **Priority-ordered request scheduling** | No priority logic found in A/B/C code; C uses a plain spinlock-serialized queue |
| **One-command setup on `mkqnximage` images** | A requires manual snippet + QEMU flags **[docs: README quickstart]**; B is embedded in a Bazel runner; C has no host tooling |
| **PC-buildable, fuzz-testable protocol layers (P9-10)** | None of the three builds without the QNX SDP (A: SDP required; B: Bazel+SDP; C: `<sys/neutrino.h>` in core transport) |

## 4. Other Observations

- **QNX Hypervisor virtio-fs**: QNX documents a virtio-fs *device* where QNX is the
  **host** sharing folders with Linux/Android guests **[docs: spec §16 link]**. No
  guest-side QNX virtiofs driver was found; absence remains **unproven** — the
  question will be asked on the hackathon Discord (spec §11, §14).
- **Reuse posture**: per [decisions.md](decisions.md), Passerelle writes its own
  transport (D3) and offers the PCI/x86_64 work back to project A (D2). No file is
  copied from any of the three repositories; see [NOTICE](../NOTICE).

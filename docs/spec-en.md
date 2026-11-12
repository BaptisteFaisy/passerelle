> **Provenance**: Complete and faithful English translation of the source specification `docs/spec-fr.md` ("Partage d'un dossier du PC avec QNX sous QEMU (9P et virtiofs)"), version 0.1, 11 November 2026. The French original remains authoritative for initial requirements text.

# Sharing a Host Directory with QNX under QEMU (9P and virtiofs) — Specification

**Working title: "Passerelle".**  
Version 0.1 — 11 November 2026 — candidate project for the QNX Global Hackathon.  
Submission deadline: Tuesday 15 December 2026 at 20:00 Paris time (UTC+1).

---

## 0. Quick Read

**The need.** Today, testing on QNX a program just cross-compiled on the host requires copying it into the virtual machine or rebuilding the disk image. A shared directory eliminates that step: the binary compiled on the host is visible and runnable in QNX instantaneously.

**What must be known first.** The opening assumption—"nobody wrote a 9P client for QNX"—is false. Two public projects exist, and one is quite mature:

- `gvergine/qnx-fs-9p` (October 2026): complete 9P client designed for day-to-day development, but **restricted to ARM64 guests**. It does not drive the PCI device of x86 machines.
- `eclipse-score/qnx_unit_tests`: 9P client for x86_64 and ARM64, but **reduced to test infrastructure needs**: no symlinks, no chmod/chown, no timestamp updates.

**What genuinely remains to be done, and carries real value:**

1. **A complete share on x86_64**, the default architecture of QNX Everywhere QEMU images. Neither existing project provides this.
2. **virtiofs for a QNX guest.** The successor to 9P, substantially faster. No public implementation was found.
3. **A conformance and benchmark testbed** that objectively compares existing clients with the new one.
4. **Real-time behavior**: a high-priority read must not wait behind a massive low-priority copy.

**Recommended strategy.** Build on prior art for 9P, innovate on virtiofs: achieve a usable 9P share on x86_64 quickly, then invest the bulk of effort on virtiofs and the benchmark harness. See Section 3 for details.

**Three ambition tiers.** **S** foundation (socle), **C** complete, **X** extreme. Each requirement also notes whether an existing project already claims it.

**Two decisions to take immediately** (Section 3): the submission category (the QNX Driver Development Kit defining "Low-Level Software" does not cover filesystem/virtio drivers); and whether to write a new 9P client or contribute to an existing one.

---

## 1. Context

### 1.1 The problem seen from a development workstation

The current development loop under QEMU looks like: compile on the host, copy the binary to the guest (via `scp` or rebuilding the image), execute, repeat. Several hackathon projects automate this loop without eliminating it: according to their descriptions, they rebuild the disk image or push files to the guest on every cycle.

With a shared directory, the loop becomes: compile, execute. Libraries, test data, and build artifacts live directly in a host directory.

### 1.2 Two sharing technologies

| | 9P | virtiofs |
|---|---|---|
| Origin | Plan 9 protocol; 9P2000.L Linux variant | FUSE protocol transported over virtio |
| Host side | Built into QEMU, nothing extra to launch | Dedicated daemon `virtiofsd` + shared guest RAM |
| Supported hosts | Linux; macOS since QEMU 7.0; Windows not integrated | Linux |
| QEMU device | `virtio-9p-pci` (x86) or `virtio-9p-device` (ARM) | `vhost-user-fs-pci` |
| Existing QNX client | Two (see Section 2) | None found |
| Reputation | Simple, slow on metadata operations | Faster, current standard for Linux VMs |

Both share the exact same transport: virtio split virtqueues shared between guest and QEMU. Writing this transport once serves both.

### 1.3 Hackathon framework

- **Eligible categories**: Low-Level Software, Developer Tools, or Open Source Contributions & Ports. The choice is non-trivial; see Decision D1.
- **Judging criteria** (equal weight): Technical Execution, Complete Product, Impact, Innovation.
- **Deliverables**: new public repository under open source license, build instructions specifying QNX version, target and toolchain, video under 3 minutes, all in English.
- **Stackable bonuses** ($3,500 total): Best Use of QNX Technologies; Best Tutorial.

---

## 2. State of the Art

Repositories inspected on 10 and 11 November 2026 based on their documentation. Nothing had been executed yet at that date.

| | A — `gvergine/qnx-fs-9p` | B — `eclipse-score/qnx_unit_tests` (`virtio9p` folder) |
|---|---|---|
| Date | Created 3 October 2026, single commit, 0 stars | Created March 2026, active |
| License | Apache-2.0 | Apache-2.0 |
| Stated goal | Everyday development: minimal image, binaries and libraries run straight from the share | Sharing test binaries and data, "test infrastructure only" |
| Architectures | **ARM64 only.** x86_64 "unsupported and never tested" | x86_64 and ARM64 |
| Transport | virtio-mmio | PCI (auto-discovery, MSI-X with fallback) and MMIO, v1 and v2 |
| Operations | Read, write, create, truncate, permissions, owner, timestamps, `fsync`, directories, rename, symlinks & hard links, `df`, execution from share | Read, write, create, directories, remove, rename |
| Unsupported | Deviations listed in its guide | Permissions, owner, timestamps, symlinks, device nodes, file locks |
| Internals | Multiple in-flight requests, signal interruption, clean unmount, timeout recovery, optional metadata cache | Unspecified |
| Privileges | Non-root with three abilities | Unspecified |
| Testing | Comparison against QEMU 9P server trace events | Unspecified |

**Other useful projects:**

- `kodebooth/qnx-virtio` (April 2026, MIT license): virtio transport library for QNX 8 (core, queues, PCI, MMIO) with a sample GPIO driver. No filesystem.
- **QNX Hypervisor** documents a virtio-fs device, but QNX acts as the **host** sharing its directories with Linux/Android guests. No mention found of a driver for a QNX guest.
- GitHub searches yielding zero results for name and description: "qnx virtiofs", "qnx virtio-fs", "qnx virtfs", "qnx 9pfs", "qnx shared folder".

**What nobody offers today:**

| Gap | Why it matters |
|---|---|
| Complete share (symlinks, permissions, execution) on x86_64 guest | Default architecture of `mkqnximage` virtual machines |
| virtiofs on QNX guest side | Faster technology; first of its kind |
| Comparative benchmarks and conformance harness | No public numbers exist to make informed choices |
| Priority-ordered request scheduling | Expected behavior of any driver on a real-time OS |
| One-command setup on an `mkqnximage` image | Without it, the tool remains restricted to insiders |

---

## 3. Strategy and Decisions

### 3.1 Three approaches

| Option | Description | Pros | Cons |
|---|---|---|---|
| 1 — Rewrite everything | New complete 9P client, then virtiofs | Full control, cohesive architecture | 9P repeats what A already does; 2 weeks spent without novelty |
| 2 — Contribute | Add PCI transport and x86_64 target to project A, offer patch upstream | Fast, honest, immediately useful | Depends on an unknown maintainer; modest glory |
| 3 — virtiofs first | New virtiofs guest driver; existing 9P clients serve as reference | Genuine novelty, measurable | Higher risk; host side Linux-only |

**Recommendation: combine 2 and 3.** PCI transport is needed in all cases. We write it once, use it to deliver complete 9P on x86_64, then build virtiofs on top. The conformance and benchmark harness applies to all clients and becomes an independent deliverable.

If maintainer of A does not respond, the 9P foundation on x86_64 is built in the new repository, reusing its protocol library with attribution (Apache-2.0 permits this) or writing our own.

### 3.2 Key decisions

| | Decision | Recommendation |
|---|---|---|
| D1 | **Category.** The QNX Driver Development Kit (DDK) defining "Low-Level Software" covers serial, I2C, SPI, HID, touch, network, and CAN. A virtio filesystem driver uses none of these. | Ask organizers on Discord. Absent a favorable response, submit under "Developer Tools" where definition fits, despite heavy competition. |
| D2 | **New 9P client vs upstream contribution.** | Contribution first (Option 2), with fallback to new repository. |
| D3 | **Transport: reuse `kodebooth/qnx-virtio` or write our own.** | Evaluate during Week 1. Write our own if adapting takes more than one day. |
| D4 | **Caching.** A cache speeds up I/O, but can hide modifications made on the host. | No data cache by default. QEMU docs warn that Linux guest caches do not revalidate host-side changes: this is the exact pitfall to avoid for development. |

### 3.3 Submission rule

The submission MUST cite both existing projects and state precisely what it adds. One of the judges leads QNX open source engineering; ignoring prior art would be fatal.

---

## 4. Scope

### 4.1 Objectives

- Mount a host PC directory inside a QNX 8.0 guest under QEMU, read-write, on x86_64 and ARM64.
- Run binaries and load shared libraries directly from the share.
- Behave as a standard POSIX filesystem, with documented deviations.
- Prove conformance and benchmark performance against existing clients.
- Provide a one-command setup.

### 4.2 Out of scope

- Other hypervisors (VMware, VirtualBox, QNX Hypervisor as host).
- Network sharing between physical boards: QNX already has NFS and CIFS clients.
- Extended attributes: QNX lacks an interface for them.
- Shared writable memory mapping (`mmap(MAP_SHARED)`) of files from the share.
- virtiofs Direct Access (DAX) to host memory.

### 4.3 Assumptions

- QNX SDP 8.0 through QNX Everywhere, images built via `mkqnximage`.
- Host running Linux. 9P should also work with a macOS host (untested at foundation level).
- QEMU 8.2 or newer.

---

## 5. Proposed Design

This section describes a solution. Only sections 6 and 7 are binding.

### 5.1 Three layers

```
  QNX programs:     open  read  write  stat  readdir  exec ...
        │   QNX messages handled at caller priority
 ┌──────▼─────────────────────────────────────────────────────┐
 │ Layer 3 — Filesystem Resource Manager (rmfs)               │
 │   paths, descriptors, directories, symlinks, permissions   │
 ├──────────────────────────────┬─────────────────────────────┤
 │ Layer 2 — 9P2000.L protocol  │ Layer 2 — FUSE protocol     │
 │                              │ (virtiofs)                  │
 ├──────────────────────────────┴─────────────────────────────┤
 │ Layer 1 — virtio transport                                 │
 │   shared split virtqueues, PCI or MMIO, interrupts         │
 └──────┬─────────────────────────────────────────────────────┘
        │
   QEMU: built-in 9P server      or     virtiofsd on host PC
        │
   Host PC directory
```

- **Layer 1** is shared between both protocols and reusable for other virtio drivers.
- **Layer 2** is portable C. It compiles on the PC where it is tested and fuzzed.
- **Layer 3** is common: both protocols expose the same internal interface (lookup, open, read, write, readdir, create, rename, unlink).

### 5.2 Request path

1. An application calls `read()`. The C library sends a QNX message to the driver.
2. A driver worker thread receives it at caller priority.
3. It builds the protocol request buffer, submits it to the virtio queue, and kicks QEMU.
4. It yields without replying: the client remains blocked, the driver thread is free for another request.
5. QEMU executes the operation against the host directory and writes the response into the used ring.
6. An interrupt wakes a dedicated handler/thread, which matches the response tag/unique ID and unblocks the client.

### 5.3 Tricky points

| Topic | Difficulty | Mitigation |
|---|---|---|
| Linux numeric values | 9P2000.L encodes open flags and errno codes as Linux numbers, differing from QNX | Bidirectional translation tables, tested value by value |
| Symbolic links | On QNX, path resolution is resumed by the client: driver must return a redirection response (`_RESMGR_REDIRECT`), not follow the link itself | Validate against connect message docs; dedicated test |
| Execution from share | Program loader reads and maps the binary through the resource manager | Feasibility test E3 |
| Interrupts | QNX 8.0 removed `InterruptAttach()`; driver must attach a thread | `InterruptAttachThread()` or `InterruptAttachEvent()` |
| PCI interrupt | Project B programs MSI-X directly and depends on PCI server configuration | Feasibility test E1 |
| Message size | Too small collapses throughput. Linux client used 8 KiB default, now 128 KiB | Default chosen by benchmark, user-configurable |
| Identities | QEMU offers several security models; recommended one maps permissions/ownership in xattrs | Recommended model by default, permissions checked in guest |
| Inode numbers | Files on different host partitions may share the same inode number | Recommend QEMU's renumbering option (`multidevs=remap`) |
| Case sensitivity | On macOS hosts, the host filesystem is often case-insensitive | Documented, not fixed |

### 5.4 Target user experience

```
Host    $ qshare ~/project             # prints or injects QEMU options, prepares image
Guest   # ls /host                     # host directory visible
Host    $ make                         # compile on host
Guest   # /host/build/my_program       # executes immediately, zero copy
```

---

## 6. Requirements

Tier: **S** foundation, **C** complete, **X** extreme.  
Verification: **T** automated test, **M** benchmark/measurement, **I** code inspection, **D** live demonstration.  
Prior Art: **A** claimed by `gvergine/qnx-fs-9p` (ARM64 only), **B** claimed by `eclipse-score`, **—** claimed by neither, **?** unspecified.

### 6.1 Virtio transport

| ID | Requirement | Tier | Verif. | Prior Art |
|---|---|---|---|---|
| VIO-01 | Driver discovers 9P device on PCI bus (vendor 0x1AF4, device 0x1009, or 0x1049 modern-only). | S | T | B |
| VIO-02 | Driver uses modern PCI interface defined by device capabilities. | S | T | B |
| VIO-03 | Driver supports virtio-mmio, versions 1 and 2 (ARM64 `virt`, x86 `microvm`). | C | T | A, B |
| VIO-04 | Driver receives interrupts via QNX 8.0 APIs, using MSI-X with legacy INTx fallback. | S | T | B |
| VIO-05 | Driver reads mount tag from device config and selects device by tag. | S | T | A |
| VIO-06 | Multiple shares can be mounted simultaneously, one per device. | C | T | ? |
| VIO-07 | Request buffers are passed to QEMU zero-copy within the transport layer. | C | I | ? |
| VIO-08 | Indirect descriptors and notification suppression (`EVENT_IDX`) are negotiated when offered by QEMU. | C | T, M | ? |
| VIO-09 | On unmount, device is cleanly reset and no interrupt handlers remain attached. | S | T | A |
| VIO-10 | Transport layer is a documented, standalone library reusable by other virtio drivers. | C | I, D | — |

### 6.2 9P2000.L Protocol

| ID | Requirement | Tier | Verif. | Prior Art |
|---|---|---|---|---|
| P9-01 | Negotiation of 9P2000.L version and configurable maximum message size (`msize`). | S | T | A, B |
| P9-02 | Core messages: attach, walk, open, create, read, write, clunk, getattr, readdir, mkdir, unlink/remove, rename, fsync, statfs. | S | T | A, B |
| P9-03 | Extended messages: setattr, symlink, readlink, link. | C | T | A |
| P9-04 | Open flags translated to Linux values; Linux errno codes translated to QNX errno. | S | T | A |
| P9-05 | Multiple in-flight requests supported concurrently. | C | T | A |
| P9-06 | Interrupted calls (signals, timeouts) cancel their request on the server via `Tflush`. | C | T | A |
| P9-07 | Every allocated fid on the server is clunked. Zero leak after 1,000,000 opens/closes. | S | T | ? |
| P9-08 | Reads, writes, and directory listings larger than `msize` are chunked and resumed. | S | T | ? |
| P9-09 | Every response is strictly validated: size, type, tag, internal length consistency. | S | T | ? |
| P9-10 | Protocol library builds and passes tests on host PC without QNX toolchain. | S | T | ? |

### 6.3 Filesystem Behavior

| ID | Requirement | Tier | Verif. | Prior Art |
|---|---|---|---|---|
| FS-01 | Open, read, write, seek, close; support for files larger than 4 GiB. | S | T | A, B |
| FS-02 | `stat` returns size, type, permissions, owner, nanosecond timestamps, link count, and stable inode. | S | T | A |
| FS-03 | Directory reads are consistent: no missing or duplicated entries, offset resumption, entry type populated. | S | T | ? |
| FS-04 | Exclusive creation (`O_EXCL`), truncation on open (`O_TRUNC`), append mode (`O_APPEND`). | S | T | A, B |
| FS-05 | Create and remove directories, unlink files, atomic rename across directories. | S | T | A, B |
| FS-06 | Binaries execute from the share and load shared libraries located on the share. | S | T, D | A |
| FS-07 | Modify permissions (`chmod`), ownership (`chown`), timestamps (`utimensat`), size (`truncate`). | C | T | A |
| FS-08 | Symlinks: creation, reading, resolution, loop detection. Hard links. | C | T | A |
| FS-09 | Guest-side permission enforcement based on caller identity. Mount options: `ro`, `noexec`, `nosuid`. | C | T | A |
| FS-10 | `fsync` propagates to host; `df` (`statvfs`) reflects host directory free space. | C | T | A |
| FS-11 | Unlinked file remains readable while open descriptors exist. | C | T | ? |
| FS-12 | File modified on host PC is seen modified in guest on next read without remounting. | S | T | ? |
| FS-13 | Optional metadata attribute cache with bounded, user-configurable TTL. | C | T | A |
| FS-14 | Advisory byte-range file locks work between guest processes; documented that they are not forwarded to host. | C | T | — |
| FS-15 | UTF-8 filenames up to 255 bytes supported, including special characters. | C | T | ? |
| FS-16 | Byte-range locks are relayed to 9P server. | X | T | — |

### 6.4 QNX Integration

| ID | Requirement | Tier | Verif. | Prior Art |
|---|---|---|---|---|
| QNX-01 | Driver is a standard QNX resource manager managing the entire subtree below its mount point. | S | I | A, B |
| QNX-02 | Starts from initial boot image (IFS), depending only on `procnto`, libc, and PCI server. | S | T | A |
| QNX-03 | Request handling executes at the calling thread's priority, verifiable via kernel trace. | S | D | ? |
| QNX-04 | When transport queue is saturated, pending requests are submitted in priority order. | C | M | — |
| QNX-05 | A blocked client does not lock up a driver worker thread. | C | T | ? |
| QNX-06 | Driver runs unprivileged (non-root) with minimum required capabilities; security policy provided. | C | T | A |
| QNX-07 | Integrates with standard `mount` and `umount` commands. | C | D | B |
| QNX-08 | Logs to system logger (`slog2`) and exposes per-operation performance counters. | C | D | ? |
| QNX-09 | Emits kernel trace events (`TraceEvent`) on request send and receive. | C | M | — |

### 6.5 Performance

Under QEMU, wall-clock timings depend heavily on host hardware. Requirements are therefore formulated comparatively and in server round-trips.

| ID | Requirement | Tier | Verif. | Prior Art |
|---|---|---|---|---|
| PERF-01 | Server round-trip counts measured and published for common POSIX calls via QEMU traces. | S | M | ? |
| PERF-02 | Round-trip count on each POSIX call does not exceed the best existing client. | C | M | — |
| PERF-03 | Sequential read/write of 1 GiB file: at least matching best existing client; >= 50% of Linux guest on same hardware. | C | M | — |
| PERF-04 | Metadata operations: create, stat, and unlink 10,000 files; traverse 50,000-entry directory tree. Same thresholds. | C | M | — |
| PERF-05 | A 4 KiB high-priority read launched during a bulk low-priority copy takes <= 2x its idle duration. | C | M | — |
| PERF-06 | Mount operation completes in under 50 ms. | C | M | ? |
| PERF-07 | Comparative benchmark report published: this driver, Project A, Project B, `scp`, and virtiofs (extreme tier). | C | D | — |

### 6.6 Robustness

| ID | Requirement | Tier | Verif. | Prior Art |
|---|---|---|---|---|
| ROB-01 | Any server error returns an appropriate errno code to the caller, never a hang. | S | T | ? |
| ROB-02 | Abnormal death of client process during in-flight request leaves no server leak or memory leak. | C | T | ? |
| ROB-03 | Unmount under active I/O load completes cleanly without crash. | C | T | A |
| ROB-04 | File unlinked or replaced on host during guest use produces clean error codes. | C | T | ? |
| ROB-05 | Request without response is timed out after a configurable deadline. | C | T | A |
| ROB-06 | Protocol decoder does not crash on arbitrary corrupted input; verified by fuzzer with memory sanitizers. | C | T | ? |
| ROB-07 | Resilient against a maliciously buggy server (truncated responses, invalid tags, extreme delays). | X | T | — |

### 6.7 Security

| ID | Requirement | Tier | Verif. | Prior Art |
|---|---|---|---|---|
| SEC-01 | Default mount is `nosuid` (no privilege elevation from shared files). | S | T | A |
| SEC-02 | Driver accesses only its assigned virtio device resources. | C | I | ? |
| SEC-03 | Documentation explains QEMU security models and recommends unprivileged host execution. | S | I | ? |

### 6.8 Developer Experience

| ID | Requirement | Tier | Verif. | Prior Art |
|---|---|---|---|---|
| DX-01 | Host CLI tool `qshare` outputs QEMU args and generates `mkqnximage` snippet to auto-mount at boot. | S | D | — |
| DX-02 | Complete end-to-end demonstration replayable with a single command. | S | D | — |
| DX-03 | Host CLI tool `qrun` executes a binary located on the share inside the guest, returning stdout and exit code. | C | D | — |
| DX-04 | Practical examples show executing tests of a CMake project and a Rust project inside the guest. | C | D | — |
| DX-05 | User guide, architectural design guide, and tutorial: "Writing a virtio driver for QNX 8.0". | C | I | A partial |
| DX-06 | Full root filesystem hosted on share: guest boot image contains only kernel and driver. | X | D | A partial |

### 6.9 virtiofs

| ID | Requirement | Tier | Verif. | Prior Art |
|---|---|---|---|---|
| VFS-01 | Driver discovers virtiofs device (virtio ID 26), reading mount tag and request queue count. | X | T | — |
| VFS-02 | Uses hiprio queue for `FORGET` and `INTERRUPT` messages, and standard queues for all file I/O. | X | T | — |
| VFS-03 | Negotiates FUSE version (7.x) and supports lookup, getattr, setattr, open, read, write, release, readdir, mkdir, unlink, rename, symlink, readlink, link, fsync, statfs. | X | T | — |
| VFS-04 | Tracks node reference counts (`lookup_count`) and returns them cleanly via `FORGET`. | X | T | — |
| VFS-05 | Reuses Layer 3 (`rmfs`) and passes identical conformance suite as 9P client. | X | T | — |
| VFS-06 | `qshare` launches `virtiofsd` and configures shared memory in QEMU. | X | D | — |
| VFS-07 | Benchmark report includes virtiofs pitted against 9P clients. | X | M | — |

### 6.10 Reverse Sharing

| ID | Requirement | Tier | Verif. | Prior Art |
|---|---|---|---|---|
| SRV-01 | In-guest 9P server exports a QNX directory tree, mounted on host via standard Linux 9P client. | X | D | — |
| SRV-02 | Export can expose `/proc` and `/dev`, allowing host inspection of QNX guest internals. | X | D | — |

---

## 7. Validation Plan

### 7.1 Five testing levels

| Level | Environment | Scope |
|---|---|---|
| 0 — Unit | Host PC | Wire codec, translation tables, ID pools, path manipulation |
| 1 — Fuzzing | Host PC | Wire decoders subjected to randomized malformed inputs under AddressSanitizer/UBSan |
| 2 — Conformance | QEMU | Differential testing against host filesystem, content exerciser, targeted edge cases |
| 3 — Performance | QEMU | Round-trip counts per call, throughput, metadata scalability, priority inversion test |
| 4 — Robustness | QEMU | Forced kill, unmount under heavy load, host-side file mutations, faulty mock server |

### 7.2 Differential testing

The host PC directory serves as ground truth. The harness runs randomized sequences of POSIX filesystem operations inside the guest, then cross-checks the resulting tree on both sides: entry names, types, sizes, permissions, timestamps, symlink targets, and SHA-256 file contents.

- Every sequence is deterministic and reproducible via its PRNG seed.
- Any discrepancy is automatically minimized to the shortest reproducing sequence.
- The exact same harness applies to Project A, Project B, and virtiofs, ensuring unbiased comparison.

### 7.3 Content exerciser

A concurrent exerciser writes, reads, truncates, and extends files using pseudorandom patterns, validating byte-exact content on every read. Runs first with a single process, then with 16 concurrent processes on distinct files and a shared file.

### 7.4 Targeted edge cases

Covers corner cases rarely reached by random exploration: overwrite existing target on rename, unlink while descriptor is held open, concurrent exclusive creation, permission bit matrix, symlink loops, 255-byte filenames, 5 GiB files, 100,000-entry directory, max path length.

### 7.5 Benchmarks

| Workload | Metric measured |
|---|---|
| Sequential read & write of 1 GiB file | Throughput vs message size |
| 10,000 small files created, stat'd, unlinked | Metadata transaction latency |
| Traversal of 50,000-entry directory tree | Directory listing overhead |
| Extraction of large source archive + grep | Realistic mixed development workload |
| Launching 1,000 programs from share | Binary loader execution path |
| 4 KiB high-prio read during bulk low-prio copy | Real-time priority inheritance & scheduling |

### 7.6 Acceptance thresholds

| Metric | Foundation (S) | Complete (C) | Extreme (X) |
|---|---|---|---|
| Differential test operations without divergence | 100,000 | 1,000,000 (x86_64 & ARM64) | Same on virtiofs |
| Content exerciser operations | 1,000,000 | 10,000,000 (16 processes) | Same on virtiofs |
| Targeted edge cases passed | Foundation subset | All | All on virtiofs |
| Fuzzing on response decoders | — | 1 hour without defect | 24 hours |
| Leaked descriptors after 1,000,000 opens | 0 | 0 | 0 |
| Comparative report | Round-trip counts | Complete vs A and B | Including virtiofs |

---

## 8. Demonstration (Video under 3 minutes)

| Time | On Screen | Key Message |
|---|---|---|
| 0:00 – 0:20 | Current pain: compile on host, scp/rebuild image, execute in QNX | Every test cycle pays a copy tax |
| 0:20 – 0:55 | `qshare ~/project`, compile with `make` on host, instant execution in QNX; host file edit visible immediately | Zero copy, instant iteration |
| 0:55 – 1:25 | Differential conformance test running, 1,000,000 operations, zero divergence | Industrial robustness, not a toy |
| 1:25 – 2:00 | Benchmark chart: existing 9P clients vs Passerelle 9P vs virtiofs | Quantified performance justification |
| 2:00 – 2:30 | High-priority read during bulk low-priority copy; kernel trace shows inherited priority | Built for a hard real-time OS |
| 2:30 – 2:45 | Driver running unprivileged without root rights | Least privilege security |
| 2:45 – 3:00 | Repository, tutorial, explicit attribution of prior art and novel additions | Honesty and open-source ethics |

---

## 9. Deliverables and Repository Layout

```
passerelle/
├── README.md            English: overview, prior art, additions, one-command demo
├── LICENSE              Apache-2.0
├── NOTICE               Attribution of reused and referenced projects
├── virtio/              Transport: core, queues, PCI, MMIO, mock transport for PC tests
├── proto9p/             Portable 9P2000.L protocol library
├── protofuse/           Portable FUSE protocol library (virtiofs)
├── rmfs/                Common filesystem resource manager layer
├── drivers/             Driver binaries (qnx-fs-9p-pci, qnx-virtiofs)
├── host/
│   ├── qshare/          Host CLI tooling and QEMU setup
│   ├── fsproof/         Differential test harness and edge-case exerciser
│   └── bench/           Benchmark suite and report generator
├── image/               mkqnximage snippets and security policy
├── upstream/            Patches offered to existing upstream projects
├── docs/                Guides, architectural specifications, verification status
└── codelab/             Step-by-step tutorial
```

- **Languages**: C11 for drivers and core libraries; Python 3 for host tooling.
- **Key QNX technology highlights**: resource manager framework, message-based priority inheritance, QNX 8.0 `InterruptAttachThread`, capabilities and security policies, PCI server integration, kernel event tracing (`TraceEvent`).
- **Tutorial bonus target**: "Writing a virtio driver for QNX 8.0: from PCI discovery to first byte read".

---

## 10. Schedule

Five weeks remain until the hackathon deadline.

| Week | Dates | Content | Milestone |
|---|---|---|---|
| 1 | 11 – 17 Nov | Feasibility tests E1 to E5. Benchmark clients A and B. PCI transport: 9P version negotiation completes on x86_64. Questions asked on Discord and to author of A. | Decisions D1 to D3 settled |
| 2 | 18 – 24 Nov | 9P read, write, execution on x86_64 (via patch to A or new repo). `qshare`. Differential test v1. | **Foundation achieved**: compile on host, run in QNX |
| 3 | 25 Nov – 1 Dec | Portable FUSE protocol + read-only virtiofs driver. Content exerciser, edge cases. | virtiofs reads a file |
| 4 | 2 – 8 Dec | virtiofs write support. Priority scheduling. Comparative benchmarks. ARM64 support. Least privilege. | Full comparative report |
| 5 | 9 – 14 Dec | README polish, tutorial, video recording. Submit on 14 Dec evening. | Submitted with 1 day margin |

**Feasibility tests (first 4 days):**

| Test | Question | Fallback if negative |
|---|---|---|
| E1 | Under QEMU x86_64, does a QNX driver successfully receive the virtio PCI interrupt and exchange a 9P message? | Adopt Project B's MSI-X setup; fall back to polling temporarily |
| E2 | Do clients A and B run as claimed, and what are their actual performance numbers? | Discovered gaps become compelling value propositions |
| E3 | Does QNX execute a binary served by an out-of-tree resource manager, including shared libraries? | Study Project A's ARM64 solution; fall back to data sharing only |
| E4 | Does `mkqnximage` allow injecting arbitrary extra QEMU arguments? | `qshare` invokes QEMU directly on the generated image |
| E5 | On the host PC, do `virtiofsd` and shared memory work with a standard Linux guest? | Drop virtiofs; redirect effort to 9P depth and benchmarks |

**First items to de-scope if schedule slips**: reverse sharing, rootfs on share, faulty server testing, ARM64 for virtiofs. The foundation tier and conformance harness are never dropped.

---

## 11. Risks and Mitigations

| Risk | Severity | Mitigation |
|---|---|---|
| Project perceived as duplicate of existing clients | High | Clear positioning (Section 3); virtiofs and benchmarks as core novelty; explicit attribution |
| "Low-Level Software" category rejected due to DDK scope | High | Question submitted Week 1; fallback to "Developer Tools" |
| Unannounced QNX virtiofs driver already exists | Medium | Inquiry on hackathon Discord during Week 1 |
| PCI interrupt difficult under QEMU | Medium | Feasibility test E1; Project B's proven MSI-X approach documented |
| Execution from share fails on x86_64 | Medium | Feasibility test E3; Project A achieved it on ARM64 |
| virtiofs exceeds available time | High | Week 3 milestone "reads a file"; Foundation 9P is independently deliverable |
| Maintainer of Project A does not reply | Low | Fallback plan defined in Section 3.1 |
| QEMU wall-clock timings disputed | Medium | Publish round-trip counts; relative comparisons on identical hardware |
| Data cache hides host modifications | Medium | No data cache by default; requirement FS-12 validated |

---

## 12. Pushing the Idea Further

Beyond the Complete tier, ordered by impact:

1. **virtiofs** (6.9): the primary innovation.
2. **Root filesystem on share**: image contains only kernel and driver; entire OS lives on host PC. Modifying a system daemon requires no image rebuild.
3. **Reverse sharing** (6.10): inspect guest `/proc` and `/dev` from host via Linux 9P mount.
4. **Fault-injection server**: scripted 9P server simulating broken responses.
5. **Network transport** for 9P client, enabling physical boards (Raspberry Pi 5).
6. **File change notifications**: notify guest applications when host files are modified.
7. **POSIX conformance test suite port** (e.g. pjdfstest), complementing custom harness.
8. **Formal protocol model**: state machine of file descriptors verified via model checking.
9. **VS Code QNX extension integration**: launch and debug from share with one click.
10. **QNX ecosystem package recipe** submitted to official ports repository.

---

## 13. Acceptance Criteria

**Foundation (S) — Minimum viable submission.**

- [ ] On an `mkqnximage` x86_64 image, one command mounts a host PC directory.
- [ ] A binary compiled on host executes in QNX without copying.
- [ ] A file modified on host is seen modified in guest.
- [ ] 100,000 differential test operations pass without divergence.
- [ ] Round-trip counts per POSIX call published.
- [ ] Public repo, license, English README citing prior art, video under 3 min.

**Complete (C) — Target hackathon version.**

- [ ] All Tier C requirements verified on x86_64 and ARM64.
- [ ] 1,000,000 differential operations; 16-process content exerciser.
- [ ] Comparative benchmark report vs Project A and Project B.
- [ ] High-priority read benchmarked during bulk low-priority copy.
- [ ] Unprivileged non-root driver; published tutorial.

**Extreme (X).**

- [ ] virtiofs passes the exact same conformance harness and is benchmarked.
- [ ] At least one additional item from Section 12.

---

## 14. Verification Status of Claims

| Claim | Status |
|---|---|
| Existence and advertised features of clients A and B | Read in their documentation. **Not yet executed.** |
| Content of `kodebooth/qnx-virtio` | File listing inspected; code not yet analyzed |
| QNX DDK does not cover virtio or filesystems | Verified on official DDK documentation page |
| QEMU 9P options, security models, message size and cache warnings | Verified on official QEMU documentation |
| macOS host supported since QEMU 7.0; Windows not integrated | Verified on official QEMU documentation |
| 9P2000.L message formats; Linux open flag and errno values | Verified against diod protocol documentation |
| 9P device configuration (mount tag preceded by length) | Verified against Linux virtio_9p header |
| Modern virtio PCI IDs (0x1049, 0x105A) | Inferred from virtio 1.2 specification rule, not yet cross-checked in QEMU source |
| virtiofs: device ID 26, 36-byte tag, hiprio queue + request queues | From secondary sources; virtio specification section to re-read |
| virtiofs host setup instructions | Verified on virtiofs project documentation |
| `InterruptAttach()` removed in QNX 8.0 | Verified in QNX 8.0 migration guide |
| No public QNX guest virtiofs driver exists | GitHub searches yielded zero hits. **Absence unproven**: ask on Discord |
| Symlink resolution by redirect; binary execution from resource manager | **Hypotheses**, subject to dedicated tests |
| Arbitrary QEMU options via `mkqnximage` | Not found in documentation; subject to test E4 |
| Message size ceiling imposed by QEMU virtqueue | Recalled from memory; to be empirically measured |

---

## 15. Glossary

The table below lists the terms the specification needs in passing. The
complete glossary of the project — every term of the code as well, with its
French equivalent and the document or source file it comes from — is in
[glossary.md](glossary.md).

| Term | Meaning |
|---|---|
| 9P, 9P2000.L | Remote filesystem protocol originally from Plan 9; Linux-adapted variant |
| virtiofs | Virtual machine shared filesystem transporting FUSE protocol over virtio |
| FUSE | Linux kernel-to-userspace filesystem protocol |
| virtio | Standard for virtual devices using shared-memory descriptor rings (virtqueues) |
| PCI, MMIO | Bus attachment methods: PCI bus (x86) or fixed memory addresses (ARM) |
| MSI-X | Message Signaled Interrupts extended mechanism for PCI devices |
| Mount tag | Identifier string configured on QEMU command line and matched by guest mount |
| Message size (`msize`) | Maximum byte length of a 9P transaction, bounding single-round-trip transfer |
| Round-trip | A single protocol request sent to the server and its corresponding response |
| Resource manager | QNX userspace process implementing POSIX namespace operations via messages |
| Abilities / Capabilities | Fine-grained privileges retained by a QNX process after dropping root |
| Differential testing | Conformance testing comparing guest filesystem behavior against host reference |
| `mkqnximage` | QNX utility that builds and launches virtual machine disk images |

---

## 16. References

**Existing projects**

- https://github.com/gvergine/qnx-fs-9p
- https://github.com/eclipse-score/qnx_unit_tests (folder `src/common/virtio9p`)
- https://github.com/kodebooth/qnx-virtio
- QNX Hypervisor, host-side virtio-fs device: https://qnx.com/developers/docs/7.1/com.qnx.doc.qavf.overview/topic/virtfs_arch.html

**9P and QEMU**

- QEMU 9P setup: https://wiki.qemu.org/Documentation/9psetup
- QEMU 9P developer documentation: https://wiki.qemu.org/Documentation/9p
- 9P2000.L protocol description (diod project): https://github.com/chaos/diod/blob/master/protocol.md

**virtiofs**

- virtio-fs QEMU how-to: https://virtio-fs.gitlab.io/howto-qemu.html

**QNX**

- Driver Development Kit (DDK): https://www.qnx.com/developers/docs/qnxeverywhere/com.qnx.doc.ddk/topic/about.html
- Filesystem resource managers: https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.resmgr/topic/fsystems.html
- Returning directory entries: https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.resmgr/topic/fsystems_ReturnEntries.html
- QNX 8.0 migration guide, kernel & interrupts: https://qnx.com/developers/docs/8.0/com.qnx.doc.qnxsdp.migration/topic/kernel.html
- `InterruptAttachThread()`: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/interruptattachthread.html
- `mkqnximage`: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkqnximage.html

**Hackathon**

- Home page and rules: https://qnx.devpost.com/ and https://qnx.devpost.com/rules

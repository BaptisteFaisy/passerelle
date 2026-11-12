<!--
SPDX-License-Identifier: Apache-2.0
-->

# Devpost submission text — Passerelle

Everything between the field markers is copy-pasteable into the Devpost form.
Numbers are only the measured ones; each is traceable to a file in
[`docs/measurements/`](../measurements/), [`docs/verification-status.md`](../verification-status.md)
or [`docs/traceability.md`](../traceability.md). Nothing here promises work that
is not in the repository.

---

## Project title

**Passerelle — a shared host directory for QNX 8.0 guests under QEMU (9P2000.L and virtiofs)**

## One-sentence summary

Passerelle mounts a directory of your PC inside a QNX virtual machine — over
9P2000.L *and* over virtiofs — so a program cross-compiled on the host runs in
the guest with no copy, no image rebuild and no cache to invalidate.

## Category

**Developer Tools** (the QNX Driver Development Kit that defines *Low-Level
Software* covers serial, I2C, SPI, HID, touch, network and CAN — a filesystem
over virtio uses none of them; see [`docs/adr/`](../adr/) and
[`docs/outreach/d1-category-question.md`](../outreach/d1-category-question.md)).

Bonus tracks claimed: **Best Use of QNX Technologies** (resource manager,
priority inheritance, `InterruptAttachThread()`, PCI server and MSI-X, abilities
and security policy, kernel trace events) and **Best Tutorial**
([`codelab/`](../../codelab/README.md), eight chapters, code included).

## Links

| Field | Value |
|---|---|
| Repository | https://github.com/Tradoooo/passerelle (public, Apache-2.0) |
| Video | **TODO — to be filled at submission** (script and shot list: [`docs/video-script.md`](../video-script.md), target 2:52, ceiling 3:00) |
| Screenshot | **TODO** (the E5 console with both protocols passing) |
| Traceability matrix | [`docs/traceability.md`](../traceability.md) — generated, 77 requirements |
| Requirements | [`docs/spec-en.md`](../spec-en.md) (English), [`docs/spec-fr.md`](../spec-fr.md) (French original) |

## What it does

Testing a QNX program today means paying a copy tax on every iteration:
cross-compile, then `scp` the binary into the guest or rebuild the disk image,
then run. Passerelle removes the step:

```
Host    $ qshare ~/project          # prints or injects the QEMU options
Guest   # ls /host                  # the host directory is there
Host    $ make                      # compile on the host
Guest   # /host/build/my_program    # runs immediately, zero copy
```

A file edited on the host is seen on the guest's next `read()`, with no remount:
there is no data cache by default. The same transport serves two protocols —
9P2000.L (built into QEMU) and virtiofs (FUSE over virtio, with `virtiofsd`) —
and requests are submitted in **priority order**, so a control-loop read does not
queue behind a bulk copy.

## Why this is new (prior art, named — spec §3.3)

Two public 9P clients for QNX exist, and one virtio transport library. Both
clients are cited in the README, in [`NOTICE`](../../NOTICE) and in
[`docs/state-of-the-art.md`](../state-of-the-art.md); neither is dismissed:

| Project | What it does | What it does not do |
|---|---|---|
| [`gvergine/qnx-fs-9p`](https://github.com/gvergine/qnx-fs-9p) (Oct 2026, Apache-2.0) | a complete, everyday 9P2000.L client: symlinks, permissions, timestamps, execution from the share | **ARM64 only**, over virtio-mmio; x86_64 is stated as unsupported and never tested, so it cannot drive the PCI device of an x86 image |
| [`eclipse-score/qnx_unit_tests`](https://github.com/eclipse-score/qnx_unit_tests) (`virtio9p`, Mar 2026, Apache-2.0) | 9P on x86_64 (PCI, MSI-X with fallback) and ARM64 | reduced to test-infrastructure needs: no symlinks, no `chmod`/`chown`, no timestamps, no locks |
| [`kodebooth/qnx-virtio`](https://github.com/kodebooth/qnx-virtio) (Apr 2026, MIT) | a real virtio transport for QNX 8 with a GPIO driver on top | no filesystem; modern-only PCI ids; cannot be built or unit-tested without the SDP (evaluated line by line, see [ADR-003](../adr/ADR-003-write-our-own-virtio-transport.md)) |

What Passerelle adds, and nobody offers today:

1. **A complete share on x86_64** — the default architecture of `mkqnximage`
   images — with symlinks, permissions, ownership, timestamps, locks and
   directory resumption.
2. **virtiofs for a QNX guest.** FUSE over virtio is the current standard for
   Linux guests and is substantially faster than 9P; no public QNX guest
   implementation was found. (This is a *search result, not a proof* — spec §14.)
3. **A differential conformance and benchmark testbed** that treats the host
   directory as the ground truth and can be pointed at any client, so the
   comparison is apples to apples — and a published round-trip baseline.
4. **Real-time behaviour**: priority-ordered submission, one conversion point
   from QNX priority to scheduler urgency ([ADR-002](../adr/ADR-002-qnx-priority-conversion.md)),
   and a blocked client that never holds a driver thread.
5. **Everything testable without the SDP**: layers 1 to 3 are portable C11, so
   they build, run and are fuzzed on a Linux PC, in CI, in under five minutes.

## Architecture — three layers

```
  QNX programs:  open  read  write  stat  readdir  exec ...
        |  QNX messages, handled at the caller's priority
  +-----v----------------------------------------------------------+
  | Layer 3 - rmfs: paths, descriptors, permissions, attr cache,   |
  |           guest-local locks, priority scheduler                |
  +------------------------------+---------------------------------+
  | Layer 2 - 9P2000.L (proto9p) | Layer 2 - FUSE (protofuse)      |
  +------------------------------+---------------------------------+
  | Layer 1 - virtio: split virtqueues, PCI modern + transitional, |
  |           MMIO v1/v2, mock backend for host tests              |
  +-----+----------------------------------------------------------+
        |
   QEMU built-in 9P server   or   virtiofsd on the host PC
        |
   a directory of the host PC
```

Layer 1 is written once and serves both protocols; layer 2 is portable C with no
OS header; layer 3 is common, so the same conformance suite applies to 9P and to
virtiofs. Detail in [`docs/architecture.md`](../architecture.md).

## What is demonstrated, and how to reproduce it

Every row is a command in this repository and a recorded result. Rows marked
*blocked* are stated as blocked, not as done.

| # | Claim | Command | Measured result |
|---|---|---|---|
| 1 | The portable stack builds and its tests pass on a Linux PC, no SDP | `cmake -B build && cmake --build build -j8 && ctest --test-dir build --output-on-failure` | green, and green again under ASan+UBSan (`-DPGW_SANITIZERS=ON`) |
| 2 | Differential conformance against the host directory, seeded and replayable | `PGW_DIFF_OPS=1000000 build/tests/test_diff` | **3 000 000 operations** (3 seeds × 1 000 000), **0 divergence**, 60 s, 11 Nov 2026 |
| 3 | Decoders survive arbitrary garbage | `PGW_FUZZ_ITERS=5000000 build/tests/fuzz_p9` (and `fuzz_fuse`) | **5 000 000 iterations per family**, 11 s and 10 s; repeated under sanitizers (21 s, 18 s), **0 defect** |
| 4 | Host side of both protocols, end to end, with a real guest (E5) | `python3 tests/qemu/run_share_check.py --protos 9p,virtiofs` | both **PASS**: the guest mounts, reads what the PC wrote, writes what the PC reads back — 9.0 s per protocol on the dev PC; in CI on `ubuntu-24.04`, virtiofs **7.2 s**, 9P **6.7 s** |
| 5 | A real QNX guest sees the device | `tests/qemu/qnx71/qnx-vm.sh` then `pci-tool -vv` | QNX Neutrino **7.1.0** reports `1af4/1009`, the MSI-X capability plus the five virtio vendor capabilities, three assigned BARs — and **no interrupt routed**, which is a finding, not a guess ([EVIDENCE.md](../../tests/qemu/qnx71/EVIDENCE.md)) |
| 6 | Round trips per POSIX call, published | `python3 -m bench roundtrips <trace>` | **1 643 trace events → 822 requests**, 1.00 round trip per call ([linux-baseline-9p.md](../measurements/linux-baseline-9p.md)) |
| 7 | Every requirement has a state and a proof | `python3 tools/traceability.py --check` | **77 requirements: 50 tested, 21 partial, 6 not started** — the matrix is generated, so it cannot rot |

**Blocked, and said so:** building or running the QNX driver itself. There is no
QNX SDP on the machine that produced this tree and no public SDP 8.0 image could
be pulled without an account, so feasibility tests E1 (driver half), E2, E3 and
E4 and the performance numbers PERF-02 … PERF-07 have no measurement. The QNX
sources exist and are compile-checked against stub headers in CI (`qnx-syntax`),
which proves they parse and type-check, **not** that they run.

## Limits we assume

- **No QNX-side requirement is claimed as verified.** 21 requirements are
  partial and 6 are not started, all of them for the same reason: they need a
  guest built with the SDP.
- The QNX guest we could boot is **7.1, not 8.0**, and ships no compiler: it
  answers device questions (enumeration, capabilities, BARs, interrupt routing),
  not driver questions.
- **No data cache by default** (decision D4): correctness of the development loop
  first, synthetic metadata numbers second. An attribute cache exists, off by
  default, bounded and TTL-configurable.
- Out of scope, by design: other hypervisors, network sharing between physical
  boards (QNX has NFS and CIFS), extended attributes (QNX has no interface for
  them), `mmap(MAP_SHARED)` of a file on the share, virtiofs DAX.
- Advisory byte-range locks are **guest-local**; relaying them to the server is
  FS-16, extreme tier, not started.

## Licence and attribution

Apache-2.0 ([`LICENSE`](../../LICENSE)), with third-party attributions in
[`NOTICE`](../../NOTICE). No vendored third-party code: `kodebooth/qnx-virtio`
was evaluated and not reused ([ADR-003](../adr/ADR-003-write-our-own-virtio-transport.md)),
and the patches we offer to `gvergine/qnx-fs-9p` live in
[`upstream/`](../../upstream/) under their original licence.

## What is left to do

In order, and all of it in [`docs/gap-analysis.md`](../gap-analysis.md):

1. **Install the QNX SDP 8.0** — the single blocking item; it unblocks E2, E3,
   E4 and the driver half of E1.
2. Build one `mkqnximage` x86_64 image with the driver in it, and answer E1:
   does the driver get the interrupt and exchange a first 9P message? The QNX
   7.1 measurement already tells us the image routes no interrupt to this device,
   so the driver must add its own INTx entry or take the MSI-X path.
3. Run the differential harness **in the guest** (`host/fsproof`) and execute a
   binary from the share (FS-06, the point of the project).
4. Fill the three missing measurement tables: our driver's round trips, 1 GiB
   throughput, and the priority test under a bulk copy.
5. Record the video (script ready) and take the screenshot.
6. If the schedule slips, de-scope in this order: reverse sharing (SRV-01/02),
   root filesystem on the share (DX-06), the faulty-server bench (ROB-07), the
   virtiofs comparative bench (VFS-07). The foundation tier and the conformance
   harness are never dropped.

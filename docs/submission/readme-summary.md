<!--
SPDX-License-Identifier: Apache-2.0
-->

# README summary — the 60-second block for the top of the repository

Text to paste at the head of `README.md`, below the title. Four movements: the
problem, the solution, the proof, one command. Every number is measured and
dated in [`docs/verification-status.md`](../verification-status.md) or
[`docs/measurements/`](../measurements/); the requirement states come from the
generated [`docs/traceability.md`](../traceability.md).

---

## Passerelle in 60 seconds

**The problem.** Testing a QNX program under QEMU means paying a copy tax on
every iteration: cross-compile on the host, then `scp` the binary into the guest
or rebuild the disk image, then run. A minimal or read-only image makes it
worse — one changed line costs a whole image.

**The solution.** Passerelle mounts a directory of the host PC inside the QNX
guest, over **9P2000.L** and over **virtiofs**, through one portable virtio
transport (PCI modern and transitional, MMIO v1/v2). Compile on the host, run in
the guest, zero copy. A file changed on the host is seen on the guest's next
`read()` — there is no data cache by default, so nothing to invalidate. Requests
are submitted in priority order, because the guest is a real-time kernel.

Three layers, one of them shared by both protocols:

```
Layer 3  rmfs       paths, descriptors, permissions, locks, priority scheduler
Layer 2  proto9p    9P2000.L codec + session   |   protofuse   FUSE codec + session
Layer 1  virtio     split virtqueues, PCI, MMIO, and a mock backend for host tests
```

**The proof.** Layers 1 to 3 are portable C11 with no OS header, so they are
built, tested and fuzzed on a Linux PC — no QNX SDP needed:

| Measured | Result |
|---|---|
| Differential conformance against the host directory | **3 000 000 operations** (3 seeds × 1 000 000), **0 divergence**, 60 s |
| Randomized input on every wire decoder | **5 000 000 iterations per family**, **0 defect**, also under ASan+UBSan |
| Host side of both protocols with a real guest (E5) | **PASS** for 9P and virtiofs — virtiofs in **7.2 s**, 9P in **6.7 s** in CI |
| Round trips per POSIX call, from a QEMU 8.2 trace | **822 requests**, 1.00 round trip per call (published baseline) |
| A real QNX guest looking at the device | QNX Neutrino **7.1.0** reports `1af4/1009`, MSI-X plus five virtio capabilities, three BARs |
| Requirements | **77** in the spec: **50 tested, 21 partial, 6 not started** — a generated matrix, never a hand-written claim |

**What is not proven, said plainly.** The QNX driver itself has never run: there
is no SDP on the machine that produced this tree. The QNX sources are
compile-checked against stub headers in CI, which proves they type-check, not
that they work. Feasibility tests E1 (driver half), E2, E3, E4 and the
performance numbers PERF-02 … PERF-07 wait on that one install.

**One command.**

```bash
python3 tests/qemu/run_share_check.py --protos 9p,virtiofs
```

It boots a minimal guest on the exact options `qshare` emits, mounts the share
for both protocols, and checks the two directions — a file written by the PC is
read by the guest, a file written by the guest is read back by the PC. Exit
status `0` when everything passed, `1` on a failed check, `2` when a prerequisite
is missing (an explicit skip, never a silent green).

Build and test the stack itself:

```bash
cmake -B build && cmake --build build -j8 && ctest --test-dir build --output-on-failure
(cd host && python3 -m pytest tests -q)
python3 tools/traceability.py --check      # the requirement matrix is up to date
```

Prior art is named, compared and attributed — see
[`docs/state-of-the-art.md`](../state-of-the-art.md) and [`NOTICE`](../../NOTICE).

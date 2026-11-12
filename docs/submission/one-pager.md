<!--
SPDX-License-Identifier: Apache-2.0
-->

# Passerelle — one page

**A shared host directory for QNX 8.0 guests under QEMU, over 9P2000.L *and* virtiofs.**
QNX Global Hackathon 2026 · category *Developer Tools* · Apache-2.0 ·
https://github.com/Tradoooo/passerelle

---

## The problem, in one line

Every test cycle of a QNX program pays a copy tax: cross-compile, then `scp`
into the guest or rebuild the disk image, then run.

## The answer, in one line

`qshare ~/project` — the host directory is mounted in the guest, the binary runs
from the share, and a file edited on the host is seen on the next `read()`.

---

## The numbers

All measured, all reproducible with a command in the repository. Nothing here is
an estimate or a target.

| | |
|---|---|
| **3 000 000** | differential operations against the host directory (3 seeds × 1 000 000) — **0 divergence**, in 60 s |
| **5 000 000** | randomized-input iterations **per decoder family** (9P and FUSE) — **0 defect**, in 11 s and 10 s; repeated under ASan+UBSan in 21 s and 18 s |
| **7.2 s** | for feasibility test **E5** to boot a guest, mount the share through `virtiofsd` and exchange files both ways (9P: **6.7 s**), on a CI runner; 9.0 s each on the dev PC |
| **822** | 9P requests counted in a real QEMU 8.2 trace (**1 643 events**, 63 errors) — the published round-trip baseline: **1.00 round trip per POSIX call** |
| **77 / 50 / 21 / 6** | requirements in the spec / tested / partial / not started — a **generated** matrix, `python3 tools/traceability.py --check` |
| **1af4:1009** | what a **real QNX Neutrino 7.1 guest** reports for our device, with the MSI-X capability, the five virtio vendor capabilities and three assigned BARs |
| **5 min** | the ceiling of the whole CI verdict — every job declares `timeout-minutes: 5`, guarded by `ci-budget`, on self-hosted runners |
| **0** | QNX-side requirements claimed as verified, because no SDP was available on the machine that built this tree |

## Three layers, one transport

```
Layer 3  rmfs       paths · descriptors · permissions · locks · priority scheduler
Layer 2  proto9p    9P2000.L codec + session     protofuse   FUSE codec + session
Layer 1  virtio     split virtqueues · PCI modern + transitional · MMIO v1/v2 · mock
                        |                                  |
                  QEMU built-in 9P server            virtiofsd on the host
                        \________________  ________________/
                                     a directory of the host PC
```

Layers 1 to 3 are portable C11 with **no OS header**: they build, run and are
fuzzed on a Linux PC, in CI, without a QNX SDP. Only `*_qnx.c` files touch QNX,
and they are compile-checked against stub headers on every pull request.

## What is new

| Existing | Gap it leaves | Passerelle |
|---|---|---|
| `gvergine/qnx-fs-9p` — complete 9P client | **ARM64 only**, virtio-mmio | complete share on **x86_64**, PCI modern *and* transitional |
| `eclipse-score/qnx_unit_tests` — 9P on x86_64 | test infrastructure only: no symlinks, no `chmod`/`chown`, no timestamps, no locks | symlinks with redirect resolution, permissions, ownership, timestamps, locks, directory resumption |
| `kodebooth/qnx-virtio` — virtio transport | no filesystem, modern-only ids, unbuildable without the SDP | our own transport, evaluated line by line ([ADR-003](../adr/ADR-003-write-our-own-virtio-transport.md)) |
| — | **virtiofs for a QNX guest**: no public implementation found | FUSE-over-virtio codec, session, node refcounts, adapter onto the same layer 3 |
| — | no public numbers to choose a client | differential + benchmark testbed that can be pointed at any client |
| — | FIFO submission | **priority-ordered** submission, one conversion point from QNX priority ([ADR-002](../adr/ADR-002-qnx-priority-conversion.md)) |

## Reproduce it

```bash
python3 tests/qemu/run_share_check.py --protos 9p,virtiofs   # E5, both protocols
cmake -B build && cmake --build build -j8 && ctest --test-dir build --output-on-failure
PGW_DIFF_OPS=1000000 build/tests/test_diff                   # 1 000 000 operations, one seed
python3 tools/traceability.py --check                        # the matrix is up to date
```

## The honest blocker

There is **no QNX SDP** on the machine that produced this tree, and no public
SDP 8.0 could be pulled without an account. So the driver has never run:
feasibility tests E1 (driver half), E2, E3 and E4 are blocked, PERF-02 … PERF-07
have no number, and 21 requirements stay partial for that single reason. What we
did instead of guessing: prove everything that can be proven on the host, and
boot a real QNX 7.1 guest to ask the device what it looks like from the QNX side
— including the finding that **no interrupt is routed to it** in the reference
image, so the driver must add its own INTx entry or take the MSI-X path.

Failures and dead ends are written down:
[`what-did-not-work.md`](what-did-not-work.md).

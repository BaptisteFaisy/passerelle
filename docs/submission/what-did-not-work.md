<!--
SPDX-License-Identifier: Apache-2.0
-->

# What did not work

Five things cost real time, and four of them changed a design or a process. They
are written down because a submission that only lists what worked is not a
submission anybody can build on. Each entry says what was tried, what actually
happened, what it cost, and where the fix lives.

---

## 1. The QNX SDP could not be obtained without an account

**Tried.** Install QNX SDP 8.0 (QNX Everywhere) to cross-compile the driver with
`qcc` and to build a guest image with `mkqnximage`, as the specification assumes
(§4.3). Every route was checked on the development machine: no `qcc`, no
`mkqnximage`, no `~/qnx800`, and no public download that does not require a myQNX
account. The community image that is known to ship an SDP,
`ghcr.io/kodebooth/qnx-sdp`, is **private**.

**What happened.** Nothing to install. The machine has QEMU 8.2.2 and
`virtiofsd`, so the *host* half of the project is fully exercisable, and the
*guest* half is not.

**What it cost.** This is the single blocking item of the project: feasibility
tests **E1 (driver half), E2, E3 and E4** cannot be run; **PERF-02 … PERF-07**
have no measurement; **21 requirements stay partial** and the QNX driver sources
have never executed. It also means the demonstration video cannot show a QNX
guest mounting the share, which is the shot the whole project was designed
around.

**What we did instead.** Rather than assert that the driver works:

- layers 1 to 3 were made portable C11 with no OS header, so they are built,
  tested and fuzzed on the PC in CI (P9-10 is a requirement, and it is now the
  reason the project has any proof at all);
- a **mock virtio backend** (`virtio/src/mock.c`) replaced the missing device, so
  the transport is tested without QNX;
- the QNX-only files are compile-checked against stub headers
  (`tests/qnx-stub/include`, CI job `qnx-syntax`) — which proves they parse and
  type-check, and is labelled as such everywhere;
- a **real QNX 7.1 guest** was booted to ask the device what it looks like from
  the QNX side (next entry).

Recorded in [`docs/verification-status.md`](../verification-status.md) §3 and
§4, and in [`docs/gap-analysis.md`](../gap-analysis.md) (every gap marked `[S]`).

## 2. The QNX 7.1 image we could get ships no compiler — and routes no interrupt

**Tried.** Use the community QNX Neutrino **7.1.0** x86_64 image (2022-07-12) as
the guest: build our driver into it, mount the share, run a binary from it.

**What happened.** The image boots fine under QEMU 8.2.2 and its own tools
answer, but it is a *runtime* image: **no `qcc`, no `mkifs`**. It cannot build
our driver and nothing can be compiled for it. It is also 7.1, not 8.0, so the
kernel changes the design relies on (`InterruptAttach()` removed, the capability
model) are not exercised by it.

One more thing came out of it, and it is a finding rather than a failure:
`pci-tool -vv` reports our device as `1af4/1009` with the MSI-X capability, the
five virtio vendor capabilities and three assigned BARs — **and
`Interrupt list - 0 assigned`**. The image's `pci_hw.cfg` routes INTx only for
other bus/device addresses, and `pci_server.cfg` sets `DO_BUS_CONFIG=no`, so the
BARs are assigned by QEMU firmware, not by the PCI server. A driver must
therefore add its own INTx entry (bus, device, pin A) or take the MSI-X path, for
which the image does ship `pci_cap-0x11.so`.

**What it cost.** E1 stays half answered; FS-06 (execute a binary from the
share), QNX-02, QNX-06 and QNX-07 cannot be demonstrated.

**What we kept.** The observation is now evidence instead of an assumption:
[`tests/qemu/qnx71/EVIDENCE.md`](../../tests/qemu/qnx71/EVIDENCE.md), with the
transcript copied from the guest, and the interrupt-routing consequence is
written into the driver design and into
[ADR-003](../adr/ADR-003-write-our-own-virtio-transport.md)'s PCI id discussion.
It also cross-checks our device-id constants against a real QEMU rather than
against a datasheet (VIO-01, VFS-01).

## 3. The `lookup` / `getattr` symlink convention was inverted

**Tried.** Implement the backend interface with `lookup` and `getattr` as two
attribute calls, without writing down which one follows a final symbolic link.

**What happened.** `rmfs` assumed one convention, the POSIX truth backend and the
9P adapter assumed the other. The observable symptom was inverted: `lstat` on a
link loop answered **`ELOOP`** while `stat` on the same path answered **`OK`**.
Worse, because the walk never *saw* a link, it never produced the redirect answer
QNX needs, and **FS-08 (loop detection) was unreachable** — every `lstat` on a
symlink was wrong.

**What it cost.** A conformance defect that no amount of reading would have
found: it was the differential bench (`tests/e2e`) that produced it, on a
randomly generated symlink loop.

**Fix.** The convention is now written where both sides read it, in
[`rmfs/include/pgw/rmfs.h`](../../rmfs/include/pgw/rmfs.h): `lookup` = `lstat(2)`
semantics, never follows the final link; `getattr` = `stat(2)` semantics, follows
it and may legitimately return `ELOOP` or `ENOENT`. An adapter whose protocol
cannot distinguish the two may implement both identically, because `rmfs`
resolves links itself through `readlink`. Pinned by `test_rmfs_path`
(`test_symlink_yields_redirect`, `test_symlink_loop_detected`,
`test_open_nofollow_refuses_symlink`). Full record:
[ADR-001](../adr/ADR-001-lookup-getattr-symlink-convention.md).

## 4. `strict: true` on branch protection is incompatible with the Mergify queue

**Tried.** Protect `main` the conventional way: required status checks **and**
"Require branches to be up to date before merging" (`required_status_checks.strict:
true`).

**What happened.** Measured on pull request **#13**: with every check green, the
Mergify queue **refused to queue anything** and posted a failing
`Mergify Merge Queue` check reading *"Configuration not compatible with a branch
protection setting"*. The mechanism is structural — the queue tests a PR through
a draft built on the latest base and then merges the *original* branch, which
GitHub by then considers out of date. Two extra traps on the same afternoon: the
protection endpoint is `PUT` (a `PATCH` answers 404), and all four keys must be
supplied or GitHub answers 422. And `queue_rules` alone does nothing: a
`pull_request_rules` entry with a `queue` action is required, plus the Merge
Queue product activated for the repository.

**What it cost.** Roughly a day of CI archaeology, and a merge pipeline that
silently did nothing while looking configured.

**Fix.** `strict: false`, every required context kept, and the reasoning written
down with the exact `gh api` call:
[ADR-005](../adr/ADR-005-mergify-queue-strict-false.md) and
[`docs/ci.md`](../ci.md). Losing `strict` costs nothing here because the queue
re-tests every PR on the latest base together with the ones ahead of it, and
`hermes-lite` watches freshness separately (in dry-run: it alerts, it never
merges). If `strict` ever has to come back, the queue must switch to
`merge_method: fast-forward` or to serial in-place checks.

## 5. `-trace file=` writes a binary trace, not text

**Tried.** Record the QEMU 9P trace for the round-trip baseline with
`-trace 'enable=v9fs_*' -trace file=baseline.log`, then parse `baseline.log` as
text — the whole benchmark (`host/bench/trace.py`) counts one line per event.

**What happened.** The file was **simpletrace binary**, not text: the parser
found no `v9fs_*` line at all. On Ubuntu's QEMU 8.2 the trace events go through
the `log` backend, which writes plain text to the file named by **`-D`**; the
`file=` option belongs to the simpletrace format.

**What it cost.** A baseline that looked recorded and contained nothing usable —
the failure was loud (`no 9P event in <file>; start QEMU with -trace
'enable=v9fs_*' -trace file=...`), but the message itself pointed at the wrong
option, which cost another round.

**Fix.** `qshare` passes both switches — `-D <path>` for the text log the parser
reads and `-trace enable=v9fs_*` to enable the events
([`host/qshare/runner.py`](../../host/qshare/runner.py), with the reason in a
comment), and the error message in [`host/bench/cli.py`](../../host/bench/cli.py)
names the working combination. A second limit is stated rather than papered over:
the text log carries **no timestamps**, so latencies are not reported at all —
round trips per call do not need them
([ADR-004](../adr/ADR-004-roundtrip-measured-baseline.md)).

---

## Smaller things that also did not work

| Attempt | Outcome |
|---|---|
| Passing the QNX priority straight through to the submission scheduler | A background copy at priority 250 **overtook** a priority-10 read: PERF-05 inverted on target. Fixed by a single conversion point, `pgw_prio_from_qnx()` — [ADR-002](../adr/ADR-002-qnx-priority-conversion.md) |
| `*.log` in `.gitignore` | Swallowed the recorded QEMU traces under `host/tests/fixtures/`, so `host-tools` failed on the runner while passing locally. Fixed with an explicit negation |
| A hard-coded include list in the `qnx-syntax` job | Failed on `pgw/drvsrv.h`, the drivers' shared service layer. The job now discovers every `include/` directory of the stack |
| Running E5 on the self-hosted R630 workers | They have neither QEMU nor `virtiofsd`: the step reports an **explicit skip** (exit 2, never a silent green), and E5 runs on a GitHub-hosted `ubuntu-24.04` runner instead |
| Direct boot on the CI runner kernel | The runner's `/boot/vmlinuz-*` is `-rw-------`, and `linux-modules-extra` is not installable there; a step chmods the kernel, and the Azure kernel already ships `netfs`, `9pnet`, `9pnet_virtio`, `9p` and `virtiofs` |
| Reading a red `Verdict` check as a veto | `hermes-lite` runs with `cancel-in-progress: true`, so a superseded run turns red with a cancellation annotation; `Verdict` is not a required check — read `hermes-lite/verdict` |
| The spec's one-hour fuzzing campaign (§7.6) | At the measured rate it would be ~10⁹ iterations, so the honest claim is **5 000 000 per family**, and the long CTest entries stay disabled so they never run on a pull request |

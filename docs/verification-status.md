# Verification Status

> **Snapshot date: 11 November 2026, 01:25 CET.** This table records the state of
> each requirement at the time of writing. Several modules are being implemented in
> parallel by other contributors, so a requirement marked *not started* below may
> already be in progress. The authoritative source is always the CI run
> (`ctest` + `size-guard` + sanitizer build) on the branch head, not this file.
> Update this file whenever a requirement changes state.

## Status vocabulary

| State | Meaning |
|---|---|
| **Implemented** | Code exists, compiles, and is exercised by an automated test |
| **Tested on PC** | Portable code with an automated host-side test, no QNX needed |
| **To validate under QNX** | Code exists but can only be proven on a QNX 8.0 guest under QEMU |
| **Not started** | Public contract (header) defined; implementation pending |

**Environment facts verified on the development PC** (`baptiste-MS-7D18`, Linux):

- QEMU **8.2.2** installed (`qemu-system-x86_64 --version`).
- `virtiofsd` present at `/usr/libexec/virtiofsd`.
- QEMU supports `virtio-9p-pci` and `vhost-user-fs-pci` (`qemu-system-x86_64 -device help`).
- **No QNX SDP installed**: `qcc` and `mkqnximage` are absent, `~/qnx800` does not exist.
  Cross-compiling for QNX is therefore impossible on this machine.
- **A real QNX guest does run here**: QNX Neutrino **7.1.0** x86_64, booted by
  QEMU with a `virtio-9p-pci` device attached (`tests/qemu/qnx71/`). It ships no
  compiler, so it cannot build our driver, but it answers what QNX sees of the
  device — see §3.2.

---

## 1. Baseline Infrastructure (module `common/`)

| Item | State | Proof |
|---|---|---|
| Byte cursors, sticky-fail, zero-copy `pgw_take` | **Implemented** | `ctest -R common` → `test_buf_roundtrip`, `test_buf_sticky_fail_and_clamp` |
| Linux↔host errno / open-flags / mode tables | **Implemented** | `ctest -R common` → `test_err_linux_table`, `test_flags_and_modes` |
| Injectable monotonic clock | **Implemented** | `ctest -R common` → `test_time_fake_clock` |
| Logging + per-operation counters | **Implemented** | `ctest -R common` → `test_log_stats_and_pattern` |
| Tunable limits in one header | **Implemented** | `common/include/pgw/limits.h` |
| Size rule (< 500 lines/file, < 50 lines/function) | **Implemented** | `ctest -R size-guard`, `python3 tools/check-size.py --root .` |

Baseline command:
```bash
cmake -B build -DPGW_MODULES=common && cmake --build build -j8
ctest --test-dir build --output-on-failure      # 2/2 tests passed
```

---

## 1.1 Conformance and randomized-input benches (`tests/`)

Added at the second snapshot. These are the levels 1 and 2 of the validation plan
(spec 7.1) running **on the PC, in the CI**, without a QNX SDP.

| Item | State | Proof |
|---|---|---|
| Truth backend over a real POSIX directory | **Implemented** | `tests/posix_backend/`, an implementation of `pgw_fs_backend_ops` |
| Differential test, seeded and replayable (spec 7.2) | **Tested on PC** | `ctest -R test_diff` — random suites through `pgw_rmfs`, compared field by field with the folder: names, types, sizes, modes, link targets, content digest |
| Automatic reduction of a divergence | **Tested on PC** | `diff_reduce()` bisects the suite and reports the shortest prefix that reproduces, with the seed |
| Directed cases (spec 7.4) | **Tested on PC** | `ctest -R test_directed` — rename over an existing file, exclusive create, truncate, append, permission matrix with a non-owner caller, symlink loop, 255- and 256-byte names, non-empty `rmdir`, 600-entry directory read in pages of 7, `fsync`, `statfs`, local range locks |
| FS-11 deleted-but-open stays readable | **Tested on PC** | `test_deleted_but_open_stays_readable` |
| FS-12 a change on the PC is seen without remounting | **Tested on PC** | `test_host_change_visible_without_remount` |
| ROB-02 / P9-07 no descriptor or fid leak | **Tested on PC** | `test_no_handle_leak_after_many_cycles` (20 000 open/close cycles, counters back to zero) |
| ROB-06 randomized input on every decoder | **Tested on PC** | `ctest -R fuzz` — noise plus five kinds of damage on well-formed messages, with a header invariant that fails the test when a decoder accepts garbage |
| Priority conversion QNX → scheduler | **Implemented** | `ctest -R prio` (`common/tests/test_prio.c`) |
| 1 000 000-operation campaign (spec 7.6) | **Executed — 0 divergence** | `PGW_DIFF_OPS=1000000 build/tests/test_diff`: 3 seeds × 1 000 000 operations = **3 000 000 operations**, 60 s, 11 Nov 2026. The CTest entry `e2e-long` stays disabled so it never runs on a PR |
| Long randomized-input campaign | **Executed — 0 defect** | `PGW_FUZZ_ITERS=5000000`: **5 000 000 iterations** on the 9P decoders (11 s) and 5 000 000 on the FUSE decoders (10 s), 11 Nov 2026; repeated under ASan+UBSan (21 s and 18 s), still zero defect. The spec's one-hour campaign (§7.6) is not run: at this rate it would be ~10⁹ iterations, and "5 million each" is the honest claim |
| Coverage-guided fuzzing | **Optional** | `-DPGW_LIBFUZZER=ON` builds `fuzz_p9_lf`; never required, the deterministic campaigns run everywhere |
| ROB-07 a deliberately faulty server must not crash the client | **Tested on PC** | `ctest -R faulty` (`tests/faulty/`): 9 message faults + 5 transport faults served by a real 9P server that breaks one rule per run; the client must answer the right error, within a 300 ms deadline, with exactly one fid left open. Under ASan/UBSan (`sanitize`) an out-of-bounds read or a leak fails the job. Found and fixed a real SIGPIPE kill on `close()` mid-frame |
| Directed case: the 5 GiB file (§7.4) | **Tested on PC** | `ctest -R test_cas_big_file` — sparse file, reads across a hole, at the boundary, past the end; the full 5 GiB variant is a `DISABLED` long test, the gate runs a scaled one. Real disk use stays in the megabytes |
| Directed case: the 100 000-entry directory (§7.4) | **Tested on PC** | `ctest -R test_cas_big_dir` — paged listing compared with the truth; exposed a silent 4 096-entry truncation in the truth backend, now fixed |
| Directed case: maximum path length (§7.4) | **Tested on PC** | `ctest -R test_cas_path_len` — PATH_MAX / NAME_MAX on both sides, at the limit and one byte over, compared with the kernel |
| Directed case: exotic names (§7.4) | **Tested on PC** | `ctest -R test_cas_exotic` — UTF-8, combining marks (NFC ≠ NFD, never normalised), emoji, spaces, newlines, quotes, a 255-byte component; the bytes must survive the whole chain. Exposed a `strstr(.., "..")` guard that refused valid names, now fixed |
| ROB-03 / PERF-05 a mount under a continuous workload | **Tested on PC** | `ctest -R test_cas_load` — concurrent metadata traffic during a bulk copy, a bounded priority read, `diff_verify_tree` at the end, no deadlock and no fd growth. PC-side only: the QNX end-to-end latency of gap 15 is still not measured |
| FS-10 / FS-13 what `fsync` proves | **Tested on PC** | `ctest -R test_cas_fsync` — propagation to the backend (counting decorator), no cache shortcut, survival of a page-cache drop and of a re-read through a fresh handle, write ordering. **Durability on a real platter is not claimed and cannot be** |
| virtiofs over a real vhost-user socket | **Tested on PC** | `python3 -m pytest host/tests/test_vhost_virtiofsd.py -q` — `host/vhost/` drives the real `virtiofsd` 1.10.0 through a memfd-backed split virtqueue: 5 checks in ~1 s, skipped with an explicit reason when the daemon is absent. No QEMU, no guest, no virtio PCI/MMIO device: transport and FUSE only |
| vhost-user front end, protocol and virtqueue | **Tested on PC** | `python3 -m pytest host/tests/test_vhost_protocol.py host/tests/test_vhost_vring.py host/tests/test_vhost_fuse_client.py -q` against a Python vhost-user backend, so the tests need no `virtiofsd` |
| One environment definition, one package | **Implemented** | `.devcontainer/devcontainer.json` validated statically by pytest; `packaging/debian/` validated against `VERSION` and the real file list; `pip install ./host` + the four console scripts are exercised by `release.yml` |

What the PC bench **cannot** prove: anything that depends on the guest kernel —
priority inheritance in the trace (QNX-03), interrupt delivery (VIO-04), running
a binary straight from the share (FS-06), `mount`/`umount` (QNX-07), the security
policy (QNX-06). Those need E1–E5.

---

## 2. Requirement Matrix

### 2.1 Virtio transport (`virtio/`)

Implemented and tested on the PC; the QNX-only parts (`pci_qnx.c`, `mmio_qnx.c`)
are compile-checked against `tests/qnx-stub` in CI. What a real QNX guest reports
about the same device is in [§3.2](#32-a-real-qnx-71-guest-on-this-pc).

| ID | State | Proof |
|---|---|---|
| VIO-01 | **Tested on PC + observed on QNX** | vendor/device matching: `ctest -R test_virtio_pci`; QNX 7.1's `pci-tool` reports `1af4/1009` for our device (EVIDENCE.md §2) |
| VIO-02 | **Tested on PC + observed on QNX** | capability walk on a fake config space (`test_virtio_pci`); QNX exposes the MSI-X cap and the five vendor (virtio) caps |
| VIO-03 | **Tested on PC** | MMIO v1 and v2 against a RAM-backed register file: `ctest -R test_virtio_mmio` |
| VIO-04 | Implemented, **not verifiable here** | MSI-X programming with INTx fallback is separated from bus access (`test_virtio_pci` covers the programming); delivery needs QNX + a routed vector — see §3.2 and E1 |
| VIO-05 | **Tested on PC** | mount tag read from device config: `ctest -R test_virtio_dev` |
| VIO-06 | **Tested on PC** | one device object per share: `test_virtio_dev` |
| VIO-07 | **Tested on PC** | zero-copy scatter-gather: `ctest -R test_virtio_vq` (no intermediate copy in the transport) |
| VIO-08 | **Tested on PC** | indirect descriptors and notification suppression: `test_virtio_vq`, `test_virtio_dev` |
| VIO-09 | **Tested on PC** | reset and interrupt teardown: `test_virtio_dev` |
| VIO-10 | **Tested on PC** | the mock backend that makes all of the above possible without QNX: `ctest -R test_virtio_mock` |

### 2.2 9P2000.L protocol (`proto9p/`)

Portable C11, no OS header, tested on the PC — this is requirement P9-10 applied
to the whole layer.

| ID | State | Proof |
|---|---|---|
| P9-01 | **Tested on PC** | version and `msize` negotiation: `ctest -R test_p9_session` |
| P9-02 | **Tested on PC** | every core message encode/decode round trip: `ctest -R test_p9_codec` |
| P9-03 | **Tested on PC** | `tsetattr`, `tsymlink`, `treadlink`, `tlink`: `test_p9_codec` |
| P9-04 | **Tested on PC** | Linux↔QNX tables (`test_err_linux_table`, `test_flags_and_modes`) and their use in the session: `test_p9_session` |
| P9-05 | **Tested on PC** | several requests in flight, one tag each: `test_p9_session` |
| P9-06 | **Tested on PC** | `Tflush` on cancellation: `test_p9_session` |
| P9-07 | **Tested on PC** | 1 000 000 alloc/free on the fid and tag pools, no leak: `ctest -R test_p9_idpool` |
| P9-08 | **Tested on PC** | reads/writes/readdir split across messages and resumed: `test_p9_session` (a 300 KiB read with `msize` 8192) |
| P9-09 | **Tested on PC** | strict validation (size, type, tag, internal lengths, no trailing bytes): `ctest -R test_p9_validation`, plus `ctest -R fuzz_p9` |
| P9-10 | **Tested on PC** | the layer builds and its tests run with no QNX header: that is the CI `build-test` job |

### 2.3 Filesystem behaviour (`rmfs/`)

| ID | State | Proof |
|---|---|---|
| FS-01 | **Tested on PC** | open/read/write/seek/close with 64-bit offsets: `ctest -R test_rmfs_open` |
| FS-02 | **Tested on PC** | size, type, mode, owner, ns timestamps, nlink, stable ino: `ctest -R test_rmfs_meta` |
| FS-03 | **Tested on PC** | readdir resumed by offset, no entry lost or doubled (600 entries in pages of 7): `ctest -R test_directed` |
| FS-04 | **Tested on PC** | exclusive create, `O_TRUNC`, `O_APPEND`: `test_directed`, `test_rmfs_open` |
| FS-05 | **Tested on PC** | mkdir/rmdir/unlink/atomic rename incl. across directories: `test_directed`, `test_rmfs_meta` |
| FS-06 | **Not verified** | needs a QNX guest that executes a binary from the share — E3 |
| FS-07 | **Tested on PC** | chmod/chown/truncate/utimens: `test_rmfs_meta`, `test_directed` |
| FS-08 | **Tested on PC** | symlink create/read/resolve, loop detection (`PGW_SYMLINK_MAX`), hard links: `ctest -R test_rmfs_path`, `test_directed` |
| FS-09 | **Tested on PC** | permission matrix with a non-owner caller, `ro`/`noexec`/`nosuid`: `ctest -R test_rmfs_perms`, `test_directed` |
| FS-10 | **Tested on PC** | `fsync` reaches the backend, `statfs` reports the share: `test_directed` |
| FS-11 | **Tested on PC** | deleted-but-open stays readable until close: `ctest -R test_diff` |
| FS-12 | **Tested on PC** | a change made on the PC is seen without remounting (no data cache): `test_diff` |
| FS-13 | **Tested on PC** | bounded attribute cache, hits and misses counted, off by default: `ctest -R test_rmfs_cache` |
| FS-14 | **Tested on PC** | local byte-range locks between guest processes, released when the owner dies: `ctest -R test_rmfs_locks`, `test_directed` |
| FS-15 | **Tested on PC** | 255-byte names accepted, 256 refused, UTF-8 preserved: `test_directed` |
| FS-16 | Not started | X tier: relay locks to the server |

### 2.4 QNX integration (`drivers/`)

The driver sources exist and their portable half is tested; every requirement
below needs a QNX build (SDP) to be *verified*, and §3.2 records what a real QNX
7.1 guest already told us about the device itself.

| ID | State | Proof / next step |
|---|---|---|
| QNX-01 | Written, **compiles on PC**, not run | resource-manager glue in `drivers/common/src/resmgr_*_qnx.c`; type-checked by CI job `qnx-syntax` |
| QNX-02 | Written, not run | IFS fragment in `image/passerelle.build`; the guest's own `/proc/boot` shows the minimum is realistic (kernel, libc, `pci-server`) — EVIDENCE.md §4 |
| QNX-03 | Written, not measured | `drivers/common/src/prio_qnx.c` re-applies the caller priority for the whole exchange; needs the kernel trace — E1 |
| QNX-04 | **Tested on PC** | priority-ordered submission: `ctest -R test_rmfs_sched` (a priority request overtakes eight low-priority ones) |
| QNX-05 | **Tested on PC** | a parked request never holds the caller thread: `test_rmfs_sched` |
| QNX-06 | Written, not run | privilege drop + `image/passerelle.sec`; the guest ships `ability` and `libsecpol.so`, so the mechanism exists — EVIDENCE.md §4 |
| QNX-07 | Written, not run | `mount_qnx.c`; E1 |
| QNX-08 | **Tested on PC** (counters), backend not run | `pgw_stat_*` table exported as `name=value`; slog2 backend is `drivers/common/src/syslog_qnx.c` |
| QNX-09 | Written, not measured | trace events around each request in `drivers/common/src/trace_qnx.c`; needs the kernel trace tooling |

### 2.5 Performance

| ID | State | Proof / next step | Blocker |
|---|---|---|---|
| PERF-01 | **Baseline published** | `docs/measurements/linux-baseline-9p.md`: roundtrips per POSIX call from a real QEMU 8.2 trace (1 643 events, 822 requests) on an active Linux guest executing 30 create/stat/read/unlink cycles | Passerelle's own numbers need a QNX guest |
| PERF-02 … PERF-07 | Not started | `host/bench/` harness exists in Python (`pbench`), baseline recorded | **No SDP → no guest to measure** |

### 2.6 Robustness

| ID | State | Proof / next step |
|---|---|---|
| ROB-01 | **Tested on PC** | a refused operation comes back as a code, never as a hang: `ctest -R test_rmfs_meta`, `test_p9_validation` |
| ROB-02 | **Tested on PC** | no descriptor or fid left behind after a client disappears: `ctest -R test_diff` (20 000 open/close cycles, counters back to zero), `test_p9_idpool` |
| ROB-03 | Written, not run | unmount under load in `drivers/*/src/mount_qnx.c`; needs QNX |
| ROB-04 | **Partly tested on PC** | a file removed or replaced on the PC while in use gives clean errors: `test_diff`, `test_directed` |
| ROB-05 | **Tested on PC** | request abandoned after a configurable deadline, with an injected clock so no test sleeps: `ctest -R common` (`test_time_fake_clock`), `test_p9_session` |
| ROB-06 | **Tested on PC** | seeded campaigns against every decoder, under ASan+UBSan, with a header invariant that fails on a decoder accepting garbage: `ctest -R "fuzz_p9|fuzz_fuse"` |
| ROB-07 | Not started | X tier: a deliberately faulty 9P server over the network |

### 2.7 Security

| ID | State | Proof / next step |
|---|---|---|
| SEC-01 | **Tested on PC** | `nosuid` on by default, read-only mount refuses writes, permission matrix enforced in the guest: `ctest -R test_rmfs_perms`, `test_directed` |
| SEC-02 | Written, not run | the driver only opens its own device (single `pci_device_find` match); audit needs QNX |
| SEC-03 | **Implemented (docs)** | [architecture.md](architecture.md) §3.7 documents the QEMU security models; `qshare` defaults to `mapped-xattr`, which needs no root, and E5 ran `virtiofsd` unprivileged |

### 2.8 Developer experience

| ID | State | Proof / next step |
|---|---|---|
| DX-01 | **Verified on the host** | `qshare` emits the QEMU options and the `mkqnximage` fragment; those exact options booted a Linux guest (E5) and a QNX 7.1 guest (§3.2) |
| DX-02 | Not started | one-command demonstration needs the QNX driver |
| DX-03 | Partial | `qrun` written; needs a QNX guest with the driver mounted |
| DX-04 | **Implemented (docs)** | [user-guide.md](user-guide.md) §7 (CMake + Rust examples, also written by `qshare --example`) |
| DX-05 | **Implemented** | [user-guide.md](user-guide.md), [architecture.md](architecture.md) and the eight-chapter [codelab](../codelab/README.md) |
| DX-06 | Not started | X tier: whole system root on the share |

### 2.9 virtiofs (`protofuse/`) — implemented, tested on PC

| ID | State | Proof / next step | Blocker |
|---|---|---|---|
| VFS-01 | **Tested on PC** | `PGW_VIRTIO_ID_FS` = 26 and the tag/queue-count read: `ctest -R test_virtio_dev`, and confirmed against a real QEMU by E5 (guest reports device `0x001a`) | Discovery on QNX pending (E1) |
| VFS-02 | **Tested on PC** | hiprio lane for FORGET/INTERRUPT: `ctest -R protofuse_session` | — |
| VFS-03 | **Tested on PC** | version negotiation with fallback, full operation set, splitting on `max_write`, 8-aligned `reclen`: `ctest -R "protofuse_codec|protofuse_session"` | — |
| VFS-04 | **Tested on PC** | node reference counts, forget/batch-forget, session close settles every reference: `ctest -R protofuse_refs` (100 000 lookup/forget cycles) | — |
| VFS-05 | **Implemented** | `pgw_rmfs_from_fuse()` in `rmfs/adapters/fuse_adapter.c`, built when `pgw_protofuse` exists | Conformance run through the adapter needs a guest |
| VFS-06 | **Verified on the host** | `qshare` starts `virtiofsd` and a Linux guest mounts the share: `tests/qemu/run_share_check.py` (§3.1) | QNX guest side pending (E1) |
| VFS-07 | Not started | Needs the comparative bench on a QNX guest | No SDP |

### 2.10 Reverse sharing (X tier)

| ID | State | Proof / next step | Blocker |
|---|---|---|---|
| SRV-01, SRV-02 | Not started | First item to de-scope if the schedule slips (spec §10) | No SDP |

**Tally at snapshot time**: 1 module fully implemented and tested on PC (`common/`),
~11 requirements with design-level code present but not yet built (`rmfs/` sources,
`host/` Python tools), the rest not started. **Zero requirement is claimed as
verified under QNX**, because no QNX SDP is available on this workstation.

---

## 3. Feasibility Tests E1–E5

| Test | Question | State | What blocks it |
|---|---|---|---|
| **E1** | Under QEMU x86_64, does a QNX driver obtain the virtio PCI interrupt and exchange a first 9P message? | **Half answered** (11 Nov 2026, 17:20 CET) | **Enumeration and capability discovery: answered yes on a real QNX 7.1 guest** — `pci-tool` reports our device `1af4:1009` with the MSI-X capability and the five virtio vendor capabilities, and three assigned BARs. **Interrupt: measured as *not routed* for this device** (`Interrupt list - 0 assigned`), because the image only routes INTx for other bus/device addresses — see §3.2. Still blocked for the remaining half: the driver itself needs the SDP to be built |
| **E2** | Do clients A and B behave as advertised, and what do they measure? | **Blocked** | Both require the SDP to build; A is aarch64-only (needs `qemu-system-aarch64`, present, but no guest image), B needs Bazel + SDP |
| **E3** | Does QNX execute a program served by a third-party resource manager, shared libraries included? | **Blocked** | No SDP, no `mkqnximage` image. Fallback: limit the objective to data sharing |
| **E4** | Does `mkqnximage` accept extra QEMU options? | **Blocked** | `mkqnximage` ships with the SDP and is absent. Fallback already planned: `qshare` launches QEMU itself on the produced image |
| **E5** | On this PC, do `virtiofsd` and shared memory work with a Linux guest? | **Done — PASS** (11 Nov 2026, 10:47 CET) | Nothing. Executed by `python3 tests/qemu/run_share_check.py`: a minimal Alpine guest on the host kernel, booted by QEMU 8.2.2 with **the options `qshare` emits**, mounted both shares, read what the PC had written and wrote what the PC then read back. See §5 below |

**Unblocking checklist** (in order):

1. Install QNX SDP 8.0 (QNX Everywhere) → unblocks E2, E3, E4 and the rest of E1.
2. Build one `mkqnximage` x86_64le image with the driver embedded → unblocks E1, E3.
3. ~~Boot a minimal Linux guest with `virtiofsd`~~ → **done**, `tests/qemu/`.
4. ~~Boot a real QNX guest and ask its PCI server about the device~~ → **done**,
   `tests/qemu/qnx71/`, see §3.2.

---

## 3.1 E5, measured

Command and prerequisites (`tests/qemu/README.md` explains the mechanism):

```bash
python3 tests/qemu/run_share_check.py --protos 9p,virtiofs
```

Host: `baptiste-MS-7D18`, QEMU **8.2.2**, `virtiofsd` **1.10.0**
(`/usr/lib/qemu/virtiofsd`), guest kernel `6.14.0-37-generic` (direct boot, no
disk), initramfs = Alpine minirootfs 3.21.9 + the host kernel's `netfs`, `9pnet`,
`9pnet_virtio`, `9p` and `virtiofs` modules.

| Protocol | virtio device seen by the guest | Mount | PC → guest | guest → PC | Time |
|---|---|---|---|---|---|
| 9P2000.L (`-fsdev local,…,security_model=mapped-xattr` + `virtio-9p-pci`) | `0x0009` | PASS | PASS | PASS | 9.0 s |
| virtiofs (`memory-backend-memfd,share=on` + `vhost-user-fs-pci`) | `0x001a` = 26 | PASS | PASS | PASS | 9.0 s |

Two side results worth keeping:

- The device identifiers the guest reports match the constants the driver uses
  (`PGW_VIRTIO_ID_9P` = 9, `PGW_VIRTIO_ID_FS` = 26), which cross-checks VIO-01
  and VFS-01 against a real QEMU rather than against a datasheet.
- `virtiofsd` runs **without root** here (`--cache=never`, warnings about
  setuid only), which is what SEC-03 recommends for the QEMU side too.

Re-run in CI the same day, on a GitHub-hosted `ubuntu-24.04` runner (job
`e5-guest-share`, workflow `qemu-e2e`, out of pull requests): kernel
`7.0.0-1017-azure`, **virtiofs PASS in 7.2 s and 9P PASS in 6.7 s**, four
markers each, job total 40 s.
Two runner facts had to be handled, and both are documented in
[ci.md](ci.md) §3: the runner kernel is `-rw-------` so a step runs
`sudo chmod a+r /boot/vmlinuz-*`, and `linux-modules-extra` is not installable
there — the Azure kernel already ships `netfs`, `9pnet`, `9pnet_virtio`, `9p`
and `virtiofs` as modules, which is all the guest needs. The R630 workers have
neither QEMU nor `virtiofsd`, so the same step there reports an explicit skip.

What E5 does **not** cover: the QNX client. The guest above is Linux, so this
proves the host half of the plumbing (options, daemon, shared memory, tag,
read/write both ways) and nothing about our driver.

---

---

### 3.2 A real QNX 7.1 guest on this PC

`tests/qemu/qnx71/` boots QNX Neutrino **7.1.0** x86_64 (community image,
2022-07-12) under QEMU 8.2.2 with the same `virtio-9p-pci` options `qshare`
emits, then asks the guest's own tools what it sees. Full transcript and the
reasoning line by line: [`tests/qemu/qnx71/EVIDENCE.md`](../tests/qemu/qnx71/EVIDENCE.md).

Measured, copy-pasted from the guest:

| Observation | Meaning for the project |
|---|---|
| `uname -a` → `QNX vm1 7.1.0 ... x86pc x86_64` | a genuine QNX kernel, not a stub |
| `pci-tool -vv` → `vid/did: 1af4/1009` at `B000:D05:F00` | QNX's PCI server enumerates our device with exactly the IDs in `limits.h` (**VIO-01** premise verified on QNX) |
| `Capabilities list (6): 11 (MSI-X) --> 09 (VEND) ×5` | the five vendor capabilities a modern virtio device exposes are readable through QNX's PCI API, and MSI-X is present (**VIO-02**, **VIO-04**) |
| `Address Space list - 3 assigned` (I/O `c000`, MEM `feb96000`, 64-bit prefetch `fe000000`) | QEMU assigned the legacy PIO window, the modern configuration window and the shared-memory window; QNX reports them (**VIO-07** premise) |
| **`Interrupt list - 0 assigned`** | **no interrupt is routed to the device.** `pci_hw.cfg` in the image routes INTx only for `B0:D17:F0` and `B3:D0:F0`. A driver must either add its own entry (bus, device, pin — QEMU chose `pin A`) or take the MSI-X path, for which the image already ships `pci_cap-0x11.so` |
| `pci_server.cfg` → `DO_BUS_CONFIG=no` | the PCI server does **not** program BARs itself; they are assigned here by QEMU's firmware. A driver must not assume that on real hardware (decision D3) |
| `/proc/boot` contains `fs-dos.so`, `fs-qnx6.so`, **no 9P or virtiofs client** | the gap this project fills, now *observed* instead of asserted: a stock QNX runtime image cannot mount a host share |
| ships `libsecpol.so`, `ability`, `qconn`; **no `qcc`, no `mkifs`** | the QNX-06/SEC-01 mechanisms exist in a runtime image; but nothing can be compiled in or for it without the SDP |

What this does **not** do: build or run our driver. QNX 7.1 is also not QNX 8.0
— the kernel changes the specification relies on (no `InterruptAttach()`,
capability model) are not exercised by this image. It answers the *device*
question, not the *driver* question.

## 4. Known Issues at Snapshot Time

Third snapshot, **11 November 2026, 17:30 CET**: every module has a
`CMakeLists.txt`, the whole stack builds, both guards are green, E5 passes in CI,
and a real QNX 7.1 guest runs on this PC and confirms what the device looks like
from the QNX side.

| Issue | Impact | State |
|---|---|---|
| **No QNX SDP on the workstation** | The driver cannot be built for QNX, so E2, E3, E4 and the driver half of E1 remain impossible; PERF-02…07 have no number | **Open**, the single blocking item (issues #2–#6) |
| The QNX 7.1 guest ships no compiler | It answers device questions (enumeration, capabilities, BARs, interrupt routing) but cannot build or run our driver | **Open**, inherent to a runtime image |
| No interrupt routed to the device in the reference image | `pci_hw.cfg` only routes INTx for other bus/device addresses, so `pci-tool` reports `Interrupt list - 0 assigned` | **Measured** (§3.2): the driver must add its own INTx entry or use MSI-X, for which `pci_cap-0x11.so` is present |
| No published performance number | PERF-01…PERF-07 have harnesses (`host/bench`) but no measurement | **Open**: needs a QNX guest, nothing is claimed in the README |
| `lookup` / `getattr` symlink convention was inverted between `rmfs` and its backends | `lstat` answered `ELOOP` on a link loop while `stat` answered `OK`: FS-08 unreachable, every `lstat` on a symlink wrong | **Fixed** by the PC conformance bench; convention now written in `pgw/rmfs.h` |
| QNX priority passed through to the submission scheduler without conversion | A background copy at QNX priority 250 overtook a priority-10 read: PERF-05 inverted on target | **Fixed**: single conversion point `pgw_prio_from_qnx()` (`common/include/pgw/prio.h`), pinned by `common/tests/test_prio.c` |
| `*.log` in `.gitignore` swallowed the recorded QEMU traces under `host/tests/fixtures/` | `host-tools` failed on the runner with "missing trace fixture" while passing locally | **Fixed**: explicit negation in `.gitignore` |
| `qnx-syntax` include list was hard-coded to the portable modules | The job failed on `pgw/drvsrv.h`, the drivers' shared service layer | **Fixed**: the job now discovers every `include/` directory of the stack |

Both fixes in the middle of this table were found by *running* the code, not by
reading it: the first by `tests/e2e`, the second by writing the directed case for
PERF-05. That is the argument for the bench being part of the CI rather than a
pre-submission chore.

Re-check everything with:
```bash
python3 tools/check-size.py --root .          # < 500 lines/file, < 50 lines/function
python3 tools/check-spdx.py --root .          # Apache-2.0 header on every source file
cmake -B build && cmake --build build -j8 && ctest --test-dir build --output-on-failure
cmake -B build-asan -DPGW_SANITIZERS=ON && cmake --build build-asan -j8 \
    && ctest --test-dir build-asan --output-on-failure
(cd host && python3 -m pytest tests -q)
```
At the second snapshot: **28/28 C tests** in both the normal and the
ASan+UBSan build, **133 pytest tests** green (2 skipped: they need a QNX guest),
both guards OK.

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
  Every QNX-side validation below is blocked by this single fact.

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

What the PC bench **cannot** prove: anything that depends on the guest kernel —
priority inheritance in the trace (QNX-03), interrupt delivery (VIO-04), running
a binary straight from the share (FS-06), `mount`/`umount` (QNX-07), the security
policy (QNX-06). Those need E1–E5.

---

## 2. Requirement Matrix

### 2.1 Virtio transport (`virtio/`) — contract: `include/pgw/virtio.h` **[exists]**

| ID | State | Proof / next step | Blocker |
|---|---|---|---|
| VIO-01 | Not started | `pgw_vio_pci_open()` declared; device IDs in `limits.h` | Sources pending; E1 |
| VIO-02 | Not started | PCI modern capability walk | Sources pending; E1 |
| VIO-03 | Not started | `pgw_vio_mmio_open()` with `pgw_mmio_io` accessor | Sources pending |
| VIO-04 | Not started | MSI-X with INTx fallback (`allow_msix` flag) | Sources pending; E1 |
| VIO-05 | Not started | `pgw_vio_read_tag_9p()` / `pgw_vio_read_tag_fs()` | Sources pending |
| VIO-06 | Not started | One `pgw_vio_dev` per device | Sources pending |
| VIO-07 | Not started | `pgw_vio_sg` zero-copy scatter-gather | Sources pending |
| VIO-08 | Not started | `PGW_VIO_F_RING_INDIRECT_DESC` / `EVENT_IDX` bits declared | Sources pending |
| VIO-09 | Not started | `pgw_vio_destroy()` resets + tears down IRQ | Sources pending |
| VIO-10 | Not started | Mock backend `pgw_vio_mock_open()` designed for PC tests | Sources pending |

### 2.2 9P2000.L protocol (`proto9p/`) — contract: `include/pgw/p9.h` **[exists]**

| ID | State | Proof / next step | Blocker |
|---|---|---|---|
| P9-01 | Not started | `pgw_p9_enc_tversion` + `PGW_MSIZE_DEFAULT` (128 KiB) | Sources pending |
| P9-02 | Not started | Encoders/decoders declared for all core messages | Sources pending |
| P9-03 | Not started | `tsetattr`, `tsymlink`, `treadlink`, `tlink` declared | Sources pending |
| P9-04 | Implemented (tables) | `test_err_linux_table`, `test_flags_and_modes` in `common` | P9 wiring pending |
| P9-05 | Not started | `pgw_p9_rpc_fn` carries a `prio` argument | Sources pending |
| P9-06 | Not started | `pgw_p9_flush` declared | Sources pending |
| P9-07 | Not started | `pgw_p9_idpool` (fid/tag pools) declared | Sources pending |
| P9-08 | Not started | `pgw_p9_read/write/readdir` chunking documented | Sources pending |
| P9-09 | Not started | `pgw_p9_dec_*` validate length/type/tag/trailing bytes | Sources pending |
| P9-10 | Not started | Zero OS headers in `p9.h` — builds on PC by design | Sources pending |

### 2.3 Filesystem behaviour (`rmfs/`) — contract: `include/pgw/rmfs.h` **[exists]**, sources partly present

| ID | State | Proof / next step | Blocker |
|---|---|---|---|
| FS-01 | Not started | `pgw_rmfs_read/write` take `uint64_t` offsets ( > 4 GiB capable) | `rmfs/` has no `CMakeLists.txt` yet |
| FS-02 | Not started | `pgw_fs_attr` carries ns timestamps, nlink, stable `ino` | Not built |
| FS-03 | Not started | `pgw_rmfs_readdir` returns `next_off` | Not built |
| FS-04 | Not started | `pgw_oflags` carries EXCL/TRUNC/APPEND | Not built |
| FS-05 | Not started | `pgw_rmfs_mkdir/unlink/rename` declared | Not built |
| FS-06 | Not started | Execution from share | Not built; **E3** |
| FS-07 | Not started | `pgw_rmfs_chmod/chown/truncate/utimens` declared | Not built |
| FS-08 | Not started | `pgw_rmfs_symlink/readlink/link`, `PGW_SYMLINK_MAX`=40 | Not built |
| FS-09 | Not started | `pgw_fs_opts` + `pgw_fs_cred` permission model | Not built |
| FS-10 | Not started | `pgw_rmfs_fsync/statfs` declared | Not built |
| FS-11 | Not started | `rmfs/src/open_table.c` present | Not built |
| FS-12 | Not started | No data cache by default (`PGW_DATA_CACHE_DEFAULT=0`) | Not built |
| FS-13 | Not started | `rmfs/src/attr_cache.c` + `attr_ttl_ns` option | Not built |
| FS-14 | Not started | `rmfs/src/lock.c`, `pgw_rmfs_lock/getlock` | Not built |
| FS-15 | Not started | `PGW_NAME_MAX` = 255 UTF-8 bytes | Not built |
| FS-16 | Not started | X tier: relay locks to server | Not built |

### 2.4 QNX integration

| ID | State | Proof / next step | Blocker |
|---|---|---|---|
| QNX-01 | Not started | `drivers/` empty | No SDP |
| QNX-02 | Not started | IFS startup ordering | No SDP |
| QNX-03 | Not started | Priority inheritance demo | No SDP; needs kernel trace |
| QNX-04 | Implemented (design) | `rmfs/src/sched.c` + `pgw_prio_sched_*` | Not built; needs QNX measurement |
| QNX-05 | Not started | Non-blocking worker yield | No SDP |
| QNX-06 | Not started | Non-root + capabilities policy | No SDP |
| QNX-07 | Not started | `mount`/`umount` integration | No SDP |
| QNX-08 | Implemented (PC side) | `pgw_log_*` + `pgw_stat_*` counters; slog2 backend pending | Not built on QNX |
| QNX-09 | Not started | Kernel trace events | No SDP |

### 2.5 Performance

| ID | State | Proof / next step | Blocker |
|---|---|---|---|
| PERF-01 | **Baseline published** | `docs/measurements/linux-baseline-9p.md`: roundtrips per POSIX call from a real QEMU 8.2 trace (1 643 events, 822 requests) on an active Linux guest executing 30 create/stat/read/unlink cycles | Passerelle's own numbers need a QNX guest |
| PERF-02 … PERF-07 | Not started | `host/bench/` harness exists in Python (`pbench`), baseline recorded | **No SDP → no guest to measure** |

### 2.6 Robustness

| ID | State | Proof / next step | Blocker |
|---|---|---|---|
| ROB-01 | Not started | errno propagation path | Not built |
| ROB-02 | Not started | `rmfs/src/open_table.c` | Not built |
| ROB-03 | Not started | Unmount under load | No SDP |
| ROB-04 | Not started | Host-side unlink/replace handling | No SDP |
| ROB-05 | Implemented (design) | `PGW_TIMEOUT_MS_DEFAULT` + injectable clock (`test_time_fake_clock`) | Not built |
| ROB-06 | Not started | `-DPGW_SANITIZERS=ON` build option exists in root `CMakeLists.txt` | Fuzz harness pending |
| ROB-07 | Not started | Faulty-server tier (X) | Not built |

### 2.7 Security

| ID | State | Proof / next step | Blocker |
|---|---|---|---|
| SEC-01 | Implemented (design) | `pgw_fs_opts.nosuid` defaults to true | Not built |
| SEC-02 | Not started | Transport restricted to its own device | No SDP |
| SEC-03 | **Implemented (docs)** | [architecture.md](architecture.md) §3.7 documents QEMU security models | — |

### 2.8 Developer experience

| ID | State | Proof / next step | Blocker |
|---|---|---|---|
| DX-01 | Partial | `host/qshare/{cli,options,image,runner}.py` written | Not yet validated end-to-end |
| DX-02 | Not started | One-command demo | Needs driver + image |
| DX-03 | Partial | `host/qshare/qrun.py` written | Needs a running guest |
| DX-04 | **Implemented (docs)** | [user-guide.md](user-guide.md) §7 (CMake + Rust) | Examples not yet executed |
| DX-05 | Partial | This `docs/` set; `codelab/` empty | Tutorial pending |
| DX-06 | Not started | Root-on-share (X) | Needs driver |

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
| **E1** | Under QEMU x86_64, does a QNX driver obtain the virtio PCI interrupt and exchange a first 9P message? | **Blocked** | No QNX SDP (no `qcc`, no `mkqnximage`, no bootable QNX image). QEMU 8.2.2 itself is available. Method reference: Project B's manual MSI-X sequence (see [state-of-the-art.md](state-of-the-art.md)) |
| **E2** | Do clients A and B behave as advertised, and what do they measure? | **Blocked** | Both require the SDP to build; A is aarch64-only (needs `qemu-system-aarch64`, present, but no guest image), B needs Bazel + SDP |
| **E3** | Does QNX execute a program served by a third-party resource manager, shared libraries included? | **Blocked** | No SDP, no `mkqnximage` image. Fallback: limit the objective to data sharing |
| **E4** | Does `mkqnximage` accept extra QEMU options? | **Blocked** | `mkqnximage` ships with the SDP and is absent. Fallback already planned: `qshare` launches QEMU itself on the produced image |
| **E5** | On this PC, do `virtiofsd` and shared memory work with a Linux guest? | **Done — PASS** (11 Nov 2026, 10:47 CET) | Nothing. Executed by `python3 tests/qemu/run_share_check.py`: a minimal Alpine guest on the host kernel, booted by QEMU 8.2.2 with **the options `qshare` emits**, mounted both shares, read what the PC had written and wrote what the PC then read back. See §5 below |

**Unblocking checklist** (in order):

1. Install QNX SDP 8.0 (QNX Everywhere) → unblocks E1, E2, E3, E4.
2. Build one `mkqnximage` x86_64le image with the driver embedded → unblocks E1, E3.
3. ~~Boot a minimal Linux guest with `virtiofsd`~~ → **done**, `tests/qemu/`.

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

What E5 does **not** cover: the QNX client. The guest above is Linux, so this
proves the host half of the plumbing (options, daemon, shared memory, tag,
read/write both ways) and nothing about our driver. The CI job
`qemu-e2e` (out of pull requests) re-runs it and reports an explicit notice when
a runner lacks QEMU or `virtiofsd`, which is currently the case on the R630
workers.

---

---

## 4. Known Issues at Snapshot Time

Second snapshot, **11 November 2026, 05:00 CET**: every module now has a
`CMakeLists.txt`, the whole stack builds, and both guards are green.

| Issue | Impact | State |
|---|---|---|
| No QNX SDP on the workstation | Every QNX-side requirement stays unproven; feasibility tests E1–E5 blocked | **Open**, project-wide (issues #2–#6) |
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

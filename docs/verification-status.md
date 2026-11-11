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

### 2.5 Performance (all require a QNX guest)

| ID | State | Proof / next step | Blocker |
|---|---|---|---|
| PERF-01 … PERF-07 | Not started | `host/bench/` harness exists in Python (roundtrips, loads, report) | **No SDP → no guest to measure**; E5 for virtiofs |

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

### 2.9 virtiofs (`protofuse/`) — contract: `include/pgw/fusep.h` **[exists]**

| ID | State | Proof / next step | Blocker |
|---|---|---|---|
| VFS-01 | Not started | `PGW_VIRTIO_ID_FS` = 26, `read_tag_fs()` | Sources pending |
| VFS-02 | Not started | `hiprio` flag in `pgw_fuse_rpc_fn` | Sources pending |
| VFS-03 | Not started | Opcode enum + encoders/decoders declared | Sources pending |
| VFS-04 | Not started | `pgw_fuse_ref_get` / `forget_node` declared | Sources pending |
| VFS-05 | Not started | `pgw_rmfs_from_fuse()` declared | Sources pending |
| VFS-06 | Not started | `qshare` virtiofsd launch | `virtiofsd` present, E5 untested |
| VFS-07 | Not started | Benchmark inclusion | Needs driver |

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
| **E5** | On this PC, do `virtiofsd` and shared memory work with a Linux guest? | **Partially unblocked** | `virtiofsd` present (`/usr/libexec/virtiofsd`) and QEMU 8.2.2 supports `vhost-user-fs-pci`; **not yet executed** — needs a Linux guest kernel/rootfs to boot |

**Unblocking checklist** (in order):

1. Install QNX SDP 8.0 (QNX Everywhere) → unblocks E1, E2, E3, E4.
2. Build one `mkqnximage` x86_64le image with the driver embedded → unblocks E1, E3.
3. Boot a minimal Linux guest with `virtiofsd` → unblocks E5 (independent of the SDP).

---

## 4. Known Issues at Snapshot Time

| Issue | Impact | Owner |
|---|---|---|
| `size-guard` reports violations in modules under active development (`host/bench/cli.py`, `host/fsproof/guest_agent.py`, `host/fsproof/replay.py`, `rmfs/src/path.c` at the last check — the list changes as those files are edited) | CI red until each function is split below 50 lines | `host/bench`, `host/fsproof`, `rmfs` contributors |
| `host/tests/test_fsproof.py` failed to collect at one point: `ImportError: cannot import name 'test_concurrent_exclusive_create' from 'fsproof.cases'` | `pytest host/tests` aborts while `host/fsproof` is mid-edit | `host/fsproof` contributor |
| `rmfs/`, `virtio/`, `proto9p/`, `protofuse/` have no `CMakeLists.txt` yet | Not part of the CMake build yet | Module owners |
| No QNX SDP on the workstation | All QNX-side requirements unverified | Project-wide |

None of these violations is in `README.md` or `docs/`: the documentation set is
neutral with respect to the size guard (it scans only `.c`, `.h` and `.py`).

Re-check with:
```bash
python3 tools/check-size.py --root .
cmake -B build && cmake --build build -j8 && ctest --test-dir build --output-on-failure
python3 -m pytest host/tests -q
```

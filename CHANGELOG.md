<!--
SPDX-License-Identifier: Apache-2.0
-->

# Changelog

All notable changes to Passerelle are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Entries cite the requirement identifiers of the specification
([docs/spec-en.md](docs/spec-en.md)). What each identifier is worth *today*, with
the command that proves it, lives in
[docs/verification-status.md](docs/verification-status.md); what is still missing
lives in [docs/gap-analysis.md](docs/gap-analysis.md). This file only records what
changed.

## [Unreleased]

Verification-depth wave of 12 November 2026: what was "written, not run" becomes
"run, and it found things". Four bugs were found by *executing* the code against
a hostile or a larger-than-expected peer, not by reading it.

### Added

- `tests/faulty/`: a deliberately faulty 9P2000.L server — nine message faults
  (truncated frame, `size[4]` claiming 2 GiB, bad tag, out-of-order reply,
  garbage, a lying `Rread` count, a lying `Rwalk nwname`, version mismatch,
  short frame) and five transport faults (unsolicited reply, 4 GiB prefix, a
  frame that never ends, a reply 1.2 s late, `close()` mid-frame). `test_faulty.c`
  drives the production client against each one and asserts the error code, a
  bounded wait, exactly one fid left open, and — under ASan/UBSan — no
  out-of-bounds read and no leak (ROB-05, ROB-06, ROB-07, P9-07, P9-09).
- `tests/cas/`: the directed cases of §7.4 that were missing — the 5 GiB file
  (sparse), the 100 000-entry directory (paged), the maximum path length,
  exotic names (UTF-8, combining marks, emoji, hostile punctuation), a mount
  under a continuous workload with a bounded priority read, and what `fsync`
  really proves. 28 named cases, each with a scaled variant inside the 5-minute
  CI budget and a full-size variant registered `DISABLED` for the long
  campaigns (FS-03, FS-10, FS-13, FS-15, P9-08, PERF-04, PERF-05, ROB-02,
  ROB-03).
- `host/vhost/`: a vhost-user **front end** in Python — this process plays the
  hypervisor side of the socket, so the real `virtiofsd` becomes the peer
  instead of our own fake: framing and `REPLY_ACK`, feature negotiation, a
  memfd-backed memory table passed over `SCM_RIGHTS`, a split virtqueue in
  shared memory, eventfd kick/call, `SET_VRING_ENABLE`. `INIT`, `LOOKUP`,
  `GETATTR`, `OPEN`, `READ`, `WRITE` and `READDIR` are driven end to end and
  compared with the truth on the shared directory (VFS-02, VFS-03, VFS-04,
  VFS-06). 43 pytest cases, plus a Python vhost-user *backend* so the unit
  tests need no `virtiofsd`.
- `host/pgwutil/fuse_wire.py`: the single Python declaration of the FUSE wire
  ABI, now shared by `tests/fuse_ref/` and the vhost-user front end.
- `packaging/`: a Debian skeleton (`control`, `rules`, `changelog`, `copyright`,
  native source format) validated by pytest against `VERSION` and against the
  real file list, plus the pipx path (PR #37).
- `.devcontainer/`: one development-environment definition, shared with the
  root `Dockerfile` and validated statically (PR #37).
- `docs/vhost-user-front-end.md`: what that proof covers, and what it does not.

### Fixed

- **A hostile server could kill the client with a single `close()`**: after the
  peer closed the socket, `pgw_p9_session_close()` still sent its final
  `Tclunk` and the transport called `send()` without `MSG_NOSIGNAL`, so the
  client died of SIGPIPE (exit 141). The transport now reports `PGW_EIO`
  (ROB-07).
- **The POSIX truth backend silently truncated a directory at 4 096 entries**:
  the snapshot stopped reading with no error, so a client listing a 5 000-entry
  directory believed it had seen everything — the 100 000-entry case of the
  specification would have hidden 96 % of the tree. The snapshot now grows as
  the directory is read and the ceiling answers `EOVERFLOW` loudly (FS-03,
  PERF-04).
- **A valid name holding two dots was refused**: `pgw_posix_join()` rejected any
  path containing `..` as a substring, so `dots..inside` or `back..up.txt` —
  perfectly legal POSIX names — were answered `ENAMETOOLONG`. The guard now
  walks the components and refuses only a component that is exactly `..`
  (FS-15).
- **The reference-server socket was delayed**: no `TCP_NODELAY`, so every frame
  waited for Nagle and a round trip cost ~40 ms more than the protocol needs
  (P9-10).
- Two test buffers copied a 256-byte protocol name into a 64- and a 32-byte
  buffer; with `-Werror=format-truncation` the `Debug` build did not compile at
  all. Fixed, and `nightly.yml` now builds `gcc + Debug + -Werror` so it cannot
  rot again (PR #31, #35).



## [0.1.0] - 2026-11-11

First public release, prepared for the QNX Global Hackathon 2026.

The portable stack is implemented and tested on the host PC; the QNX driver is
written and syntax-checked but has never run, because no QNX SDP is available on
the development workstation. Feasibility test **E5 passes** (a Linux guest mounts
both shares through the exact options `qshare` emits) and **E1 is half answered**
(a real QNX 7.1 guest enumerates the device). See *Known limitations* below -
nothing in this release is claimed as verified under QNX.

### Added

- **Baseline layer** (`common/`): byte cursors with sticky failure and zero-copy
  take, Linux↔host errno / open-flag / mode tables, an injectable monotonic
  clock, a journal with per-operation counters, tunable limits in one header, and
  a C11 test harness used by every module (spec §3.1).
- **virtio transport** (`virtio/`): modern PCI capability walk and MMIO v1/v2
  register files, zero-copy scatter-gather queues, indirect descriptors,
  notification suppression, reset and interrupt teardown, and a mock backend
  that makes all of it testable without a guest (VIO-01 … VIO-10).
- **9P2000.L client** (`proto9p/`): version and `msize` negotiation, the full
  message set including `tsetattr`/`tsymlink`/`treadlink`/`tlink`, several
  requests in flight with one tag each, `Tflush` on cancellation, leak-free
  fid/tag pools, reads and directories split across messages and resumed, and
  strict validation of size, type, tag and internal lengths (P9-01 … P9-10).
- **virtiofs / FUSE client** (`protofuse/`): device id 26 and tag discovery,
  hiprio lane for FORGET and INTERRUPT, version negotiation with fallback, node
  reference counting with batch forget, `max_write` splitting, 8-aligned
  `reclen` (VFS-01 … VFS-04).
- **Filesystem layer** (`rmfs/`): path resolution with symlink-loop detection,
  descriptor table, bounded attribute cache with hit/miss counters, byte-range
  locks released when the owner dies, and a priority scheduler, plus 9P and FUSE
  adapters over one core (FS-01 … FS-15, VFS-05, QNX-04, QNX-05).
- **QNX driver scaffolding** (`drivers/`): resource-manager glue, caller-priority
  re-application, privilege drop and security policy file, `mount`/`umount`,
  slog2 backend, kernel-trace events (QNX-01 … QNX-09, SEC-02). Written and
  compiled on the PC against `tests/qnx-stub`; **never executed**.
- **Conformance benches** (`tests/`): a POSIX truth backend over a real
  directory, a seeded and replayable differential suite with automatic reduction
  to the shortest reproducing sequence, and directed edge cases (rename over an
  existing file, exclusive create, truncate, append, permission matrix with a
  non-owner caller, symlink loop, 255- and 256-byte names, non-empty `rmdir`,
  600-entry directory read in pages of 7, `fsync`, `statfs`, range locks)
  (spec §7.2, §7.4; FS-03, FS-11, FS-12, FS-15, ROB-01, ROB-02, P9-07).
- **Randomized-input campaigns** (`tests/fuzz/`): noise plus five kinds of damage
  on well-formed messages, against every wire decoder, with a header invariant
  that fails the test when a decoder accepts garbage (ROB-06, P9-09).
- **Host tools, pure standard library** (`host/`): `qshare` (QEMU option emitter,
  `--run`, `--demo`, `mkqnximage` snippets, security-model explainer), `qrun`,
  `fsproof` (diff / exercise / cases / guest agent) and `pbench` (round trips,
  throughput, metadata, mixed, priority, report) (DX-01, DX-03, DX-04, SEC-03,
  VFS-06, PERF-01 … PERF-07 harness).
- **E5 feasibility bench** (`tests/qemu/run_share_check.py`): boots a minimal
  Linux guest under QEMU with the options `qshare` emits, for both protocols, and
  checks the mount, a host→guest read and a guest→host write (VFS-06, SEC-03).
- **Linux 9P baseline** (`docs/measurements/linux-baseline-9p.md`): round trips
  per POSIX call measured from a real QEMU 8.2 trace (PERF-01).
- **QNX 7.1 guest evidence** (`tests/qemu/qnx71/EVIDENCE.md`): what a real QNX
  kernel reports about the device - `1af4:1009`, MSI-X and five virtio vendor
  capabilities, three assigned BARs, no interrupt routed (VIO-01, VIO-02, VIO-04
  premise, decision D3).
- **Documentation set** (`docs/`): specification FR/EN, architecture, user guide,
  decisions, verification status, measurements, gap analysis, and an
  eight-chapter codelab (DX-05).
- **Repository guards, run as CTest tests**: every source file under 500 lines and
  every function under 50 lines (`tools/check-size.py`), and an Apache-2.0 SPDX
  header on every C and Python file (`tools/check-spdx.py`).
- **Release pipeline**: a top-level `VERSION` file as the single source of truth,
  this changelog, a deterministic source tarball with a SHA256 sidecar, a
  coherence checker and a changelog extractor (`tools/release/`), a tag-triggered
  release workflow (`.github/workflows/release.yml`), and the procedure itself
  (`docs/release.md`).
- **Root `Makefile`**: `help`, `build`, `check`, `asan`, `bench`, `demo`,
  `coverage`, `size`, `spdx`, `tarball`, `release-check`, `install`, `clean` -
  every target announces an explicit skip when a tool it needs is absent.
- **Community files**: `CONTRIBUTING.md`, `SECURITY.md`, `CODE_OF_CONDUCT.md`.
- `qshare --version`, so an installed tool can be identified from the shell.

### Changed

- The version string now has a single source of truth: the top-level `VERSION`
  file, which `make release-check` verifies against `host/pyproject.toml`, this
  changelog and the git tag. Why the Python package still carries a literal
  copy, and how a release is cut, are in
  [docs/release.md](docs/release.md).
- QNX-only code moved into `*_qnx.c` files, added to the build `if(QNX)` only and
  syntax-checked on the PC with `-DPGW_QNX_STUBS=ON`, so the whole tree builds
  and tests without an SDP (P9-10).
- The root `CMakeLists.txt` now adds a module only when its own `CMakeLists.txt`
  exists, and `-DPGW_MODULES` restricts the set: parallel work on separate layers
  never touches the top-level file, and a half-finished module cannot break the
  others.
- CI moved to a 5-minute budget per job. The long campaigns (1 000 000
  differential operations, 5 000 000 fuzz iterations per decoder) and the QEMU
  end-to-end bench are registered but `DISABLED`, and run from `qemu-e2e.yml`
  (`workflow_dispatch` or a push on `main`), never from a pull request.
- `qshare --run` now starts `virtiofsd` itself for the virtiofs protocol, and
  defaults to the `mapped-xattr` security model, which needs no root (SEC-03,
  VFS-06).
- The E5 bench reports an explicit skip (exit 2) when the host kernel has no
  `9p` / `virtiofs` module, instead of failing or passing silently.

### Fixed

- `lookup` / `getattr` symlink convention was inverted between `rmfs` and its
  backends: `lstat` answered `ELOOP` on a link loop while `stat` answered `OK`,
  which made FS-08 unreachable and every `lstat` on a symlink wrong. Found by the
  PC conformance bench; the convention is now written in `pgw/rmfs.h` (FS-08).
- A QNX priority was passed through to the submission scheduler without
  conversion, so a background copy at QNX priority 250 overtook a priority-10
  read: PERF-05 was inverted on target. Now converted at a single point,
  `pgw_prio_from_qnx()`, pinned by `common/tests/test_prio.c` (PERF-05, QNX-03).
- `*.log` in `.gitignore` swallowed the recorded QEMU traces under
  `host/tests/fixtures/`, so the `host-tools` job failed on the runner while
  passing locally (PERF-01 fixtures).
- The `qnx-syntax` job had a hard-coded include list and failed on
  `pgw/drvsrv.h`, the drivers' shared service layer; it now discovers every
  `include/` directory of the stack.
- A red "Verdict" check could be nothing but a cancelled run; the CI verdict now
  distinguishes a failure from a cancellation.

### Known limitations

- **No QNX SDP on the development workstation.** `qcc` and `mkqnximage` are
  absent, so no driver in this release has ever been built for, or run on, QNX.
  QNX-01 … QNX-09, SEC-02, FS-06, DX-02 and DX-03 are written and type-checked
  against `tests/qnx-stub` only.
- **E1 is half answered.** A real QNX 7.1 guest enumerates the device, exposes
  the MSI-X capability and the five virtio vendor capabilities, and reports three
  assigned BARs - but that image routes no interrupt to the device, and the
  driver half of E1 still needs the SDP. E2, E3 and E4 remain blocked.
- **The guest evidence is QNX 7.1, not QNX 8.0.** The kernel differences the
  specification relies on are not exercised, and the guest ships no compiler, so
  nothing of ours can run inside it. The 6.1 GiB image is not in the repository:
  §3.2 of the verification status is not reproducible from the repository alone.
- **Only E5 passes end to end**, and its guest is Linux: it proves the host half
  of the plumbing (options, daemon, shared memory, tag, read/write both ways),
  and nothing about the Passerelle client.
- **PERF-02 … PERF-07 have no numbers.** Only the Linux baseline exists
  (PERF-01); measuring our client needs a QNX guest.
- **The one-hour fuzzing campaign of spec §7.6 is not run.** The honest claim is
  5 000 000 iterations per decoder family, clean under ASan+UBSan - not one hour
  of coverage-guided fuzzing.
- **De-scoped X-tier requirements** are not started: FS-16 (locks relayed to the
  server), ROB-07 (a deliberately faulty server), DX-06 (whole system rooted on
  the share), SRV-01 and SRV-02 (reverse sharing).
- **VFS-05 is built, not exercised**: the FUSE adapter compiles and is wired into
  `rmfs`, but the conformance bench has never run through it (VFS-07 likewise).
- **ROB-04 is partial**: a file removed while in use is covered, the
  replaced-while-in-use case is not.
- The client is tested against in-memory fakes and a POSIX truth backend; there
  is no reference 9P server or reference FUSE backend to cross-check against yet.

[0.1.0]: https://github.com/BaptisteFaisy/passerelle/releases/tag/v0.1.0

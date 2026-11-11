<!--
SPDX-License-Identifier: Apache-2.0
-->

# Gap analysis — what is still missing

**Date: 11 November 2026, 21:00 CET.** The specification
([spec-en.md](spec-en.md)) has **77 distinct numbered requirements** (87 lines if
the grouped ranges are counted line by line). This file lists **100 concrete
gaps**: the requirement lines that are not fully satisfied, and everything else a
submission is judged on that is not a requirement line.

Where the 77 requirements stand:

| State | Count | Meaning |
|---|---|---|
| Green | 50 | implemented **and** covered by an automated test that runs here |
| Partial | 21 | the code exists, only one side of the claim is proven |
| Not started | 6 | the design is decided, the work is not done |

Legend: **[S]** = needs the QNX SDP, blocked on a myQNX account · **[P]** =
doable here, now · **[X]** = extreme tier of the specification, de-scoped on
purpose.

---

## A. The 29 requirement gaps

| # | Gap | Blocked |
|---|---|---|
| 1 | **QNX-01** — the resource manager is written but has never run: pathname-space attach, `iofunc` dispatch, teardown | **[S]** |
| 2 | **QNX-02** — startup from the boot image: the IFS fragment exists (`image/passerelle.build`), nothing was ever built or booted | **[S]** |
| 3 | **QNX-03** — request processed at the caller's priority, shown in the kernel trace: the code re-applies the priority, the trace was never taken | **[S]** |
| 4 | **QNX-04** — priority order when the transport is saturated: proven on the PC, not measured on QNX | **[S]** |
| 5 | **QNX-05** — a blocked client must not hold a driver thread: proven on the PC, not measured on QNX | **[S]** |
| 6 | **QNX-06** — run without root with the minimum capabilities: the privilege drop and the policy file exist, neither ran | **[S]** |
| 7 | **QNX-07** — `mount` / `umount` integration: written, never executed | **[S]** |
| 8 | **QNX-08** — system log and per-operation counters: counters proven on the PC, the slog2 backend never ran | **[S]** |
| 9 | **QNX-09** — kernel trace events on send and receive: written, never measured | **[S]** |
| 10 | **FS-06** — execute a program served by the share, shared libraries included: feasibility E3, the point of the project | **[S]** |
| 11 | **FS-16** — relay locks to the server (X tier) | **[X]** |
| 12 | **PERF-02** — never exceed the best existing client per call: needs both clients measured on the same machine | **[S]** |
| 13 | **PERF-03** — 1 GiB sequential throughput, at least half of a Linux guest | **[S]** |
| 14 | **PERF-04** — 10 000 metadata operations and a 50 000-entry tree | **[S]** |
| 15 | **PERF-05** — a priority read during a bulk copy: the scheduler is proven on the PC, the end-to-end latency is not | **[S]** |
| 16 | **PERF-06** — mount takes under 50 ms | **[S]** |
| 17 | **PERF-07** — published comparative report (this driver, A, B, `scp`, virtiofs): harness and Linux baseline exist, the four other rows do not | **[S]** |
| 18 | **ROB-03** — unmount under load | **[S]** |
| 19 | **ROB-04** — a file replaced on the PC while in use: partly covered, the replace case is not | **[P]** |
| 20 | **ROB-07** — a deliberately faulty server must not crash the client (X tier): no faulty server exists | **[P]** |
| 21 | **SEC-02** — the driver touches no device but its own: written, never audited on a running system | **[S]** |
| 22 | **DX-02** — the one-command demonstration: the demo script exists, it cannot run without an image | **[S]** |
| 23 | **DX-03** — `qrun` executes a program from the share and returns its output and exit code | **[S]** |
| 24 | **DX-06** — whole system rooted on the share (X tier) | **[X]** |
| 25 | **VFS-05** — the same conformance bench through the FUSE adapter: the adapter is built, the bench never ran through it | **[P]** |
| 26 | **VFS-07** — virtiofs included in the comparative bench | **[S]** |
| 27 | **VIO-04** — MSI-X with INTx fallback: the programming is proven against a fake config space, delivery on QNX is not | **[S]** |
| 28 | **SRV-01** — a QNX guest exporting its tree, read by the PC (X tier) | **[S]** **[X]** |
| 29 | **SRV-02** — the export includes `/proc` and `/dev` (X tier) | **[S]** **[X]** |

## B. The 71 gaps that are not requirement lines

### B1. Publishing and packaging (13)

| # | Gap |
|---|---|
| 30 | No `CHANGELOG.md` — what changed, when, and against which requirement |
| 31 | No release notes for `v0.1.0`, the version a judge would point at |
| 32 | No git tag, no source tarball, no SHA256 of that tarball |
| 33 | The Python tools are not installable as a package (`pip install` is never tested) |
| 34 | No `qshare --version` |
| 35 | The console entry points (`qshare`, `qrun`, `fsproof`, `pbench`) are declared but never installed and run in CI |
| 36 | No root `Makefile`: `make check`, `make bench`, `make demo` do not exist |
| 37 | No `cmake --install` target, no `pkg-config` file, no exported CMake package |
| 38 | No out-of-tree install check (install to a prefix, run the tools from there) |
| 39 | No SBOM, no build provenance |
| 40 | No `SECURITY.md` — how to report a vulnerability in a driver |
| 41 | No `CONTRIBUTING.md`, no `CODE_OF_CONDUCT.md` |
| 42 | No `VERSION` file and no single source of truth for the version string |

### B2. Quality gates and CI (14)

| # | Gap |
|---|---|
| 43 | No coverage measurement (gcov/lcov) and no coverage report among the artifacts |
| 44 | No coverage badge |
| 45 | No static analysis job (clang-tidy / cppcheck) |
| 46 | No nightly job: the long campaigns and the fuzzing should run every night |
| 47 | No continuous fuzzing (libFuzzer with a corpus) |
| 48 | The fuzz corpus is not kept as a repository artifact for regression |
| 49 | No CTest presets (`ctest --preset quick`, `--preset long`) |
| 50 | No CI artifact upload (console logs, traces, bench reports) |
| 51 | No dependency cache for the QEMU and QNX downloads in CI |
| 52 | The QNX 7.1 guest job is not in CI — today it is only run by hand |
| 53 | The CI matrix covers one compiler only (gcc), not clang |
| 54 | CI does not run on tags |
| 55 | The 9P trace fixtures are never regenerated, so a parser change can keep passing on stale data |
| 56 | No formatting gate (`clang-format` + `.editorconfig` not enforced) |

### B3. Verification depth (13)

| # | Gap |
|---|---|
| 57 | **No reference 9P server**: the client is only tested against an in-memory fake, never against a real server implementation |
| 58 | **No reference FUSE backend**: the virtiofs client is only tested against a fake |
| 59 | No cross-check against a third-party 9P client (interoperability proof) |
| 60 | No cross-check against `diod`'s protocol description turned into an executable test |
| 61 | No property-based tests (Hypothesis) on the codec boundaries |
| 62 | The divergences reduced by the bench are not stored as regression cases |
| 63 | No load test: mount under a continuous workload |
| 64 | The 5 GiB file case of the directed suite is not implemented |
| 65 | The 100 000-entry directory case is not implemented |
| 66 | The maximum path length case is not implemented |
| 67 | Exotic names (UTF-8, combining marks, emoji) are only partly covered |
| 68 | Hard-link semantics beyond `nlink` are not checked |
| 69 | `fsync` durability is not checked against a real disk (only against the host cache) |

### B4. The QNX 7.1 guest we now have (10)

| # | Gap |
|---|---|
| 70 | The guest's `pci_hw.cfg` routes no interrupt for our device: there is no INTx entry for `B0:D05:F0` |
| 71 | The guest image has no compiler, so nothing of ours can run inside it |
| 72 | The guest is QNX 7.1, not 8.0: the kernel differences the specification relies on are not exercised |
| 73 | No automation: booting the guest and capturing its PCI view is a manual command sequence |
| 74 | The 6.1 GiB guest image is not in the repository and not versioned: nobody can reproduce it from the repository alone |
| 75 | No check that the guest boots with the `qshare` options **and** the `image/passerelle.build` fragment |
| 76 | The QNX 6.5 cross-check is not done: an SDP 6.5 (aarch64) exists in a public image and could at least prove the portable layers compile for a real QNX target, API generation aside |
| 77 | The INTx-versus-MSI-X question is measured but not *decided* in the design |
| 78 | No `pci_hw.cfg` fragment for our device in `image/` |
| 79 | No capture of the guest's kernel trace, so QNX-09 has no reference even guest-side |

### B5. The submission itself (12)

| # | Gap |
|---|---|
| 80 | No Devpost submission text (the form wants a description, a video link, a repository link) |
| 81 | No 60-second "read this first" summary at the top of the repository |
| 82 | No submission checklist (dates, licence, video, repository, demo command, prior-art disclosure) |
| 83 | The demo video itself is not recorded (explicitly postponed) |
| 84 | No poster or one-pager for the judges |
| 85 | The Discord question on the submission category (D1) is drafted but not asked |
| 86 | No upstream pull request to the existing 9P client (the patches are ready, nobody sent them) |
| 87 | No architecture diagram as an image (the ASCII one is fine, a picture is better) |
| 88 | No English glossary — the specification has one in French only |
| 89 | No `docs/adr/` — the decisions live in prose, not as records |
| 90 | No "what we tried that failed" note, which judges appreciate |
| 91 | The requirement traceability matrix is hand-written; it should be generated from the specification and the code |

### B6. Everyday use of the tools (9)

| # | Gap |
|---|---|
| 92 | No `qshare doctor`: one command saying what is missing here (QEMU, virtiofsd, SDP, kernel) |
| 93 | No `qshare init`: scaffolding a guest project ready to be shared |
| 94 | No `qshare smoke`: one command proving the whole chain works on this machine |
| 95 | The CMake and Rust examples (`qshare --example`) are written but never executed by CI |
| 96 | No packaging of the tools for `pipx` or `apt` |
| 97 | The `host/fsproof` guest agent has no way to run inside a guest yet (needs a cross-compiler) |
| 98 | No man pages for the four commands |
| 99 | The tools do not share one configuration file; each re-reads its own options |
| 100 | No `.devcontainer` or Dockerfile reproducing the development environment in one command |

---

## The ten I would do next, in order

Ranked by *value to the submission* over *effort*. All ten are **[P]**: doable on
this machine, without an account and without a camera.

| Rank | What | Why it pays | Effort |
|---|---|---|---|
| 1 | **Reference 9P server + cross-check** (57, 59, 60) | the client is only tested against a fake today; against a real server, PERF-01 becomes a measurement and P9-01…P9-10 stop being self-referential | 2 days |
| 2 | **Reference FUSE backend + cross-check** (58, 25) | same for virtiofs: VFS-01…VFS-07 and the VFS-05 bench through the adapter become real | 2 days |
| 3 | **Host-side vhost-user backend** (58, 75) | drive the real `virtiofsd` from our own mock transport: the first end-to-end proof of the virtiofs path, and it needs no QNX | 3 days |
| 4 | **Submission pack** (80, 81, 82, 84, 90, 91) | the deadline is 15 December; a judge must read, run and understand the project in five minutes, and the traceability matrix must be generated, not hand-written | 1 day |
| 5 | **Automate the QNX 7.1 guest** (52, 70, 73, 78) | what we discovered by hand today becomes a CI job, and the INTx entry for our device lands in `image/` | 1 day |
| 6 | **Publication pipeline** (30, 31, 32, 33, 34, 35, 36, 39) | a tagged, checksummed, installable release is what "open source project" means to a judge | 1 day |
| 7 | **Quality gates** (43, 44, 45, 46, 47, 48, 49, 50, 53, 56) | coverage, static analysis, nightly fuzzing, formatting: the difference between "it works" and "it is maintained" | 2 days |
| 8 | **Verification depth** (20, 61, 62, 63, 64, 65, 66, 67, 69) | property-based tests, the faulty server, the load test and the missing directed cases — this is where the remaining crashes hide | 3 days |
| 9 | **Documentation quality** (7, 8, 9, 87, 88, 89, 91) | Doxygen for the C API, decision records, diagrams, glossary: a repository is read as much as it is run | 2 days |
| 10 | **Everyday tooling** (92, 93, 94, 95, 96, 98, 99, 100) | `qshare doctor`, `qshare init`, `qshare smoke`, one command to reproduce the whole environment | 2 days |

Nothing on this list needs an account, a purchase or a camera.

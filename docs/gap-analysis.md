<!--
SPDX-License-Identifier: Apache-2.0
-->

# Gap analysis — what is still missing

**Date: 11 November 2026, 21:00 CET. Updated 12 November 2026, 15:30 CET.**
The specification ([spec-en.md](spec-en.md)) has **77 distinct numbered
requirements** (87 lines if the grouped ranges are counted line by line). This
file listed **100 concrete gaps**: the requirement lines that are not fully
satisfied, and everything else a submission is judged on that is not a
requirement line.

**68 of those 100 are closed** and are marked inline (`Closed 11-12/11` for the
night wave that delivered the reference servers, the publication pipeline, the
quality gates, the QNX 7.1 guest in CI and the submission pack; `Closed 12/11`
for the two waves that delivered the faulty server, the directed cases of §7.4,
the vhost-user front end driving the real `virtiofsd`, the shared configuration
file, the Debian skeleton, the devcontainer, the generated man pages, the
examples built in CI, the property harness and the kept corpus, the install and
export story, the SBOM, the pinned guest image, the architecture picture, the
English glossary and the everyday commands). Rows are kept rather than
deleted: the closure note says *what* closed them, so a claim can be checked.
What is left: **26 requirement gaps** (24 of them blocked on the QNX SDP, 2
de-scoped X-tier) and **6 non-requirement gaps**: the demo video, the Discord question and the upstream pull
request need a human, and the three QNX guest items (71, 72, 75) need an SDP or a guest
image with a toolchain.

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
| 1 | **Closed 13/11** - the resource manager runs in a real QNX 8.0 guest: the driver attaches at `/host`, the kernel dispatches to it, and the listing, the file reads and the sub-directories all come from the share (`tests/qemu/qnx8/`). Nine defects came out of it (the short device-specific region, the physical addresses of the descriptors, the reset that released the transport, the empty path of the mount point, ...) | **[T]** |
| 2 | **Closed 13/11** - the IFS is built with `mkifs` from `image/passerelle.build` and boots: the kernel, the C library, the PCI server, the tools and our two drivers are in it, and the start-up script mounts the share (`tests/qemu/qnx8/README.md`) | **[T]** |
| 3 | **Closed 14/11** - **QNX-03**: the kernel trace of the real guest carries the priority that served each request. 1,604 submissions decoded, priorities 10 and 63 present, and the payload operation agrees with the event code for every parsed event (`docs/measurements/qnx8-guest.md`) | **[S]** |
| 4 | **Closed 14/11** - **QNX-04**: the same trace pairs 1,177 transport round trips under a 16-reader bulk load; the high-priority reads are served beside the saturated copy, and the cookie collision that used to steal completions is fixed in `common/src/cookie.c` | **[S]** |
| 5 | **Partly closed 14/11** - **QNX-05**: the trace shows requests completing while sixteen bulk readers hold the transport, so a blocked client does not hold a driver thread. A dedicated saturation measurement is still not a benchmark series | **[S]** |
| 6 | **Closed 13/11** - the driver drops to `uid 200 gid 200` in the guest with four abilities (map the device BAR, `InterruptAttachThread` on its own line, `resmgr_attach` of the mount point, the dispatch channel), and it serves the share afterwards | **[T]** |
| 7 | **Closed 13/11** - the mount happens in the guest (`fs9p -o ro,nosuid,noexec hostshare /host`), the driver creates the mount point when the system allows it, and the listing of the mount point comes from the share | **[T]** |
| 8 | **Closed 13/11** - the slog2 backend runs in the guest: the driver writes its start-up, its mount and its errors in the `passerelle` buffer, readable with `slog2info` | **[T]** |
| 9 | **Closed 14/11** - **QNX-09**: the kernel trace is captured and read back on the real guest: 3,207 user events (1,604 submissions, 1,603 replies), every submission paired with its reply (`tests/qemu/qnx8/capture_trace.py` + `trace_report.py`). The events are labelled with the operation they belong to, not with a default: `Twalk` used to be labelled OPEN, which hid every path resolution | **[S]** |
| 10 | **Partly closed 13/11** - the share serves a program built for the target (`tests/qemu/qnx8/fs6-demo.c`, cross-compiled by `build-image.sh`), and the guest reads and copies it through the driver. **The exec itself does not work yet**: the kernel asks the filesystem for the *exec info* (the interpreter and its arguments) and answers with the shell instead. A filesystem that serves an executable has to reply to an `_IO_CONNECT_OPEN` carrying `_IO_FLAG_EX` with a link reply whose path holds that information - that reply is the missing piece | **[P]** |
| 11 | **FS-16** — relay locks to the server (X tier) | **[X]** |
| 12 | **PERF-02** — never exceed the best existing client per call: needs both clients measured on the same machine | **[S]** |
| 13 | **Partly closed 13/11** - 64 MiB read in **9.60 s** (6.7 MiB/s) through our driver in a real QNX 8.0 guest (`docs/measurements/qnx8-guest.md`). The comparison with a Linux guest on the same machine is still missing: the Linux baseline counts round trips per POSIX operation, not a bulk byte rate | **[P]** |
| 14 | **PERF-04** — 10 000 metadata operations and a 50 000-entry tree | **[S]** |
| 15 | **PERF-05** — a priority read during a bulk copy: the scheduler is proven on the PC, the end-to-end latency is not | **[S]** |
| 16 | **Closed 13/11** - the mount takes **11 ms** in a real QNX 8.0 guest (from the driver's first log line to the 9P session and the interrupt thread), measured with the system log (`docs/measurements/qnx8-guest.md`) | **[T]** |
| 17 | **PERF-07** — published comparative report (this driver, A, B, `scp`, virtiofs): harness and Linux baseline exist, the four other rows do not | **[S]** |
| 18 | **Partly closed 13/11** - the driver stops on a signal, the mount point leaves the namespace and a supervisor remounts it (`tests/qemu/qnx8/`: `slay fs9p` then the mount is back). The first attempt deadlocked in `resmgr_detach()`, which waits for the mount point's attribute; the driver no longer calls it, the kernel removes the entry when the process exits. **Still missing**: the unmount under load | **[P]** |
| 19 | **Closed 12/11** - `tests/cas/test_cas_replace.c`: a file replaced on the PC while it is open - the descriptor opened before keeps the old inode, one opened after sees the new file, the attributes of the name describe the new file, and the attribute cache serves no stale size or mtime (PR #55). **ROB-04** — a file replaced on the PC while in use: partly covered, the replace case is not | **[P]** |
| 20 | **Closed 12/11** - `tests/faulty/`: a faulty 9P server (9 message faults, 5 transport faults) driving the production client, plus `test_faulty.c` under ASan/UBSan. It found a real crash: a `close()` mid-frame killed the client with SIGPIPE (PR #33). **ROB-07** — a deliberately faulty server must not crash the client (X tier): no faulty server exists | **[P]** |
| 21 | **SEC-02** — the driver touches no device but its own: written, never audited on a running system | **[S]** |
| 22 | **Closed 13/11** - `tests/qemu/qnx8/build-image.sh` then `run-guest.sh` boot the guest and mount the share; the console shows the listing and the file contents | **[T]** |
| 23 | **DX-03** — `qrun` executes a program from the share and returns its output and exit code | **[S]** |
| 24 | **DX-06** — whole system rooted on the share (X tier) | **[X]** |
| 25 | **Closed 12/11** - `tests/fuse_e2e/`: the *existing* differential bench of `tests/e2e` now runs through the FUSE adapter against the same truth, through a mount provider instead of a copy of the bench (PR #55). **VFS-05** — the same conformance bench through the FUSE adapter: the adapter is built, the bench never ran through it | **[P]** |
| 26 | **VFS-07** — virtiofs included in the comparative bench | **[S]** |
| 27 | **Closed 14/11** - both delivery modes work in the real QNX 8.0 guest. MSI-X is now allocated by the **PCI server** (`vector bound to line 256`, `interrupt 256 attached (msix)`) through the documented sequence (`cap_msix_set_irq_entry` → `pci_device_cfg_cap_enable` → `pci_device_read_irq`), and INTx is still proven through `-o irq=10` (`31.20 MiB/s`, the same rate as MSI-X within the noise). Three defects only a real target could show: the shadow of configuration space carried no MSI-X capability, `pci_device_read_irq()` was asked for one line when the device has two, and `pgw_vio_reset()` cleared `msix_active` while the vector was still bound | **[T]** |
| 28 | **SRV-01** — a QNX guest exporting its tree, read by the PC (X tier) | **[S]** **[X]** |
| 29 | **SRV-02** — the export includes `/proc` and `/dev` (X tier) | **[S]** **[X]** |

## B. The 71 gaps that are not requirement lines

### B1. Publishing and packaging (13)

| # | Gap |
|---|---|
| 30 | **Closed 11-12/11** - `CHANGELOG.md`, Keep a Changelog format, entries cite the requirement identifiers. No `CHANGELOG.md` — what changed, when, and against which requirement |
| 31 | **Closed 11-12/11** - release notes for `v0.1.0` in `CHANGELOG.md` and `docs/release.md`. No release notes for `v0.1.0`, the version a judge would point at |
| 32 | **Closed 11-12/11** - `tools/release/` (tarball + SHA256) and `.github/workflows/release.yml`. No git tag, no source tarball, no SHA256 of that tarball |
| 33 | **Closed 11-12/11** - `release.yml` job `install-check`: `pip install ./host` in a venv. The Python tools are not installable as a package (`pip install` is never tested) |
| 34 | **Closed 11-12/11** - `qshare --version`, compared with the released tag by `release.yml`. No `qshare --version` |
| 35 | **Closed 11-12/11** - the four console scripts are installed and run in `release.yml`. The console entry points (`qshare`, `qrun`, `fsproof`, `pbench`) are declared but never installed and run in CI |
| 36 | **Closed 11-12/11** - root `Makefile`, 13 idempotent targets (`make help` lists them). No root `Makefile`: `make check`, `make bench`, `make demo` do not exist |
| 37 | **Closed 12/11** - `cmake --install` rules, a generated `passerelle.pc`, and an exported CMake package, so a downstream project can `find_package(Passerelle)` (PR #53). No `cmake --install` target, no `pkg-config` file, no exported CMake package |
| 38 | **Closed 12/11** - an out-of-tree install check: install to a throwaway prefix, then compile and link a downstream consumer against the installed package (PR #53). No out-of-tree install check (install to a prefix, run the tools from there) |
| 39 | **Closed 12/11** - a generated SBOM and a build-provenance record (compiler, flags, build type, commit), produced by a standard-library-only generator under `tools/` (PR #53). No SBOM, no build provenance |
| 40 | **Closed 11-12/11** - `SECURITY.md`. No `SECURITY.md` — how to report a vulnerability in a driver |
| 41 | **Closed 11-12/11** - `CONTRIBUTING.md` and `CODE_OF_CONDUCT.md`. No `CONTRIBUTING.md`, no `CODE_OF_CONDUCT.md` |
| 42 | **Closed 11-12/11** - `VERSION` is the single source of truth; `tools/release/test_version_sync.py` fails the build on any drift. No `VERSION` file and no single source of truth for the version string |

### B2. Quality gates and CI (14)

| # | Gap |
|---|---|
| 43 | **Closed 11-12/11** - `quality.yml` job `coverage`, report uploaded as an artefact. No coverage measurement (gcov/lcov) and no coverage report among the artifacts |
| 44 | **Closed 12/11** - the coverage badge is generated from the measured number and gated by a `--check` mode: a stale badge fails CI instead of lying (PR #53). No coverage badge |
| 45 | **Closed 11-12/11** - `quality.yml` job `static-analysis` (clang-tidy). No static analysis job (clang-tidy / cppcheck) |
| 46 | **Closed 11-12/11** - `nightly.yml`: long differential campaign, long fuzzing, compiler matrix, libFuzzer. No nightly job: the long campaigns and the fuzzing should run every night |
| 47 | **Closed 11-12/11** - `nightly.yml` job `libfuzzer` (`-DPGW_LIBFUZZER=ON`, bounded per harness). No continuous fuzzing (libFuzzer with a corpus) |
| 48 | **Closed 12/11** - the fuzz corpus is kept under `tests/corpus/fuzz/`, replayed by the normal CTest suite, and uploaded as a CI artefact by the nightly campaign (PR #57). The fuzz corpus is not kept as a repository artifact for regression |
| 49 | **Closed 11-12/11** - `CMakePresets.json`: `quick`, `ci`, `asan`, `long`, `coverage`, `fuzz`. No CTest presets (`ctest --preset quick`, `--preset long`) |
| 50 | **Closed 11-12/11** - `nightly.yml` and `qnx71-guest.yml` upload logs, traces and artefacts. No CI artifact upload (console logs, traces, bench reports) |
| 51 | **Closed 12/11** - the QEMU and QNX downloads are cached in CI, keyed on the pinned version and its checksum, with the measured cost of the cache written down (PR #54). No dependency cache for the QEMU and QNX downloads in CI |
| 52 | **Closed 11-12/11** - `.github/workflows/qnx71-guest.yml` boots a real QNX 7.1 guest on a hosted runner (~2 min) and cross-checks `image/pci_hw.cfg` against what QEMU assigns. The QNX 7.1 guest job is not in CI — today it is only run by hand |
| 53 | **Closed 11-12/11** - `nightly.yml` job `compilers`: gcc and clang; since 12/11 gcc in `Debug` with `-Werror` as well (PR #35). The CI matrix covers one compiler only (gcc), not clang |
| 54 | **Closed 11-12/11** - `release.yml` runs on tags. CI does not run on tags |
| 55 | **Closed 12/11** - the 9P trace fixtures are regenerated by `tools/regen-fixtures.py`, byte-identical run after run, and a check fails when the committed ones drift (PR #57). The 9P trace fixtures are never regenerated, so a parser change can keep passing on stale data |
| 56 | **Closed 11-12/11** - `quality.yml` job `format` with a ratchet baseline (`tools/quality/format-baseline.txt`), clang-format pinned to 23.1.5. No formatting gate (`clang-format` + `.editorconfig` not enforced) |

### B3. Verification depth (13)

| # | Gap |
|---|---|
| 57 | **Closed 11-12/11** - `tests/ref9p/`: an independent Python 9P2000.L server, and C benches driving our client against it over a socket. **No reference 9P server**: the client is only tested against an in-memory fake, never against a real server implementation |
| 58 | **Closed 11-12/11, and further on 12/11** - `tests/fuse_ref/` is the independent FUSE backend; `host/vhost/` now drives the *real* `virtiofsd` over a vhost-user socket (memfd memory table, split virtqueue, eventfd kick/call), so the virtiofs path is no longer tested only against our own code (PR #36). **No reference FUSE backend**: the virtiofs client is only tested against a fake |
| 59 | **Closed 12/11** - our client against `diod`, a third-party 9P server, in a CI job; the local run skips with the reason written out (PR #60). No cross-check against a third-party 9P client (interoperability proof) |
| 60 | **Closed 12/11** - `tests/conformance/`: diod's own description of the 9P2000.L wire turned into an executable conformance table — 44 rules, each citing the section of the excerpt it comes from and the named case that proves it; the table checks its own citations. The 25 rules that are statements about bytes are read at the offsets the description gives, the 10 sequence rules run through the production client, the 6 that need the real server run against `diod` in CI (PR #68). No cross-check against `diod`'s protocol description turned into an executable test |
| 61 | **Closed 12/11** - `tests/prop/`: a seeded, shrinking, bounded property harness over both codec boundaries - round trip, capacity bounds, extremes, stability. No `hypothesis` dependency (PR #49). No property-based tests (Hypothesis) on the codec boundaries |
| 62 | **Closed 12/11** - a reduced divergence is persisted as a regression case (`PGW_CORPUS_SAVE` / `tools/corpus-save.sh`) and replayed by `test_corpus` in the normal suite. **The corpus is empty and that is honest**: the differential bench is clean today (PR #49). The divergences reduced by the bench are not stored as regression cases |
| 63 | **Closed 12/11** - `tests/cas/test_cas_load.c`: a mount under a continuous workload, with a bounded priority read (PR #38). No load test: mount under a continuous workload |
| 64 | **Closed 12/11** - `tests/cas/test_cas_big_file.c`: the 5 GiB case, sparse, with a scaled variant in the PR gate and the full size as a `DISABLED` long test (PR #38). The 5 GiB file case of the directed suite is not implemented |
| 65 | **Closed 12/11** - `tests/cas/test_cas_big_dir.c`: 100 000 entries, paged. It exposed a silent 4 096-entry truncation in the truth backend, fixed in the same PR (#38). The 100 000-entry directory case is not implemented |
| 66 | **Closed 12/11** - `tests/cas/test_cas_path_len.c`: PATH_MAX and NAME_MAX on both sides, compared with the kernel (PR #38). The maximum path length case is not implemented |
| 67 | **Closed 12/11** - `tests/cas/test_cas_exotic.c`: UTF-8, combining marks, emoji, hostile punctuation. It exposed a `strstr(relpath, "..")` guard that refused valid names, fixed in the same PR (#38). Exotic names (UTF-8, combining marks, emoji) are only partly covered |
| 68 | **Closed 12/11** - `tests/cas/test_cas_hardlink.c`: `nlink` on every name, one inode behind two names, a write seen through the other, the unlink of one name, `rename` and `link` onto an existing target, no stale `nlink` in the attribute cache (PR #55). Hard-link semantics beyond `nlink` are not checked |
| 69 | **Closed 12/11** - `tests/cas/test_cas_fsync.c`: `fsync` proven to reach the backend and to survive a cache drop. Durability on a real platter is *not* claimed (PR #38). `fsync` durability is not checked against a real disk (only against the host cache) |

### B4. The QNX 7.1 guest we now have (10)

| # | Gap |
|---|---|
| 70 | **Closed 11-12/11** - `image/pci_hw.cfg` carries the INTx entry for `B0:D5:F0`, cross-checked in CI against what QEMU really assigns. The guest's `pci_hw.cfg` routes no interrupt for our device: there is no INTx entry for `B0:D05:F0` |
| 71 | The guest image has no compiler, so nothing of ours can run inside it |
| 72 | The guest is QNX 7.1, not 8.0: the kernel differences the specification relies on are not exercised |
| 73 | **Closed 11-12/11** - `qnx71-guest.yml` automates the boot, the SSH session, `pci-tool` and the artefact. No automation: booting the guest and capturing its PCI view is a manual command sequence |
| 74 | **Closed 12/11** - the guest disk is pinned by version and SHA256 in a committed manifest; a script fetches it, verifies the checksum and extracts it, and the CI job boots it in snapshot mode so the pinned disk stays pristine (PR #54). The 6.1 GiB guest image is not in the repository and not versioned: nobody can reproduce it from the repository alone |
| 75 | No check that the guest boots with the `qshare` options **and** the `image/passerelle.build` fragment |
| 76 | **Answered 12/11, deliberately not done** - the public SDP 6.5 image is identified (digest, 340 MB, **x86** target, not the aarch64 the gap assumed), but the SDP is proprietary and a third party pushing it to a public registry is not a redistribution right we can rely on, and the pull does not fit the 5-minute PR budget. The verdict is written up in `docs/qnx71-guest.md` (PR #54). The QNX 6.5 cross-check is not done: an SDP 6.5 (aarch64) exists in a public image and could at least prove the portable layers compile for a real QNX target, API generation aside |
| 77 | **Closed 12/11** - `docs/adr/ADR-0006-*.md`: MSI-X first, INTx as the fallback, with the measurements we have and what would make us revisit the decision (PR #54). The INTx-versus-MSI-X question is measured but not *decided* in the design |
| 78 | **Closed 11-12/11** - `image/pci_hw.cfg`. No `pci_hw.cfg` fragment for our device in `image/` |
| 79 | **Closed 12/11** - the guest CI job captures the kernel-side trace the image can give (`tracelogger`), and says plainly that it is not the SDP's `traceprinter` and that our driver never ran in that guest (PR #54). No capture of the guest's kernel trace, so QNX-09 has no reference even guest-side |

### B5. The submission itself (12)

| # | Gap |
|---|---|
| 80 | **Closed 11-12/11** - `docs/submission/devpost.md`. No Devpost submission text (the form wants a description, a video link, a repository link) |
| 81 | **Closed 11-12/11** - `docs/submission/readme-summary.md`, the 60-second read. No 60-second "read this first" summary at the top of the repository |
| 82 | **Closed 11-12/11** - `docs/submission/checklist.md`. No submission checklist (dates, licence, video, repository, demo command, prior-art disclosure) |
| 83 | The demo video itself is not recorded (explicitly postponed) |
| 84 | **Closed 11-12/11** - `docs/submission/one-pager.md`. No poster or one-pager for the judges |
| 85 | The Discord question on the submission category (D1) is drafted but not asked |
| 86 | No upstream pull request to the existing 9P client (the patches are ready, nobody sent them) |
| 87 | **Closed 12/11** - `docs/_assets/architecture.svg`: a hand-written SVG (valid XML, no external font or image), referenced from the architecture page and the README (PR #51). No architecture diagram as an image (the ASCII one is fine, a picture is better) |
| 88 | **Closed 12/11** - `docs/glossary.md`: the English glossary, one line per term, linked from the specification and the README (PR #51). No English glossary — the specification has one in French only |
| 89 | **Closed 11-12/11** - `docs/adr/`, five decision records. No `docs/adr/` — the decisions live in prose, not as records |
| 90 | **Closed 11-12/11** - `docs/submission/what-did-not-work.md`. No "what we tried that failed" note, which judges appreciate |
| 91 | **Closed 11-12/11** - `tools/traceability.py` with `tools/traceability-map.yml` generates the matrix from the specification and the code. The requirement traceability matrix is hand-written; it should be generated from the specification and the code |

### B6. Everyday use of the tools (9)

| # | Gap |
|---|---|
| 92 | **Closed 12/11** - `qshare doctor`: one command saying what is missing on this machine, the fix for each line, `--json` for machines, and an exit code that says whether the E5 demonstration can run here (PR #47). No `qshare doctor`: one command saying what is missing here (QEMU, virtiofsd, SDP, kernel) |
| 93 | **Closed 12/11** - `qshare init`: scaffolds a guest project ready to be shared, reusing the example writer, idempotent, refusing to overwrite without `--force` (PR #47). No `qshare init`: scaffolding a guest project ready to be shared |
| 94 | **Closed 12/11** - `qshare smoke`: one command that proves the chain works here, reporting with the reason whatever cannot run, never as a pass (PR #47). No `qshare smoke`: one command proving the whole chain works on this machine |
| 95 | **Closed 12/11** - `.github/workflows/examples.yml` scaffolds both examples with the real `qshare --example all`, builds the CMake one and runs its ctest, and builds the Rust one only when `cargo` is already present - an explicit `::notice::` skip otherwise, never a toolchain install (PR #43). The CMake and Rust examples (`qshare --example`) are written but never executed by CI |
| 96 | **Closed 12/11** - `packaging/`: a Debian skeleton validated by pytest against `VERSION`, plus the pipx path already covered by the release install check (PR #37). No packaging of the tools for `pipx` or `apt` |
| 97 | **Answered 12/11** - the guest agent is a single standard-library Python 3 file: it needs an *interpreter* in the guest, not a cross-compiler, and QNX Neutrino has none. `host/fsproof/agent.sh` fixes the entry point and the interface is documented for a native C agent or a CPython port (PR #51). The `host/fsproof` guest agent has no way to run inside a guest yet (needs a cross-compiler) |
| 98 | **Closed 12/11** - `man/qshare.1`, `qrun.1`, `fsproof.1`, `pbench.1`, generated from the CLI definitions by `tools/man/build_man.py`; the date is pinned (`tools/man/PAGE_DATE`) and a pytest fails on any drift (PR #43). No man pages for the four commands |
| 99 | **Closed 12/11** - one configuration module for the four tools, precedence: command line > environment > configuration file > built-in default, with `--show-config` (PR #47). The tools do not share one configuration file; each re-reads its own options |
| 100 | **Closed 12/11** - `.devcontainer/`: one environment definition shared with the root `Dockerfile`, validated statically (PR #37). No `.devcontainer` or Dockerfile reproducing the development environment in one command |

---

## The ten I would do next, in order

Ranked by *value to the submission* over *effort*. All ten are **[P]**: doable on
this machine, without an account and without a camera.

| Rank | What | Why it pays | Effort |
|---|---|---|---|
| 1 | **Reference 9P server + cross-check** (57, 59, 60) - delivered 11-12/11 (`tests/ref9p/`, then the `diod` cross-check) | the client is only tested against a fake today; against a real server, PERF-01 becomes a measurement and P9-01…P9-10 stop being self-referential | 2 days |
| 2 | **Reference FUSE backend + cross-check** (58, 25) - delivered 11-12/11 (`tests/fuse_ref/`, then `host/vhost/` against the real `virtiofsd`) | same for virtiofs: VFS-01…VFS-07 and the VFS-05 bench through the adapter become real | 2 days |
| 3 | **Host-side vhost-user backend** (58, 75) - **delivered 12/11** (`host/vhost/`, PR #36) | drive the real `virtiofsd` from our own mock transport: the first end-to-end proof of the virtiofs path, and it needs no QNX | 3 days |
| 4 | **Submission pack** (80, 81, 82, 84, 90, 91) - delivered 11-12/11 (`docs/submission/`, `docs/adr/`, generated traceability) | the deadline is 15 December; a judge must read, run and understand the project in five minutes, and the traceability matrix must be generated, not hand-written | 1 day |
| 5 | **Automate the QNX 7.1 guest** (52, 70, 73, 78) - delivered 11-12/11, hardened 12/11 (`qnx71-guest.yml`, `image/pci_hw.cfg`, pinned checksummed image, ADR on MSI-X/INTx) | what we discovered by hand today becomes a CI job, and the INTx entry for our device lands in `image/` | 1 day |
| 6 | **Publication pipeline** (30, 31, 32, 33, 34, 35, 36, 39) - delivered 11-12/11 (`VERSION`, `CHANGELOG.md`, `Makefile`, `tools/release/`, `release.yml`) | a tagged, checksummed, installable release is what "open source project" means to a judge | 1 day |
| 7 | **Quality gates** (43, 44, 45, 46, 47, 48, 49, 50, 53, 56) - delivered 11-12/11 (coverage, clang-tidy, nightly, presets, format ratchet) | coverage, static analysis, nightly fuzzing, formatting: the difference between "it works" and "it is maintained" | 2 days |
| 8 | **Verification depth** (20, 61, 62, 63, 64, 65, 66, 67, 69) - **delivered 12/11** (`tests/faulty/`, `tests/cas/`, `tests/prop/`, `tests/corpus/`) | property-based tests, the faulty server, the load test and the missing directed cases — this is where the remaining crashes hide | 3 days |
| 9 | **Documentation quality** (7, 8, 9, 87, 88, 89, 91) - delivered 11-12/11, completed 12/11 (5 ADRs + the MSI-X one, generated traceability, architecture SVG, English glossary) | Doxygen for the C API, decision records, diagrams, glossary: a repository is read as much as it is run | 2 days |
| 10 | **Everyday tooling** (92, 93, 94, 95, 96, 98, 99, 100) - **delivered 12/11** (shared configuration file, `doctor`/`init`/`smoke`, man pages, examples built in CI, Debian skeleton, devcontainer) | `qshare doctor`, `qshare init`, `qshare smoke`, one command to reproduce the whole environment | 2 days |

Nothing on this list needs an account, a purchase or a camera.

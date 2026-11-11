# Building Passerelle

This document gives the exact build instructions required by the QNX Global
Hackathon 2026: **QNX version, target architectures, toolchain**, plus how to
build and run every test level.

> **Honest status.** The host PC used for development has **no QNX SDP installed**
> (`qcc`, `mkqnximage` absent). Everything in §2 is verified and reproducible today;
> everything in §3 is the documented procedure, **to be validated** as soon as an
> SDP is available. See [verification-status.md](verification-status.md).

---

## 1. Target Environment (what the submission targets)

| Item | Value |
|---|---|
| **QNX version** | **QNX SDP 8.0** (QNX Everywhere distribution) |
| **Guest architectures** | `x86_64le` (primary: PCI modern + MSI-X) and `aarch64le` (virtio-mmio v1/v2, machine `virt`) |
| **Guest image** | produced by `mkqnximage`, booted under QEMU |
| **Hypervisor** | QEMU **8.2 or newer** (`virtio-9p-pci`, `virtio-9p-device`, `vhost-user-fs-pci`) |
| **Compiler for guest** | `qcc` (QNX SDP 8.0), driven by CMake |
| **Compiler for host PC** | any C11 compiler (`gcc` or `clang`), CMake ≥ 3.16 |
| **C standard** | C11 strict, `CMAKE_C_EXTENSIONS OFF` (no GNU extensions except `__attribute__((format))` already used by `common/`) |
| **Host languages for tools** | Python 3.9+ (no third-party dependency) |

### Source layout relevant to the build

```
common/     portable, always built (buf, err, flags, log, time, test harness)
virtio/     Layer 1 — portable core + QNX-only bus backends
proto9p/    Layer 2 — 9P2000.L, portable
protofuse/  Layer 2 — FUSE/virtiofs, portable
rmfs/       Layer 3 — resource manager core, portable; QNX glue in *_qnx.c
drivers/    driver executables (QNX only)
host/       Python tools: qshare, fsproof, bench (host PC only)
```

The root `CMakeLists.txt` adds a module **only if its `CMakeLists.txt` exists**, so
the tree configures at any stage of development. `-DPGW_MODULES="..."` restricts
the build to a subset.

### CMake options

| Option | Default | Meaning |
|---|---|---|
| `PGW_MODULES` | all modules | Restrict the build to a subset |
| `PGW_SANITIZERS` | `OFF` | Build with ASan + UBSan (ROB-06) |
| `PGW_WERROR` | `ON` | Warnings are errors (`-Wconversion -Wsign-conversion` included) |
| `PGW_QNX_STUBS` | `OFF` | Compile QNX-target files on the PC against `tests/qnx-stub/include` |
| `PGW_FUZZ` | `OFF` | Build the fuzz harnesses (level 1) |

---

## 2. Build on a Host PC (no QNX SDP needed)

All portable layers and their tests build and run on plain Linux:

```bash
cd passerelle

# 1. Configure, build, test
cmake -B build -DPGW_MODULES="common"
cmake --build build -j8
ctest --test-dir build --output-on-failure

# 2. Same with sanitizers (ROB-06)
cmake -B build-asan -DPGW_MODULES="common" -DPGW_SANITIZERS=ON
cmake --build build-asan -j8
ctest --test-dir build-asan --output-on-failure

# 3. Project rules: < 500 lines per file, < 50 lines per function
python3 tools/check-size.py --root .

# 4. Host-side Python tools (qshare, fsproof, bench)
python3 -m pytest host/tests -q
```

> **Note.** Name your build directories `build`, `build-asan`, `build-*` is fine, but
> `tools/check-size.py` skips only the exact directory names `build` and `build-asan`.
> A directory named `build-docs` would be scanned and can trip the size guard on
> CMake's generated `CompilerIdC.c`. Use `build` / `build-asan`, or add the directory
> to `SKIP_DIRS`.

Once the other modules land, extend `PGW_MODULES`:

```bash
cmake -B build "-DPGW_MODULES=common;virtio;proto9p;protofuse;rmfs"
```

---

## 3. Cross-Compilation for QNX 8.0

```bash
# Source the SDP environment (sets QNX_HOST, QNX_TARGET, qcc on PATH)
source ~/qnx800/qnxsdp-env.sh

# x86_64le guest
cmake -B build-qnx-x86 \
      -DCMAKE_TOOLCHAIN_FILE="$QNX_HOST/usr/cmake/QNXToolchain.cmake" \
      -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
      "-DPGW_MODULES=common;virtio;proto9p;protofuse;rmfs;drivers"
cmake --build build-qnx-x86 -j8

# aarch64le guest
cmake -B build-qnx-arm \
      -DCMAKE_TOOLCHAIN_FILE="$QNX_HOST/usr/cmake/QNXToolchain.cmake" \
      -DCMAKE_SYSTEM_PROCESSOR=aarch64le \
      "-DPGW_MODULES=common;virtio;proto9p;protofuse;rmfs;drivers"
cmake --build build-qnx-arm -j8
```

**Isolation rule for QNX-only code.** Any file including `<sys/neutrino.h>`,
`<hw/pci.h>` or `<sys/resmgr.h>` must be a `*_qnx.c` file added to the build
**only** `if(QNX)` in its module's `CMakeLists.txt`. Its portable logic lives in a
sibling file that compiles on the PC. The QNX glue is syntax-checked on the PC with
the `pgw_qnx_syntax()` helper (against `tests/qnx-stub/include`, `-DPGW_QNX_STUBS=ON`).

**CMake helpers** (from `cmake/passerelle.cmake`, mandatory for every module):

| Helper | Purpose |
|---|---|
| `pgw_library(<name>)` | Builds a static library with the project's strict C11 flags and warning set |
| `pgw_test(<name>)` | Registers a CTest test using the `common/include/pgw/test.h` harness |
| `pgw_qnx_syntax(<name> SRCS ...)` | Compiles QNX-only sources on the PC against the stub headers |

### Installing into a QNX image

```bash
cmake --install build-qnx-x86 --prefix "$QNX_TARGET/x86_64le"
# then embed the driver and the mount line via the snippet qshare generates:
qshare ~/my-project --mkqnximage ~/qnx-vm --driver-bin /system/bin/qnx-fs-9p
```

---

## 4. Running Each Test Level

The validation plan (spec §7.1) defines five levels. Levels 0 and 1 run on the PC;
levels 2–4 need a QNX guest under QEMU.

### Level 0 — Unit tests (PC)

```bash
cmake -B build "-DPGW_MODULES=common;virtio;proto9p;protofuse;rmfs"
cmake --build build -j8
ctest --test-dir build --output-on-failure
```
Covers message encoding/decoding, translation tables, fid/node lifetime, path logic.

### Level 1 — Random inputs / fuzzing (PC, sanitizers required)

```bash
cmake -B build-fuzz -DPGW_SANITIZERS=ON -DPGW_FUZZ=ON \
      "-DPGW_MODULES=common;proto9p;protofuse"
cmake --build build-fuzz -j8
ctest --test-dir build-fuzz --output-on-failure -L fuzz
```
Target: 1 hour clean (Complete tier), 24 hours (Extreme tier). No crash, no ASan/UBSan report.

### Level 2 — Conformance (QEMU guest)

Differential testing against the host directory as ground truth:

```bash
# On the host, with the guest booted and the share mounted at /host:
python3 -m fsproof diff --count 100000 --seed 42 --reg-dir regressions
python3 -m fsproof exercise --dir /host/ex --ops 1000000 --threads 16
python3 -m fsproof cases --dir /host/cases            # targeted edge cases
python3 -m fsproof diff --client qnx-fs-9p            # same bench against Project A
python3 -m fsproof diff --client eclipse-virtio9p     # …and Project B
```
Each run is replayable from its seed; any divergence is minimised to the shortest
reproducing sequence and stored under `regressions/`.

### Level 3 — Performance (QEMU guest)

```bash
python3 -m bench roundtrips /tmp/qemu-trace.log        # PERF-01/02: round trips per call
python3 -m bench throughput --dir /host/bench --size-mb 1024   # PERF-03
python3 -m bench metadata  --dir /host/bench --count 10000     # PERF-04
python3 -m bench mixed     --dir /host/bench --count 500       # §7.5 mixed load
python3 -m bench exec      --dir /host/bench --count 1000      # exec-from-share
python3 -m bench priority  --dir /host/bench --count 10        # PERF-05
python3 -m bench report -o bench_report                        # PERF-07
```
QEMU traces come from `-trace events=...` (the v9fs trace events); the report is
machine-independent where it counts round trips.

### Level 4 — Robustness (QEMU guest)

```bash
# kill a client mid-request, unmount under load, mutate files on the host,
# point the driver at a deliberately faulty 9P server
python3 -m fsproof diff --count 10000 --seed 7 --kill-clients
umount /host                       # while a large copy runs
python3 -m fsproof cases --dir /host/cases --faulty-server
```

---

## 5. Continuous Integration

CI runs on self-hosted R630 workers with a **5-minute budget per job** (see
[ci.md](ci.md)): configure + build + `ctest` + `size-guard` on the PC build, plus the
sanitizer build. QEMU end-to-end jobs are kept out of pull requests and run on tags
or manual dispatch.

---

## 6. Troubleshooting the Build

| Symptom | Cause | Fix |
|---|---|---|
| `module 'X' not present yet - skipped` | The module has no `CMakeLists.txt` | Expected during parallel development; narrow `PGW_MODULES` |
| `size-guard` fails on `build-docs/.../CompilerIdC.c` | Build directory not in `SKIP_DIRS` | Use `build` / `build-asan` |
| `-Wconversion` errors | Implicit narrowing | Add an explicit cast **with a bounds check**; never silence the warning |
| `qcc: command not found` | SDP environment not sourced | `source ~/qnx800/qnxsdp-env.sh` |
| `<sys/neutrino.h>: No such file` during a PC build | QNX-only file included unconditionally | Move it to a `*_qnx.c` file, add it `if(QNX)` only |
| Link error on `InterruptAttachThread` in a PC build | QNX glue compiled without stubs | Configure with `-DPGW_QNX_STUBS=ON` |

<!--
SPDX-License-Identifier: Apache-2.0
-->

# Contributing to Passerelle

Passerelle is a 9P2000.L and virtiofs client for QNX guests under QEMU: a
filesystem driver, plus the host tools that make it usable. Contributions are
welcome - code, tests, measurements, documentation, and bug reports that come
with a reproducer.

Two documents answer most questions before you write a line:

- [docs/building.md](docs/building.md) - toolchain, CMake options, every test level.
- [docs/verification-status.md](docs/verification-status.md) - what is proven today,
  and with which command. Read this before claiming a requirement is met.

Security problems do **not** go in a public issue: see [SECURITY.md](SECURITY.md).

---

## 1. What you need

| Tool | Version | Notes |
| --- | --- | --- |
| CMake | ≥ 3.16 | drives every build |
| C compiler | C11 | `gcc` or `clang`; `-Werror` is on by default |
| Python | ≥ 3.10 | host tools and tests; **standard library only** |
| pytest | any | tests only, never in production code |
| QEMU | ≥ 8.2 | for the end-to-end bench (`make demo`) |
| QNX SDP 8.0 | - | **not required**: everything portable builds and tests without it |

`make help` lists every entry point. `make build` and `make check` are all a new
contributor needs on day one.

---

## 2. Build and test

```bash
make build        # configure + build every portable module in ./build
make check        # build + ctest + pytest + the two repository guards
make asan         # same tests under ASan + UBSan (ROB-06)
make size         # the code-size rule, plus the largest files of the tree
make spdx         # the licence-header rule
make bench        # the long campaigns of spec 7.6 (minutes, not seconds)
make demo         # the E5 bench: a real guest on the options qshare emits
```

Every target is idempotent, and a target whose tool is missing prints why it was
skipped instead of pretending to pass. Override the knobs when you need to:

```bash
make check JOBS=8 BUILD_DIR=/tmp/my-build
make bench DIFF_OPS=100000 FUZZ_ITERS=100000
```

Working on one layer only? Restrict the build so another module's work in
progress cannot break yours:

```bash
cmake -B build-proto9p "-DPGW_MODULES=common;proto9p;tests" && cmake --build build-proto9p -j8
```

---

## 3. The rules the CI enforces

These are not style preferences; two of them are CTest tests, so a violation
fails `ctest`, `make check` and the pull request.

1. **Under 500 lines per file, under 50 lines per function.**
   `python3 tools/check-size.py --root .` must stay green. Split the file
   (module, helper, sub-component) instead of raising the limit.
2. **Every C and Python file starts with the SPDX header**, then a comment that
   says what the file is for and which requirements of the specification it
   covers:

   ```c
   /* SPDX-License-Identifier: Apache-2.0 */
   /*
    * pgw_p9_codec - 9P2000.L message encode/decode (P9-02, P9-03, P9-09).
    */
   ```

   `python3 tools/check-spdx.py --root .` must stay green.
3. **C11 strict.** `-std=c11`, `CMAKE_C_EXTENSIONS OFF`, `-Wall -Wextra
   -Wconversion -Wsign-conversion -Werror`. No GNU extension except
   `__attribute__((format))`. Fix a `-Wconversion` warning with a cast *and* a
   bounds check; never silence it.
4. **Zero duplication.** If a logic already exists in `common/`, `host/` or
   another layer, reuse it. Extract a helper instead of copy-pasting;
   parametrise instead of forking. One implementation per logic.
5. **Python: standard library only** for anything that ships. pytest is allowed
   in tests and nowhere else.
6. **Comments, documentation and commit messages in English.**
7. **No secret, no personal data, no compiled binary in the repository.** QEMU
   traces under `host/tests/fixtures/` are the documented exception: they are
   test fixtures, and `.gitignore` re-includes them on purpose.
8. **QNX-only code lives in `*_qnx.c`**, added to the build `if(QNX)` only, and
   syntax-checked on the PC with `-DPGW_QNX_STUBS=ON` against
   `tests/qnx-stub/include`. Its portable logic lives in a sibling file that
   compiles everywhere (P9-10).
9. **A module never edits the root `CMakeLists.txt`.** The root file adds a
   module only when that module has its own `CMakeLists.txt`, so parallel work
   does not collide. Use the helpers of `cmake/passerelle.cmake`:
   `pgw_library()`, `pgw_test()`, `pgw_qnx_syntax()`.
10. **CI budget: 5 minutes per job.** Every job of every workflow carries an
    explicit `timeout-minutes` ≤ 5 and a `concurrency` group, and checkouts use
    `fetch-depth: 1`. Long benches stay out of pull requests: register them
    `DISABLED` in CTest and run them from `qemu-e2e.yml` or `make bench`.
    `python3 .github/ci-budget/check_timeouts.py` is the guard.

---

## 4. What a change must bring with it

- **A test.** A requirement is *implemented* when code exists and an automated
  test exercises it; without the test it is "written, not run", and
  `docs/verification-status.md` records that difference honestly. C tests use
  the `pgw_test` harness (`common/include/pgw/test.h`); host tools use pytest
  under `host/tests/`.
- **The requirement identifiers.** Cite them (`FS-08`, `P9-09`, `VIO-04`, ...)
  in the file header comment, in the test names and in the commit message, so
  the traceability matrix stays real.
- **A CHANGELOG entry** for anything a user can see: `CHANGELOG.md`, in the
  `[Unreleased]`-style section of the next version, Keep a Changelog format.
- **An updated status line** in `docs/verification-status.md` when a requirement
  changes state, with the command that proves it.
- **Measurements when you claim a number.** A performance claim needs the
  machine, the kernel/QEMU version, the command and the raw result, under
  `docs/measurements/`. An unreproducible number is worse than no number.

---

## 5. Pull requests and merges

1. Fork or branch from `main`; one branch per subject, small commits.
2. Commit messages: imperative subject, the requirement identifiers, and *why*
   when the *what* is not obvious. Example:
   `rmfs: keep the caller priority across a split read (FS-01, PERF-05)`.
3. Open the pull request against `main`. Describe what changed, how it is
   tested, and what is still not proven - the reviewers read
   `docs/verification-status.md` and expect the same honesty in the description.
4. The required checks are `ci-budget`, `build-test`, `sanitize`, `host-tools`,
   `qnx-syntax` and `rules`. They must all be green.
5. **Mergify is the only merge driver.** A ready pull request is queued
   automatically and rebased onto `main`; `main` is protected, so nobody pushes
   to it directly. Use the `hold` label to keep a pull request out of the queue,
   `urgent` to move it up, `priority-low` to move it down.
6. Mark a work in progress as a **draft**: drafts are never queued.
7. Never force-push a branch that someone else is reviewing; add a commit
   instead, and squash on merge if the history is noisy.

---

## 6. Releasing

Versions, tags, the source tarball and its SHA256 are described in
[docs/release.md](docs/release.md). `VERSION` is the single source of truth;
`make release-check` is the gate.

---

## 7. Getting help

Open an issue for a bug (with the output of `make check` and the environment
facts: kernel, QEMU version, compiler), or for a question about the
specification. Be kind to the people who answer: this project is built by a
small team, and every contributor is expected to follow the
[Code of Conduct](CODE_OF_CONDUCT.md).

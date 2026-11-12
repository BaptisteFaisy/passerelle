# SPDX-License-Identifier: Apache-2.0
#
# Passerelle - developer entry points.
#
# Every target is idempotent: running it twice changes nothing the second time.
# A target that needs a tool this machine does not have says so explicitly and
# skips (exit 0) instead of failing silently or pretending to pass. Only the
# tools the target cannot do without (cmake for `build`, python3 everywhere)
# make it fail, and then the message names the missing tool.
#
# Variables you may override:
#   make check JOBS=8 BUILD_DIR=/tmp/pgw-build
#   make bench DIFF_OPS=100000 FUZZ_ITERS=100000
#   make release-check TAG=v0.1.0
#   make install PREFIX=/opt/passerelle

SHELL := /bin/bash
.DEFAULT_GOAL := help

PYTHON      ?= python3
CMAKE       ?= cmake
CTEST       ?= ctest
BUILD_DIR   ?= build
ASAN_DIR    ?= build-asan
DIST_DIR    ?= dist
PREFIX      ?= /usr/local
JOBS        ?= 4
HOST_DIR    ?= host
# Directory `make init` scaffolds; empty means "nothing to do".
INIT_DIR    ?=
# Extra flags of `make smoke` (--fast, --dry-run, --json, ...).
SMOKE_FLAGS ?=
MODULES     ?= common;virtio;proto9p;protofuse;rmfs;drivers;tests
# Long campaigns of spec 7.6, out of every pull request (CI budget: 5 min).
DIFF_OPS    ?= 1000000
FUZZ_ITERS  ?= 5000000
VERSION     := $(shell cat VERSION 2>/dev/null | tr -d '[:space:]')
# `trash` keeps a mistake recoverable; rm -rf is the fallback when it is absent.
REMOVE      := $(shell command -v trash >/dev/null 2>&1 && echo trash || echo rm -rf)

.PHONY: help build check asan bench demo coverage size spdx tarball \
        release-check install clean doctor init smoke

help: ## Show this list
	@echo "Passerelle $(VERSION) - make targets"
	@echo
	@grep -hE '^[a-z-]+:.*?## ' $(MAKEFILE_LIST) \
	  | awk 'BEGIN {FS = ":.*?## "}; {printf "  %-14s %s\n", $$1, $$2}'
	@echo
	@echo "  build dir: $(BUILD_DIR)   jobs: $(JOBS)   modules: $(MODULES)"
	@echo "  override: make check JOBS=8 BUILD_DIR=/tmp/pgw-build"

build: ## Configure and build every portable module (no QNX SDP needed)
	@command -v $(CMAKE) >/dev/null 2>&1 || { \
	  echo "build: FAILED - $(CMAKE) not found (CMake >= 3.16 required)"; exit 1; }
	$(CMAKE) -B $(BUILD_DIR) -S . "-DPGW_MODULES=$(MODULES)"
	$(CMAKE) --build $(BUILD_DIR) -j$(JOBS)

check: build ## Build, run ctest + pytest and both repository guards
	@$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure
	@$(PYTHON) -c 'import pytest' 2>/dev/null || { \
	  echo "check: FAILED - pytest not found ($$($(PYTHON) -V) -m pip install pytest)"; exit 1; }
	$(PYTHON) -m pytest host/tests tools/release -q
	$(PYTHON) tools/check-size.py --root .
	$(PYTHON) tools/check-spdx.py --root .
	@echo "check: OK - build, ctest, pytest, size-guard and spdx-guard all green"

asan: ## Build with ASan+UBSan (ROB-06) and run the same tests
	@command -v $(CMAKE) >/dev/null 2>&1 || { \
	  echo "asan: FAILED - $(CMAKE) not found"; exit 1; }
	$(CMAKE) -B $(ASAN_DIR) -S . -DPGW_SANITIZERS=ON "-DPGW_MODULES=$(MODULES)"
	$(CMAKE) --build $(ASAN_DIR) -j$(JOBS)
	$(CTEST) --test-dir $(ASAN_DIR) --output-on-failure
	@echo "asan: OK - sanitizers reported nothing"

bench: build ## Long campaigns of spec 7.6 (1M differential ops, 5M fuzz iters)
	@set -e; \
	for bin in test_diff fuzz_p9 fuzz_fuse; do \
	  if [ ! -x "$(BUILD_DIR)/tests/$$bin" ]; then \
	    echo "bench: SKIPPED - $(BUILD_DIR)/tests/$$bin is not built"; \
	    echo "       (the 'tests' module must be in MODULES: $(MODULES))"; exit 0; \
	  fi; \
	done; \
	echo "bench: $(DIFF_OPS) differential operations per seed (~1 min)"; \
	PGW_DIFF_OPS=$(DIFF_OPS) $(BUILD_DIR)/tests/test_diff; \
	echo "bench: $(FUZZ_ITERS) randomized iterations per decoder family"; \
	PGW_FUZZ_ITERS=$(FUZZ_ITERS) $(BUILD_DIR)/tests/fuzz_p9; \
	PGW_FUZZ_ITERS=$(FUZZ_ITERS) $(BUILD_DIR)/tests/fuzz_fuse; \
	echo "bench: OK - no divergence, no sanitizer report"

demo: ## E5 bench: boot a Linux guest on the options qshare emits
	@set +e; $(PYTHON) tests/qemu/run_share_check.py; rc=$$?; \
	if [ $$rc -eq 2 ]; then \
	  echo "demo: SKIPPED - a prerequisite is missing (QEMU, virtiofsd, host kernel"; \
	  echo "      modules or the Alpine minirootfs); the reasons are printed above"; \
	  exit 0; \
	fi; \
	if [ $$rc -ne 0 ]; then echo "demo: FAILED - the bench reported a failing check"; fi; \
	exit $$rc

doctor: ## Say what is missing on this machine (qshare doctor)
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
	  echo "doctor: SKIPPED - $(PYTHON) not found (the host tools are Python)"; exit 0; }
	@set +e; PYTHONPATH=$(HOST_DIR) $(PYTHON) -m qshare.cli doctor; rc=$$?; \
	if [ $$rc -eq 3 ]; then \
	  echo "doctor: the E5 demonstration cannot run here;"; \
	  echo "        the table above says what to install"; \
	else echo "doctor: OK - the E5 demonstration can run here"; fi; \
	exit 0

init: ## Scaffold a guest project ready to be shared (qshare init)
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
	  echo "init: SKIPPED - $(PYTHON) not found (the host tools are Python)"; exit 0; }
	@if [ -z "$(INIT_DIR)" ]; then \
	  echo "init: SKIPPED - no directory given (make init DIR=/path/to/project)"; exit 0; \
	fi
	@PYTHONPATH=$(HOST_DIR) $(PYTHON) -m qshare.cli init $(INIT_DIR)

smoke: ## Prove the whole chain works here (qshare smoke)
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
	  echo "smoke: SKIPPED - $(PYTHON) not found (the host tools are Python)"; exit 0; }
	@set +e; PYTHONPATH=$(HOST_DIR) $(PYTHON) -m qshare.cli smoke $(SMOKE_FLAGS); rc=$$?; \
	if [ $$rc -eq 3 ]; then \
	  echo "smoke: SKIPPED - nothing could run here (the reasons are printed above)"; \
	  exit 0; \
	fi; \
	if [ $$rc -ne 0 ]; then echo "smoke: FAILED - a step failed (see above)"; fi; \
	exit $$rc

coverage: ## Line coverage of the C stack (needs gcovr or lcov)
	@set -e; \
	if ! command -v gcovr >/dev/null 2>&1 && ! command -v lcov >/dev/null 2>&1; then \
	  echo "coverage: SKIPPED - neither gcovr nor lcov is installed"; \
	  echo "            (python3 -m pip install gcovr, or apt install lcov)"; exit 0; \
	fi; \
	if ! command -v $(CMAKE) >/dev/null 2>&1; then \
	  echo "coverage: FAILED - $(CMAKE) not found"; exit 1; \
	fi; \
	$(CMAKE) -B build-coverage -S . "-DPGW_MODULES=$(MODULES)" \
	  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="--coverage" \
	  -DCMAKE_EXE_LINKER_FLAGS="--coverage"; \
	$(CMAKE) --build build-coverage -j$(JOBS); \
	$(CTEST) --test-dir build-coverage --output-on-failure; \
	mkdir -p coverage; \
	if command -v gcovr >/dev/null 2>&1; then \
	  gcovr --root . build-coverage --print-summary \
	    --html-details coverage/index.html --html-title "Passerelle $(VERSION)"; \
	else \
	  lcov --capture --directory build-coverage --output-file coverage.info; \
	  genhtml coverage.info --output-directory coverage; \
	fi; \
	echo "coverage: OK - report in coverage/"

size: ## Report the code-size rule (< 500 lines/file, < 50 lines/function)
	@set +e; $(PYTHON) tools/check-size.py --root .; rc=$$?; \
	echo; echo "  largest tracked source files (lines):"; \
	if command -v git >/dev/null 2>&1; then \
	  git ls-files '*.c' '*.h' '*.py' | xargs -r wc -l \
	    | grep -v ' total$$' | sort -rn \
	    | awk 'NR <= 10 {printf "    %6d  %s\n", $$1, $$2}'; \
	else echo "    (git unavailable - listing skipped)"; fi; \
	echo; \
	if command -v git >/dev/null 2>&1; then \
	  c_lines=$$(git ls-files '*.c' '*.h' | xargs -r cat | wc -l); \
	  py_lines=$$(git ls-files '*.py' | xargs -r cat | wc -l); \
	  files=$$(git ls-files | wc -l); \
	  echo "  totals: $$files tracked files, $$c_lines lines of C, $$py_lines lines of Python"; \
	fi; \
	if [ $$rc -ne 0 ]; then echo; echo "size: FAILED - the violations above must be split, not tolerated"; fi; \
	exit $$rc

spdx: ## Check the Apache-2.0 SPDX header of every C and Python file
	@$(PYTHON) tools/check-spdx.py --root .

tarball: ## Build the deterministic source tarball and its SHA256 in dist/
	@$(PYTHON) tools/release/make_tarball.py --output-dir $(DIST_DIR)

release-check: ## Verify VERSION/CHANGELOG/pyproject/tag coherence, then the tarball
	@$(PYTHON) tools/release/verify_release.py $(if $(TAG),--tag $(TAG) --require-tag,)
	@$(PYTHON) -m pytest tools/release -q
	@$(PYTHON) tools/release/make_tarball.py --output-dir $(DIST_DIR)
	@$(PYTHON) tools/release/verify_release.py \
	  --tarball $(DIST_DIR)/passerelle-$(VERSION).tar.gz
	@echo "release-check: OK - $(VERSION) is publishable (see docs/release.md)"

install: build ## Install headers, libraries and drivers under PREFIX
	@out=$$($(CMAKE) --install $(BUILD_DIR) --prefix $(PREFIX) 2>&1); echo "$$out"; \
	if ! printf '%s' "$$out" | grep -q "Installing"; then \
	  echo "install: SKIPPED for the C artefacts - this build tree declares no install()"; \
	  echo "         rule yet (the root CMakeLists.txt owns them)"; \
	else echo "install: OK - C artefacts under $(PREFIX)"; fi; \
	echo "install: the host tools are a Python package: python3 -m pip install ./host"

clean: ## Remove the build trees and the release artefacts this Makefile makes
	@for dir in $(BUILD_DIR) $(ASAN_DIR) build-coverage $(DIST_DIR) coverage coverage.info; do \
	  if [ -e "$$dir" ]; then $(REMOVE) "$$dir" && echo "clean: removed $$dir"; fi; \
	done
	@echo "clean: OK - only the artefacts of this Makefile were removed"

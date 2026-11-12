#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Shared helpers of the tools/quality scripts (Python standard library only).

Every quality script of this repository uses the same conventions, so that a
GitHub workflow and a local run behave identically:

* exit codes   : 0 = ok, 1 = violation/failure, 2 = usage error,
                 3 = explicit skip (a required tool is missing). A skip is
                 never reported as a success: the caller prints a notice.
* annotations  : GitHub Actions annotations when GITHUB_ACTIONS is set,
                 plain readable lines otherwise.
* tree walking : the same skip rules as tools/check-size.py (build trees,
                 caches) plus the vendored trees, so no script invents its
                 own list of directories to ignore.
* subprocesses : always bounded by a timeout; nothing waits forever.

Requirements covered: the hackathon brief (standard library only, English
comments, files under 500 lines), ROB-06 (instrumented builds must stay
measurable) and 7.6 (long campaigns are reported, never silently shortened).
"""

from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_USAGE = 2
EXIT_SKIP = 3

# Directories never worth scanning: build trees (any name starting with
# "build", like build/ and build-asan/), caches and vendored code.
SKIP_DIR_NAMES = {
    ".git",
    "__pycache__",
    ".venv",
    "venv",
    ".tox",
    ".pytest_cache",
    ".mypy_cache",
    "node_modules",
    ".ci-tools",
}
SKIP_DIR_PREFIX = ("build",)
# Vendored CI socle, maintained centrally and copied verbatim in every
# repository of this account (the same exclusion list as tools/check-size.py).
SKIP_PATH_PREFIXES = ((".github", "hermes"), (".github", "ci-budget"))
# upstream/ holds third-party code kept for reference, codelab/ holds the
# walkthrough samples: neither is formatted nor analysed by our own rules.
VENDORED_TOP = ("upstream", "codelab")
SOURCE_EXTS = {".c", ".h", ".py"}
# Modules of the portable stack; anything else is host tooling or tests.
PROD_MODULES = ("common", "virtio", "proto9p", "protofuse", "rmfs", "drivers")


def in_actions() -> bool:
    """True when running inside a GitHub Actions job."""
    return os.environ.get("GITHUB_ACTIONS") == "true"


def notice(message: str) -> None:
    """Print an informational message (Actions annotation when relevant)."""
    print("::notice::%s" % message if in_actions() else message)


def warning(message: str) -> None:
    """Print a warning (Actions annotation when relevant)."""
    print("::warning::%s" % message if in_actions() else "warning: %s" % message)


def error(message: str) -> None:
    """Print an error (Actions annotation when relevant)."""
    print("::error::%s" % message if in_actions() else "error: %s" % message)


def gh_summary(text: str) -> None:
    """Append markdown to the job summary (no-op outside GitHub Actions)."""
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    try:
        with open(path, "a", encoding="utf-8") as handle:
            handle.write(text.rstrip("\n") + "\n")
    except OSError as exc:  # a summary is a bonus, never a failure
        warning("cannot append to GITHUB_STEP_SUMMARY: %s" % exc)


def repo_root(start: str | os.PathLike[str] | None = None) -> pathlib.Path:
    """Return the repository root (nearest .git, else the start directory)."""
    here = pathlib.Path(start or os.getcwd()).resolve()
    for candidate in [here, *here.parents]:
        if (candidate / ".git").exists() or (candidate / "CMakeLists.txt").exists():
            return candidate
    return here


def is_skipped(rel_parts: tuple[str, ...], skip_vendored: bool = True) -> bool:
    """True when a relative path must not be scanned."""
    for part in rel_parts:
        if part in SKIP_DIR_NAMES or part.startswith(SKIP_DIR_PREFIX):
            return True
    for prefix in SKIP_PATH_PREFIXES:
        if rel_parts[:len(prefix)] == prefix:
            return True
    if skip_vendored and rel_parts and rel_parts[0] in VENDORED_TOP:
        return True
    return False


def iter_files(root: pathlib.Path, exts: set[str], skip_vendored: bool = True):
    """Yield the files under root with one of exts, skipping build trees."""
    root = root.resolve()
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix not in exts:
            continue
        rel = path.relative_to(root).parts
        if is_skipped(rel, skip_vendored):
            continue
        yield path


def module_of(rel: pathlib.PurePath) -> str:
    """Return the module a path belongs to (its top level directory)."""
    parts = rel.parts
    return parts[0] if len(parts) > 1 else "(root)"


def is_test_path(rel: pathlib.PurePath) -> bool:
    """True for test/bench code, which is excluded from the coverage score."""
    parts = set(rel.parts)
    return bool(parts & {"tests", "test", "fuzz", "e2e", "posix_backend"})


def tool_path(name: str, extra: tuple[str, ...] = ()) -> str | None:
    """Locate an executable, looking in extra directories first."""
    for directory in extra:
        candidate = pathlib.Path(directory) / name
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return shutil.which(name)


def run(cmd, cwd=None, timeout=600, env=None, log=None, quiet=False):
    """Run cmd (list) with a hard timeout; return (returncode, output).

    The output is echoed unless quiet, appended to the log file when given,
    and a timeout is reported as return code 124 (the `timeout(1)` convention)
    so a caller can tell "slow" from "failed".
    """
    merged = dict(os.environ)
    if env:
        merged.update({k: str(v) for k, v in env.items()})
    try:
        done = subprocess.run(
            [str(part) for part in cmd],
            cwd=str(cwd) if cwd else None,
            env=merged,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        out = done.stdout + done.stderr
        code = done.returncode
    except subprocess.TimeoutExpired as exc:
        out = (exc.stdout or "") + (exc.stderr or "")
        if isinstance(out, bytes):
            out = out.decode("utf-8", "replace")
        out += "\n[timeout after %ss: %s]\n" % (timeout, " ".join(map(str, cmd)))
        code = 124
    if not quiet:
        print(out, end="" if out.endswith("\n") or not out else "\n")
    if log:
        with open(log, "a", encoding="utf-8") as handle:
            handle.write("$ %s\n%s" % (" ".join(map(str, cmd)), out))
    return code, out


def ensure_dir(path: pathlib.Path) -> pathlib.Path:
    """Create a directory (and its parents) and return it."""
    path.mkdir(parents=True, exist_ok=True)
    return path


def build_dir_for(source_dir: pathlib.Path, preset: str) -> pathlib.Path:
    """Return the build tree of a preset (build/<preset>, as CMakePresets.json)."""
    return pathlib.Path(source_dir) / "build" / preset


def configure_preset(cmake, preset, source_dir, timeout=300, log=None) -> int:
    """Run `cmake --preset <preset>`; return its exit code.

    Shared by the coverage and static-analysis scripts so that both configure
    a tree exactly the way CMakePresets.json declares it, including the
    compilation database that clang-tidy needs.
    """
    code, _ = run([cmake, "--preset", preset], cwd=source_dir, timeout=timeout,
                  log=log)
    return code


def parse_warnings(text: str, pattern: str) -> list:
    """Return the diagnostic lines of a tool output (stable, sorted, unique)."""
    rx = re.compile(pattern)
    hits = [line.strip() for line in text.splitlines() if rx.search(line)]
    return sorted(set(hits))

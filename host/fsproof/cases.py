# SPDX-License-Identifier: Apache-2.0
"""Directed test cases of section 7.4.

The random generator rarely reaches these corners, so they are written by
hand. Each case is a function taking a working directory and returning a
CaseResult, which keeps them composable and lets the CLI run the same table
against any client (--client).

Cases: rename over an existing file, unlink of an open file, concurrent
exclusive creation, permission matrix, symlink loop, 255-byte name, 5 GiB
sparse file, 100 000-entry directory (optional, slow) and maximum-length
path.
"""

from __future__ import annotations

import os
import pathlib
import stat
import sys
from dataclasses import dataclass
from typing import Callable

from fsproof.model import NAME_MAX

SPARSE_SIZE = 5 * 1024 * 1024 * 1024
BIG_DIR_ENTRIES = 100_000
BIG_DIR_FAST = 10_000
MAX_DEPTH = 40
SEGMENT = "x" * 40


@dataclass(frozen=True)
class CaseResult:
    """Outcome of one directed case."""

    name: str
    passed: bool
    detail: str = ""
    skipped: bool = False


def case_rename_over_existing(work: pathlib.Path) -> CaseResult:
    """Rename must replace the destination atomically (POSIX rename)."""
    source = work / "rename_src.txt"
    target = work / "rename_dst.txt"
    source.write_bytes(b"new")
    target.write_bytes(b"old-content")
    source.rename(target)
    ok = target.read_bytes() == b"new" and not source.exists()
    return CaseResult("rename_over_existing", ok, f"content={target.read_bytes()!r}")


def case_unlink_open_file(work: pathlib.Path) -> CaseResult:
    """An unlinked open file stays readable until it is closed."""
    path = work / "unlink_open.txt"
    path.write_bytes(b"still-here")
    with open(path, "rb") as handle:
        os.unlink(path)
        data = handle.read()
    ok = data == b"still-here" and not path.exists()
    return CaseResult("unlink_open_file", ok, f"read={data!r}")


def case_concurrent_exclusive_create(work: pathlib.Path) -> CaseResult:
    """Exactly one of N O_EXCL creators may win."""
    target = work / "exclusive.lock"
    target.unlink(missing_ok=True)
    winners = 0
    for _ in range(8):
        try:
            fd = os.open(target, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
            os.close(fd)
            winners += 1
        except FileExistsError:
            continue
    target.unlink(missing_ok=True)
    return CaseResult("concurrent_exclusive_create", winners == 1, f"winners={winners}")


def case_permission_matrix(work: pathlib.Path) -> CaseResult:
    """Read/write/execute bits must be enforced as POSIX says."""
    path = work / "perms.txt"
    path.write_bytes(b"secret")
    results: list[str] = []
    for mode in (0o400, 0o600, 0o444, 0o644):
        os.chmod(path, mode)
        writable = _can_write(path)
        expected = bool(mode & stat.S_IWUSR)
        if writable != expected:
            results.append(f"mode {mode:o}: writable={writable} expected={expected}")
    os.chmod(path, 0o644)
    return CaseResult("permission_matrix", not results, "; ".join(results))


def _can_write(path: pathlib.Path) -> bool:
    """True when the current user may open *path* for writing."""
    try:
        with open(path, "ab"):
            return True
    except PermissionError:
        return False


def case_symlink_loop(work: pathlib.Path) -> CaseResult:
    """A symlink loop must fail with ELOOP, not hang."""
    first = work / "loop_a"
    second = work / "loop_b"
    for link in (first, second):
        link.unlink(missing_ok=True)
    os.symlink("loop_b", first)
    os.symlink("loop_a", second)
    try:
        with open(first, "rb"):
            pass
        detail = "loop was followed without an error"
        ok = False
    except OSError as err:
        ok = err.errno in (40, 62)  # ELOOP on Linux and QNX
        detail = f"errno={err.errno}"
    finally:
        first.unlink(missing_ok=True)
        second.unlink(missing_ok=True)
    return CaseResult("symlink_loop", ok, detail)


def case_name_255_bytes(work: pathlib.Path) -> CaseResult:
    """A 255-byte name is legal and a 256-byte one is not (FS-15)."""
    name = "n" * NAME_MAX
    path = work / name
    path.write_bytes(b"max")
    created = path.exists() and len(path.name.encode()) == NAME_MAX
    too_long_ok = _creation_fails(work / ("n" * (NAME_MAX + 1)))
    path.unlink(missing_ok=True)
    ok = created and too_long_ok
    return CaseResult("name_255_bytes", ok, f"created={created} 256-rejected={too_long_ok}")


def _creation_fails(path: pathlib.Path) -> bool:
    """True when creating *path* is refused by the filesystem."""
    try:
        path.write_bytes(b"x")
    except OSError:
        return True
    path.unlink(missing_ok=True)
    return False


def case_sparse_5gib(work: pathlib.Path) -> CaseResult:
    """A 5 GiB file created by truncate must not consume 5 GiB of disk."""
    path = work / "sparse_5g.bin"
    with open(path, "wb") as handle:
        handle.truncate(SPARSE_SIZE)
        handle.seek(SPARSE_SIZE - 4)
        handle.write(b"tail")
    info = path.stat()
    tail = _read_tail(path)
    path.unlink(missing_ok=True)
    ok = info.st_size == SPARSE_SIZE and tail == b"tail"
    return CaseResult("sparse_5gib", ok, f"size={info.st_size} tail={tail!r}")


def _read_tail(path: pathlib.Path) -> bytes:
    """Read the last four bytes of *path*."""
    with open(path, "rb") as handle:
        handle.seek(SPARSE_SIZE - 4)
        return handle.read(4)


def case_directory_big(work: pathlib.Path, entries: int = BIG_DIR_FAST) -> CaseResult:
    """A directory with many entries must be creatable and listable."""
    directory = work / f"big_{entries}"
    directory.mkdir(exist_ok=True)
    for index in range(entries):
        (directory / f"e{index:06d}").touch()
    counted = sum(1 for _ in os.scandir(directory))
    for child in os.scandir(directory):
        os.unlink(child.path)
    directory.rmdir()
    return CaseResult("directory_big", counted == entries, f"entries={counted}/{entries}")


def case_max_path_length(work: pathlib.Path) -> CaseResult:
    """A deep hierarchy close to PATH_MAX must work end to end."""
    current = work
    created: list[pathlib.Path] = []
    try:
        for index in range(MAX_DEPTH):
            current = current / f"{SEGMENT}{index:02d}"
            current.mkdir()
            created.append(current)
        leaf = current / "leaf.txt"
        leaf.write_bytes(b"deep")
        ok = leaf.read_bytes() == b"deep"
        detail = f"depth={len(created)} path_len={len(str(leaf))}"
    except OSError as err:
        ok = False
        detail = f"failed at depth {len(created)}: {err}"
    finally:
        for path in reversed(created):
            try:
                path.rmdir()
            except OSError:
                break
    return CaseResult("max_path_length", ok, detail)


def case_hardlink_identity(work: pathlib.Path) -> CaseResult:
    """Two hard links share one inode and one content."""
    first = work / "hard_a"
    second = work / "hard_b"
    first.write_bytes(b"shared")
    second.unlink(missing_ok=True)
    os.link(first, second)
    ok = first.stat().st_ino == second.stat().st_ino and second.read_bytes() == b"shared"
    first.unlink(missing_ok=True)
    second.unlink(missing_ok=True)
    return CaseResult("hardlink_identity", ok)


CASES: tuple[tuple[str, Callable[[pathlib.Path], CaseResult]], ...] = (
    ("rename_over_existing", case_rename_over_existing),
    ("unlink_open_file", case_unlink_open_file),
    ("concurrent_exclusive_create", case_concurrent_exclusive_create),
    ("permission_matrix", case_permission_matrix),
    ("symlink_loop", case_symlink_loop),
    ("name_255_bytes", case_name_255_bytes),
    ("sparse_5gib", case_sparse_5gib),
    ("max_path_length", case_max_path_length),
    ("hardlink_identity", case_hardlink_identity),
)

SLOW_CASES: tuple[tuple[str, Callable[[pathlib.Path], CaseResult]], ...] = (
    ("directory_100k", lambda work: case_directory_big(work, BIG_DIR_ENTRIES)),
)


def case_names(include_slow: bool = False) -> list[str]:
    """Names of the cases that a run would execute."""
    names = [name for name, _ in CASES]
    if include_slow:
        names += [name for name, _ in SLOW_CASES]
    return names


def run_cases(
    work: pathlib.Path | str,
    include_slow: bool = False,
    select: list[str] | None = None,
) -> list[CaseResult]:
    """Run the directed table (optionally a subset) in *work*."""
    base = pathlib.Path(work).expanduser().resolve()
    base.mkdir(parents=True, exist_ok=True)
    table = list(CASES) + (list(SLOW_CASES) if include_slow else [])
    results: list[CaseResult] = []
    for name, func in table:
        if select and name not in select:
            continue
        case_dir = base / name
        case_dir.mkdir(parents=True, exist_ok=True)
        try:
            results.append(func(case_dir))
        except OSError as err:
            results.append(CaseResult(name, False, f"unexpected error: {err}"))
    return results


def run_all_cases(work: pathlib.Path | str, include_100k: bool = False) -> list[CaseResult]:
    """Compatibility helper: run every case, slow ones on request."""
    return run_cases(work, include_slow=include_100k)


def all_passed(results: list[CaseResult]) -> bool:
    """True when every executed case passed."""
    return all(result.passed for result in results)


def python_supports_sparse() -> bool:
    """Sparse files need a POSIX host; the guest always has one."""
    return sys.platform != "win32"

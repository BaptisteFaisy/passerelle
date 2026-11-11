# SPDX-License-Identifier: Apache-2.0
"""The six workloads of section 7.5 (PERF-03..PERF-06).

  throughput  sequential read then write of a large file, by block size
  metadata    10 000 files created, stat'ed and removed
  tree        walk of a 50 000-entry tree
  mixed       unpack a source archive, then search text in it
  exec        launch 1 000 programs from the share
  priority    a 4 KiB read during a massive low-priority copy (PERF-05)

Each workload measures wall time, the operation count and, where it makes
sense, a throughput in MiB/s, and returns a LoadResult that the report
aggregates. Nothing here needs a guest: the same code runs on the PC and
inside the guest, which is what makes the numbers comparable.
"""

from __future__ import annotations

import os
import pathlib
import shutil
import time
from dataclasses import dataclass, field

MIB = 1024 * 1024


@dataclass
class LoadResult:
    """Measurement of one workload run."""

    workload: str
    operations: int
    duration_s: float
    bytes_moved: int = 0
    detail: str = ""
    extra: dict[str, float] = field(default_factory=dict)

    @property
    def throughput_mib_s(self) -> float:
        """MiB per second, or 0 when the workload moves no bulk data."""
        if self.duration_s <= 0 or not self.bytes_moved:
            return 0.0
        return (self.bytes_moved / MIB) / self.duration_s

    @property
    def ops_per_second(self) -> float:
        """Operations per second."""
        return self.operations / self.duration_s if self.duration_s > 0 else 0.0


def _sync(handle: object) -> None:
    """Flush and fsync a file handle."""
    handle.flush()  # type: ignore[attr-defined]
    os.fsync(handle.fileno())  # type: ignore[attr-defined]


def run_throughput(
    target: pathlib.Path | str, size_mib: int = 64, block_kib: int = 128
) -> LoadResult:
    """Sequential write then read of *size_mib* MiB with *block_kib* blocks."""
    base = pathlib.Path(target)
    base.mkdir(parents=True, exist_ok=True)
    path = base / "throughput.bin"
    block = b"\xa5" * (block_kib * 1024)
    blocks = max(1, (size_mib * MIB) // len(block))

    started = time.monotonic()
    with open(path, "wb") as handle:
        for _ in range(blocks):
            handle.write(block)
        _sync(handle)
    write_time = time.monotonic() - started

    started = time.monotonic()
    read_bytes = 0
    with open(path, "rb") as handle:
        while chunk := handle.read(len(block)):
            read_bytes += len(chunk)
    read_time = time.monotonic() - started

    moved = read_bytes + blocks * len(block)
    path.unlink(missing_ok=True)
    total = max(1e-6, write_time + read_time)
    return LoadResult(
        workload="throughput",
        operations=blocks * 2,
        duration_s=total,
        bytes_moved=moved,
        detail=(
            f"{size_mib} MiB in {block_kib} KiB blocks "
            f"(write {write_time:.2f}s, read {read_time:.2f}s)"
        ),
        extra={"write_s": write_time, "read_s": read_time},
    )


def run_metadata(target: pathlib.Path | str, count: int = 10000) -> LoadResult:
    """Create, stat and remove *count* files in one directory."""
    base = pathlib.Path(target) / "metadata"
    base.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    paths = [base / f"f{index:06d}" for index in range(count)]
    for path in paths:
        path.write_bytes(b"x")
    create_time = time.monotonic() - started

    started = time.monotonic()
    for path in paths:
        path.stat()
    stat_time = time.monotonic() - started

    started = time.monotonic()
    for path in paths:
        path.unlink()
    unlink_time = time.monotonic() - started
    base.rmdir()

    total = create_time + stat_time + unlink_time
    return LoadResult(
        workload="metadata",
        operations=count * 3,
        duration_s=max(1e-6, total),
        detail=(
            f"{count} files: create {create_time:.2f}s, stat {stat_time:.2f}s, "
            f"unlink {unlink_time:.2f}s"
        ),
        extra={"create_s": create_time, "stat_s": stat_time, "unlink_s": unlink_time},
    )


def run_tree(target: pathlib.Path | str, entries: int = 50000, fanout: int = 50) -> LoadResult:
    """Walk a tree of *entries* entries (reading directories)."""
    base = pathlib.Path(target) / "tree"
    base.mkdir(parents=True, exist_ok=True)
    directories = max(1, entries // fanout)
    per_dir = max(1, entries // directories)
    started = time.monotonic()
    for index in range(directories):
        directory = base / f"d{index:04d}"
        directory.mkdir(exist_ok=True)
        for sub in range(per_dir):
            (directory / f"e{sub:04d}").touch()
    build_time = time.monotonic() - started

    started = time.monotonic()
    seen = 0
    stack = [base]
    while stack:
        current = stack.pop()
        with os.scandir(current) as iterator:
            for item in iterator:
                seen += 1
                if item.is_dir(follow_symlinks=False):
                    stack.append(pathlib.Path(item.path))
    walk_time = time.monotonic() - started

    shutil.rmtree(base, ignore_errors=True)
    return LoadResult(
        workload="tree",
        operations=seen,
        duration_s=max(1e-6, walk_time),
        detail=(
            f"{seen} entries in {directories} directories "
            f"(build {build_time:.2f}s, walk {walk_time:.2f}s)"
        ),
        extra={"build_s": build_time, "walk_s": walk_time},
    )


def run_mixed(target: pathlib.Path | str, files: int = 500) -> LoadResult:
    """Write a source-like tree, then search a pattern in it."""
    base = pathlib.Path(target) / "mixed"
    base.mkdir(parents=True, exist_ok=True)
    needle = b"PGW_NEEDLE"
    started = time.monotonic()
    moved = 0
    for index in range(files):
        payload = (b"int value_%d = %d;\n" % (index, index)) * 80
        if index % 10 == 0:
            payload += needle
        path = base / f"src_{index:04d}.c"
        path.write_bytes(payload)
        moved += len(payload)
    unpack_time = time.monotonic() - started

    started = time.monotonic()
    hits = 0
    for path in sorted(base.iterdir()):
        if needle in path.read_bytes():
            hits += 1
    search_time = time.monotonic() - started

    shutil.rmtree(base, ignore_errors=True)
    return LoadResult(
        workload="mixed",
        operations=files * 2,
        duration_s=max(1e-6, unpack_time + search_time),
        bytes_moved=moved,
        detail=(
            f"{files} files ({moved // 1024} KiB): unpack {unpack_time:.2f}s, "
            f"search {search_time:.2f}s, {hits} matches"
        ),
        extra={"unpack_s": unpack_time, "search_s": search_time},
    )


def run_exec(target: pathlib.Path | str, count: int = 1000) -> LoadResult:
    """Launch *count* small programs from the share."""
    import subprocess

    base = pathlib.Path(target)
    base.mkdir(parents=True, exist_ok=True)
    script = base / "exec_probe.sh"
    script.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    script.chmod(0o755)
    started = time.monotonic()
    for _ in range(count):
        subprocess.run([str(script)], check=False, stdout=subprocess.DEVNULL)
    elapsed = time.monotonic() - started
    script.unlink(missing_ok=True)
    return LoadResult(
        workload="exec",
        operations=count,
        duration_s=max(1e-6, elapsed),
        detail=f"{count} program launches from the share",
    )


def _copy_worker(source: pathlib.Path, destination: pathlib.Path, stop: list[bool]) -> int:
    """Copy *source* to *destination* until *stop* is set."""
    rounds = 0
    while not stop[0]:
        try:
            shutil.copyfile(source, destination)
        except OSError:
            break
        rounds += 1
    return rounds


def _time_probes(path: pathlib.Path, count: int) -> float:
    """Mean latency in seconds of *count* 4 KiB reads."""
    started = time.monotonic()
    for _ in range(count):
        with open(path, "rb") as handle:
            handle.read(4096)
    return (time.monotonic() - started) / max(1, count)


def run_priority(
    target: pathlib.Path | str, probes: int = 100, bulk_mib: int = 32
) -> LoadResult:
    """Time a small read while a bulk copy runs (PERF-05).

    The measurement is the ratio between the probe latency under load and
    its latency at rest; the requirement is at most twice the idle time.
    """
    import threading

    base = pathlib.Path(target)
    base.mkdir(parents=True, exist_ok=True)
    probe = base / "probe.bin"
    probe.write_bytes(b"\x5a" * 4096)
    source = base / "bulk_src.bin"
    destination = base / "bulk_dst.bin"
    source.write_bytes(b"\x33" * (bulk_mib * MIB))

    idle = _time_probes(probe, probes)
    stop = [False]
    worker = threading.Thread(target=_copy_worker, args=(source, destination, stop))
    worker.start()
    try:
        loaded = _time_probes(probe, probes)
    finally:
        stop[0] = True
        worker.join(timeout=30)
    for path in (probe, source, destination):
        path.unlink(missing_ok=True)

    ratio = loaded / idle if idle > 0 else 0.0
    return LoadResult(
        workload="priority",
        operations=probes * 2,
        duration_s=idle + loaded,
        detail=(
            f"4 KiB read: idle {idle * 1000:.3f} ms, under load {loaded * 1000:.3f} ms, "
            f"ratio {ratio:.2f}x"
        ),
        extra={"idle_ms": idle * 1000, "loaded_ms": loaded * 1000, "ratio": ratio},
    )


WORKLOADS = ("throughput", "metadata", "tree", "mixed", "exec", "priority")


def run_workload(name: str, target: pathlib.Path | str, scale: float = 1.0) -> LoadResult:
    """Run one workload by name, scaled by *scale* (1.0 = specification size)."""
    if name == "throughput":
        return run_throughput(target, size_mib=max(1, int(1024 * scale)))
    if name == "metadata":
        return run_metadata(target, count=max(10, int(10000 * scale)))
    if name == "tree":
        return run_tree(target, entries=max(100, int(50000 * scale)))
    if name == "mixed":
        return run_mixed(target, files=max(10, int(500 * scale)))
    if name == "exec":
        return run_exec(target, count=max(1, int(1000 * scale)))
    if name == "priority":
        return run_priority(target, probes=max(10, int(100 * scale)))
    raise ValueError(f"unknown workload '{name}'; choose from {', '.join(WORKLOADS)}")

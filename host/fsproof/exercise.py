# SPDX-License-Identifier: Apache-2.0
"""Content exerciser: write, read, truncate, extend, verify (7.3).

Two shapes are covered:

  * private files -- each worker owns its file and keeps the expected bytes
    in memory, so every read is checked against the truth and any silent
    corruption stops the run at the operation that caused it;
  * one shared file -- every worker appends blocks stamped with its own id
    and then verifies that all of its blocks are intact, which exercises
    concurrent appends without assuming anything about interleaving.

The exerciser is also the integrity check used by the demonstration.
"""

from __future__ import annotations

import os
import pathlib
import random
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field

DEFAULT_SEED = 2026
MAX_BLOCK = 65536
TAG_SIZE = 8


@dataclass
class ExpectedFile:
    """In-memory truth for one private file."""

    data: bytearray = field(default_factory=bytearray)

    def write_at(self, offset: int, chunk: bytes) -> None:
        """Record a write, zero-filling the hole it may create."""
        end = offset + len(chunk)
        if end > len(self.data):
            self.data.extend(b"\x00" * (end - len(self.data)))
        self.data[offset:end] = chunk

    def truncate(self, size: int) -> None:
        """Record a truncate, zero-padding when the file grows."""
        if size <= len(self.data):
            del self.data[size:]
        else:
            self.data.extend(b"\x00" * (size - len(self.data)))

    def read_at(self, offset: int, length: int) -> bytes:
        """Expected bytes of a read."""
        return bytes(self.data[offset : offset + length])


@dataclass
class ExerciseResult:
    """Outcome of one exerciser run."""

    success: bool
    ops_completed: int
    duration_s: float
    workers: int
    shared_file: bool
    mismatch: str = ""
    ops_per_second: float = 0.0


def _private_step(handle: object, expected: ExpectedFile, rng: random.Random, choice: str) -> None:
    """Apply one random step to a private file and its in-memory truth."""
    size = len(expected.data)
    if choice == "write":
        offset = rng.randint(0, size) if size else 0
        chunk = rng.randbytes(rng.randint(1, 4096))
        handle.seek(offset)  # type: ignore[attr-defined]
        handle.write(chunk)  # type: ignore[attr-defined]
        expected.write_at(offset, chunk)
    elif choice == "append":
        chunk = rng.randbytes(rng.randint(1, 4096))
        handle.seek(0, os.SEEK_END)  # type: ignore[attr-defined]
        handle.write(chunk)  # type: ignore[attr-defined]
        expected.write_at(size, chunk)
    elif choice == "truncate":
        target = rng.randint(0, size) if size else 0
        handle.truncate(target)  # type: ignore[attr-defined]
        expected.truncate(target)
    elif choice == "extend":
        target = size + rng.randint(1, 2048)
        handle.truncate(target)  # type: ignore[attr-defined]
        expected.truncate(target)


def _private_worker(path: pathlib.Path, ops: int, seed: int, failures: list[str]) -> int:
    """Run *ops* random operations on a private file, verifying every read."""
    rng = random.Random(seed)
    expected = ExpectedFile()
    completed = 0
    with open(path, "w+b") as handle:
        for _ in range(ops):
            choice = rng.choices(
                ["write", "append", "read", "truncate", "extend"],
                weights=[35, 20, 25, 10, 10],
                k=1,
            )[0]
            if choice == "read":
                size = len(expected.data)
                if size == 0:
                    completed += 1
                    continue
                offset = rng.randint(0, size - 1)
                length = min(size - offset, rng.randint(1, MAX_BLOCK))
                handle.seek(offset)
                actual = handle.read(length)
                wanted = expected.read_at(offset, length)
                if actual != wanted:
                    failures.append(
                        f"{path.name}: read {offset}+{length} differs "
                        f"(got {len(actual)}B, expected {len(wanted)}B)"
                    )
                    return completed
            else:
                _private_step(handle, expected, rng, choice)
            completed += 1
    return completed


def _block(worker: int, index: int, rng: random.Random) -> bytes:
    """Self-describing block: worker id, block index, payload."""
    tag = f"{worker:03d}{index:05d}".encode("ascii")
    return tag + rng.randbytes(TAG_SIZE)


def _shared_worker(path: pathlib.Path, worker: int, blocks: int, seed: int, failures: list[str]) -> int:
    """Append *blocks* stamped blocks, then verify every one of them."""
    rng = random.Random(seed)
    written: dict[bytes, bytes] = {}
    with open(path, "ab", buffering=0) as handle:
        for index in range(blocks):
            block = _block(worker, index, rng)
            written[block[:TAG_SIZE]] = block
            handle.write(block)
    with open(path, "rb") as handle:
        content = handle.read()
    for tag, block in written.items():
        if content.find(block) < 0:
            failures.append(f"{path.name}: block {tag.decode()} was lost or altered")
            return len(written)
    return len(written)


def run_exercise(
    target_dir: pathlib.Path | str,
    ops: int = 10000,
    workers: int = 1,
    shared_file: bool = False,
    seed: int = DEFAULT_SEED,
) -> ExerciseResult:
    """Exercise *ops* operations with *workers* concurrent workers."""
    base = pathlib.Path(target_dir).expanduser().resolve()
    base.mkdir(parents=True, exist_ok=True)
    workers = max(1, workers)
    per_worker = max(1, ops // workers)
    failures: list[str] = []
    started = time.monotonic()

    if shared_file:
        shared = base / "shared.dat"
        shared.write_bytes(b"")
        jobs = [
            (lambda index: _shared_worker(shared, index, per_worker, seed + index * 7919, failures))
        ]
    else:
        jobs = [
            (lambda index: _private_worker(
                base / f"worker_{index}.dat", per_worker, seed + index * 7919, failures
            ))
        ]

    with ThreadPoolExecutor(max_workers=workers) as pool:
        completed = sum(pool.map(jobs[0], range(workers)))

    duration = time.monotonic() - started
    return ExerciseResult(
        success=not failures,
        ops_completed=completed,
        duration_s=duration,
        workers=workers,
        shared_file=shared_file,
        mismatch=failures[0] if failures else "",
        ops_per_second=completed / duration if duration > 0 else 0.0,
    )


def exercise_plan(
    target_dir: pathlib.Path | str, ops: int = 10000, workers: int = 16
) -> list[ExerciseResult]:
    """Run the three shapes of section 7.3 in sequence."""
    return [
        run_exercise(target_dir, ops=ops, workers=1),
        run_exercise(target_dir, ops=ops, workers=workers),
        run_exercise(target_dir, ops=ops, workers=workers, shared_file=True),
    ]


def cleanup(target_dir: pathlib.Path | str) -> None:
    """Remove the files produced by an exerciser run."""
    base = pathlib.Path(target_dir)
    for pattern in ("worker_*.dat", "shared.dat"):
        for path in base.glob(pattern):
            try:
                os.unlink(path)
            except OSError:
                pass

# PERSIST-PROBE-1794357742885610460

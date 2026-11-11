# SPDX-License-Identifier: Apache-2.0
"""Operation model and directory snapshots for the differential test (7.2).

An `Action` is one filesystem operation that both the guest and the PC can
replay. A `Snapshot` is what a directory looks like from one side: names,
types, sizes, modes, timestamps, symlink targets and a SHA-256 of the
content. Comparing two snapshots is exactly the differential test of the
specification: the PC folder is the truth.
"""

from __future__ import annotations

import hashlib
import os
import pathlib
import stat
from dataclasses import dataclass, field
from typing import Any, Iterable, Mapping

# Operation names understood by the guest helper and by the local replayer.
OPS = (
    "mkdir",
    "rmdir",
    "create",
    "write",
    "append",
    "truncate",
    "read",
    "rename",
    "link",
    "symlink",
    "unlink",
    "chmod",
    "utime",
    "listdir",
    "stat",
)

NAME_MAX = 255
PATH_MAX = 4096


@dataclass(frozen=True)
class Action:
    """One replayable filesystem operation."""

    op: str
    path: str
    arg: str = ""
    data: bytes = b""
    mode: int = 0o644
    offset: int = 0

    def to_json(self) -> dict[str, Any]:
        """Serialise the action to a JSON-compatible dict."""
        payload: dict[str, Any] = {"op": self.op, "path": self.path}
        if self.arg:
            payload["arg"] = self.arg
        if self.data:
            payload["data"] = self.data.hex()
        if self.op == "chmod":
            payload["mode"] = self.mode
        if self.op in ("write", "append", "read", "truncate"):
            payload["offset"] = self.offset
        return payload

    @classmethod
    def from_json(cls, payload: Mapping[str, Any]) -> Action:
        """Rebuild an action from its JSON form."""
        return cls(
            op=str(payload["op"]),
            path=str(payload["path"]),
            arg=str(payload.get("arg", "")),
            data=bytes.fromhex(payload.get("data", "")),
            mode=int(payload.get("mode", 0o644)),
            offset=int(payload.get("offset", 0)),
        )

    def line(self) -> str:
        """One-line textual form used in reports and regressions."""
        extra = f" {self.arg}" if self.arg else ""
        size = f" [{len(self.data)}B]" if self.data else ""
        return f"{self.op} {self.path}{extra}{size}"


@dataclass(frozen=True)
class Entry:
    """One directory entry as seen by one side of the test."""

    path: str
    kind: str
    size: int
    mode: int
    mtime_ns: int
    sha256: str = ""
    target: str = ""

    def comparable(self) -> tuple[Any, ...]:
        """Fields that must match between guest and host."""
        return (self.kind, self.size, self.mode, self.mtime_ns, self.sha256, self.target)


@dataclass
class Snapshot:
    """All entries of a tree, indexed by relative path."""

    entries: dict[str, Entry] = field(default_factory=dict)

    def add(self, entry: Entry) -> None:
        """Insert one entry."""
        self.entries[entry.path] = entry

    def paths(self) -> list[str]:
        """Sorted relative paths."""
        return sorted(self.entries)

    def __len__(self) -> int:
        return len(self.entries)


def kind_of(mode: int) -> str:
    """Map a stat mode to a short kind name."""
    if stat.S_ISDIR(mode):
        return "dir"
    if stat.S_ISLNK(mode):
        return "link"
    if stat.S_ISREG(mode):
        return "file"
    if stat.S_ISFIFO(mode):
        return "fifo"
    if stat.S_ISSOCK(mode):
        return "socket"
    if stat.S_ISCHR(mode):
        return "char"
    if stat.S_ISBLK(mode):
        return "block"
    return "other"


def hash_bytes(chunk: bytes) -> str:
    """SHA-256 of a byte string."""
    return hashlib.sha256(chunk).hexdigest()


def hash_file(path: pathlib.Path, limit: int | None = None) -> str:
    """SHA-256 of a file's content, optionally truncated to *limit* bytes."""
    digest = hashlib.sha256()
    remaining = limit
    with open(path, "rb") as handle:
        while True:
            want = 1 << 16 if remaining is None else min(1 << 16, remaining)
            if want <= 0:
                break
            block = handle.read(want)
            if not block:
                break
            digest.update(block)
            if remaining is not None:
                remaining -= len(block)
    return digest.hexdigest()


def entry_from_path(root: pathlib.Path, rel: str, hash_limit: int | None = 1 << 20) -> Entry:
    """Build an Entry for *rel* under *root*, never following symlinks."""
    full = root / rel
    info = full.lstat()
    kind = kind_of(info.st_mode)
    digest = ""
    target = ""
    if kind == "link":
        target = os.readlink(full)
    elif kind == "file":
        digest = hash_file(full, limit=hash_limit)
    return Entry(
        path=rel,
        kind=kind,
        size=info.st_size,
        mode=stat.S_IMODE(info.st_mode),
        mtime_ns=info.st_mtime_ns,
        sha256=digest,
        target=target,
    )


def scan_tree(
    root: pathlib.Path, skip: Iterable[str] = (), hash_limit: int | None = 1 << 20
) -> Snapshot:
    """Walk *root* without following links and snapshot every entry."""
    skip_set = set(skip)
    snapshot = Snapshot()
    stack = [""]
    while stack:
        current = stack.pop()
        directory = root / current if current else root
        try:
            with os.scandir(directory) as iterator:
                names = sorted(entry.name for entry in iterator)
        except OSError:
            continue
        for name in names:
            rel = f"{current}/{name}" if current else name
            if rel in skip_set:
                continue
            try:
                entry = entry_from_path(root, rel, hash_limit=hash_limit)
            except OSError:
                continue
            snapshot.add(entry)
            if entry.kind == "dir":
                stack.append(rel)
    return snapshot

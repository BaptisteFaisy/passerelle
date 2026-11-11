# SPDX-License-Identifier: Apache-2.0
"""Local replayer for operation sequences (7.2).

The same sequence is replayed by the guest helper and by this module, so
that the two trees can be compared. Failures are recorded, never fatal:
a divergence in *which* operations succeed is itself a finding.
"""

from __future__ import annotations

import os
import pathlib
from dataclasses import dataclass

from fsproof.model import Action


@dataclass(frozen=True)
class StepResult:
    """Outcome of one replayed operation."""

    index: int
    op: str
    path: str
    ok: bool
    detail: str = ""


def _resolve(root: pathlib.Path, rel: str) -> pathlib.Path:
    """Resolve *rel* under *root*, refusing to escape the tree."""
    candidate = (root / rel).resolve() if rel not in (".", "") else root
    root_resolved = root.resolve()
    if candidate != root_resolved and root_resolved not in candidate.parents:
        raise ValueError(f"path escapes the share: {rel}")
    return candidate


def _apply(root: pathlib.Path, action: Action) -> str:
    """Apply one action, returning a short detail string."""
    path = _resolve(root, action.path)
    handler = _HANDLERS.get(action.op)
    if handler is None:
        raise ValueError(f"unsupported operation: {action.op}")
    return handler(root, path, action)


def _op_mkdir(root: pathlib.Path, path: pathlib.Path, action: Action) -> str:
    """Create a directory (parents included) and set its mode."""
    path.mkdir(parents=True, exist_ok=True)
    os.chmod(path, action.mode)
    return "created"


def _op_rmdir(root: pathlib.Path, path: pathlib.Path, action: Action) -> str:
    """Remove an empty directory."""
    path.rmdir()
    return "removed"


def _op_create(root: pathlib.Path, path: pathlib.Path, action: Action) -> str:
    """Create an empty file and set its mode."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb"):
        pass
    os.chmod(path, action.mode)
    return "created"


def _op_write(root: pathlib.Path, path: pathlib.Path, action: Action) -> str:
    """Replace the file content."""
    with open(path, "wb") as handle:
        handle.write(action.data)
    return f"wrote {len(action.data)}B"


def _op_append(root: pathlib.Path, path: pathlib.Path, action: Action) -> str:
    """Append to the file content."""
    with open(path, "ab") as handle:
        handle.write(action.data)
    return f"appended {len(action.data)}B"


def _op_truncate(root: pathlib.Path, path: pathlib.Path, action: Action) -> str:
    """Truncate the file to *offset* bytes."""
    with open(path, "r+b") as handle:
        handle.truncate(action.offset)
    return f"truncated to {action.offset}B"


def _op_rename(root: pathlib.Path, path: pathlib.Path, action: Action) -> str:
    """Rename (or move) the entry."""
    path.rename(_resolve(root, action.arg))
    return f"renamed to {action.arg}"


def _op_link(root: pathlib.Path, path: pathlib.Path, action: Action) -> str:
    """Create a hard link to the entry."""
    os.link(path, _resolve(root, action.arg))
    return f"linked as {action.arg}"


def _op_symlink(root: pathlib.Path, path: pathlib.Path, action: Action) -> str:
    """Create a symbolic link pointing at *arg*."""
    os.symlink(action.arg, path)
    return f"symlink to {action.arg}"


def _op_unlink(root: pathlib.Path, path: pathlib.Path, action: Action) -> str:
    """Remove a file or a symbolic link."""
    path.unlink()
    return "unlinked"


def _op_chmod(root: pathlib.Path, path: pathlib.Path, action: Action) -> str:
    """Change the permission bits."""
    os.chmod(path, action.mode)
    return f"mode {action.mode:o}"


def _op_stat(root: pathlib.Path, path: pathlib.Path, action: Action) -> str:
    """Read the attributes without following a link."""
    return f"size {path.lstat().st_size}"


def _op_listdir(root: pathlib.Path, path: pathlib.Path, action: Action) -> str:
    """Count the directory entries."""
    return f"{len(list(os.scandir(path)))} entries"


def _op_read(root: pathlib.Path, path: pathlib.Path, action: Action) -> str:
    """Read the whole file."""
    return f"read {len(path.read_bytes())}B"


_HANDLERS = {
    "mkdir": _op_mkdir,
    "rmdir": _op_rmdir,
    "create": _op_create,
    "write": _op_write,
    "append": _op_append,
    "truncate": _op_truncate,
    "rename": _op_rename,
    "link": _op_link,
    "symlink": _op_symlink,
    "unlink": _op_unlink,
    "chmod": _op_chmod,
    "stat": _op_stat,
    "listdir": _op_listdir,
    "read": _op_read,
}


def replay(root: pathlib.Path, actions: list[Action], stop_on_error: bool = False) -> list[StepResult]:
    """Replay *actions* under *root* and return one result per step."""
    results: list[StepResult] = []
    for index, action in enumerate(actions):
        try:
            detail = _apply(root, action)
            results.append(StepResult(index, action.op, action.path, True, detail))
        except (OSError, ValueError) as err:
            results.append(StepResult(index, action.op, action.path, False, str(err)))
            if stop_on_error:
                break
    return results


def failures(results: list[StepResult]) -> list[StepResult]:
    """Return only the failed steps."""
    return [step for step in results if not step.ok]

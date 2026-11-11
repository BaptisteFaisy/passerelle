# SPDX-License-Identifier: Apache-2.0
"""Reference guest agent for the differential test (7.2, 7.4).

This file is meant to be run *inside* the guest, from the share, by
`fsproof diff`. It reads a JSON operation sequence, replays it in a work
directory, and writes back a JSON snapshot of what the guest sees.

It only uses the Python standard library, so any guest that ships a
python3 interpreter can act as the reference implementation; a future
native version can replace it without changing the protocol:

    agent.py --work <dir> --sequence <file> --snapshot <file>

Protocol (both files are UTF-8 JSON):
  sequence: {"actions": [{"op": ..., "path": ..., ...}, ...]}
  snapshot: {"entries": [{"path": ..., "kind": ..., "size": ...,
                          "mode": ..., "mtime_ns": ..., "sha256": ...,
                          "target": ...}, ...], "errors": [...]}
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import stat
import sys

NAME_MAX = 255


def kind_of(mode: int) -> str:
    """Short kind name for a stat mode (same vocabulary as the host side)."""
    for test, name in (
        (stat.S_ISDIR, "dir"),
        (stat.S_ISLNK, "link"),
        (stat.S_ISREG, "file"),
        (stat.S_ISFIFO, "fifo"),
        (stat.S_ISSOCK, "socket"),
        (stat.S_ISCHR, "char"),
        (stat.S_ISBLK, "block"),
    ):
        if test(mode):
            return name
    return "other"


def hash_file(path: pathlib.Path, limit: int | None) -> str:
    """SHA-256 of a file, optionally over its first *limit* bytes."""
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


def snapshot(root: pathlib.Path, hash_limit: int | None) -> dict[str, object]:
    """Snapshot the tree under *root* without following symlinks."""
    entries: list[dict[str, object]] = []
    errors: list[str] = []
    stack = [""]
    while stack:
        current = stack.pop()
        directory = root / current if current else root
        try:
            names = sorted(os.listdir(directory))
        except OSError as err:
            errors.append(f"listdir {current or '.'}: {err}")
            continue
        for name in names:
            rel = f"{current}/{name}" if current else name
            full = root / rel
            try:
                info = full.lstat()
            except OSError as err:
                errors.append(f"lstat {rel}: {err}")
                continue
            kind = kind_of(info.st_mode)
            target = os.readlink(full) if kind == "link" else ""
            digest = hash_file(full, hash_limit) if kind == "file" else ""
            entries.append(
                {
                    "path": rel,
                    "kind": kind,
                    "size": info.st_size,
                    "mode": stat.S_IMODE(info.st_mode),
                    "mtime_ns": info.st_mtime_ns,
                    "sha256": digest,
                    "target": target,
                }
            )
            if kind == "dir":
                stack.append(rel)
    return {"entries": entries, "errors": errors}


def resolve(root: pathlib.Path, rel: str) -> pathlib.Path:
    """Resolve *rel* under *root*, refusing to escape it."""
    root_resolved = root.resolve()
    candidate = (root / rel).resolve() if rel not in (".", "") else root_resolved
    if candidate != root_resolved and root_resolved not in candidate.parents:
        raise ValueError(f"path escapes the work directory: {rel}")
    return candidate


def apply_action(root: pathlib.Path, action: dict[str, object]) -> str:
    """Replay one action and return a short detail string."""
    op = str(action["op"])
    path = resolve(root, str(action["path"]))
    handler = _HANDLERS.get(op)
    if handler is None:
        raise ValueError(f"unsupported operation: {op}")
    handler(root, path, action)
    return op


def _args(action: dict[str, object]) -> tuple[bytes, int, int, str]:
    """Decode the common action arguments once."""
    return (
        bytes.fromhex(str(action.get("data", ""))),
        int(action.get("mode", 0o644)),
        int(action.get("offset", 0)),
        str(action.get("arg", "")),
    )


def _do_mkdir(root: pathlib.Path, path: pathlib.Path, action: dict[str, object]) -> None:
    """Create a directory and set its mode."""
    path.mkdir(parents=True, exist_ok=True)
    os.chmod(path, int(action.get("mode", 0o755)))


def _do_rmdir(root: pathlib.Path, path: pathlib.Path, action: dict[str, object]) -> None:
    """Remove an empty directory."""
    path.rmdir()


def _do_create(root: pathlib.Path, path: pathlib.Path, action: dict[str, object]) -> None:
    """Create an empty file and set its mode."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb"):
        pass
    os.chmod(path, int(action.get("mode", 0o644)))


def _do_write(root: pathlib.Path, path: pathlib.Path, action: dict[str, object]) -> None:
    """Replace the content of a file."""
    data, _, _, _ = _args(action)
    with open(path, "wb") as handle:
        handle.write(data)


def _do_append(root: pathlib.Path, path: pathlib.Path, action: dict[str, object]) -> None:
    """Append to the content of a file."""
    data, _, _, _ = _args(action)
    with open(path, "ab") as handle:
        handle.write(data)


def _do_truncate(root: pathlib.Path, path: pathlib.Path, action: dict[str, object]) -> None:
    """Truncate a file to the requested length."""
    _, _, offset, _ = _args(action)
    with open(path, "r+b") as handle:
        handle.truncate(offset)


def _do_rename(root: pathlib.Path, path: pathlib.Path, action: dict[str, object]) -> None:
    """Rename an entry."""
    _, _, _, arg = _args(action)
    path.rename(resolve(root, arg))


def _do_link(root: pathlib.Path, path: pathlib.Path, action: dict[str, object]) -> None:
    """Create a hard link."""
    _, _, _, arg = _args(action)
    os.link(path, resolve(root, arg))


def _do_symlink(root: pathlib.Path, path: pathlib.Path, action: dict[str, object]) -> None:
    """Create a symbolic link."""
    _, _, _, arg = _args(action)
    os.symlink(arg, path)


def _do_unlink(root: pathlib.Path, path: pathlib.Path, action: dict[str, object]) -> None:
    """Remove a file or a link."""
    path.unlink()


def _do_chmod(root: pathlib.Path, path: pathlib.Path, action: dict[str, object]) -> None:
    """Change permission bits."""
    os.chmod(path, int(action.get("mode", 0o644)))


def _do_stat(root: pathlib.Path, path: pathlib.Path, action: dict[str, object]) -> None:
    """Read attributes."""
    path.lstat()


def _do_listdir(root: pathlib.Path, path: pathlib.Path, action: dict[str, object]) -> None:
    """Read a directory."""
    os.listdir(path)


def _do_read(root: pathlib.Path, path: pathlib.Path, action: dict[str, object]) -> None:
    """Read a file."""
    with open(path, "rb") as handle:
        handle.read()


_HANDLERS = {
    "mkdir": _do_mkdir,
    "rmdir": _do_rmdir,
    "create": _do_create,
    "write": _do_write,
    "append": _do_append,
    "truncate": _do_truncate,
    "rename": _do_rename,
    "link": _do_link,
    "symlink": _do_symlink,
    "unlink": _do_unlink,
    "chmod": _do_chmod,
    "stat": _do_stat,
    "listdir": _do_listdir,
    "read": _do_read,
}


def replay_all(root: pathlib.Path, actions: list[dict[str, object]]) -> list[str]:
    """Replay every action, collecting the errors instead of raising."""
    errors: list[str] = []
    for index, action in enumerate(actions):
        try:
            apply_action(root, action)
        except (OSError, ValueError) as err:
            errors.append(f"step {index} {action.get('op')} {action.get('path')}: {err}")
    return errors


def main(argv: list[str] | None = None) -> int:
    """Entry point of the guest agent."""
    parser = argparse.ArgumentParser(prog="guest_agent", description=__doc__)
    parser.add_argument("--work", required=True, help="work directory to replay into")
    parser.add_argument("--sequence", required=True, help="JSON sequence to replay")
    parser.add_argument("--snapshot", required=True, help="JSON snapshot to write")
    parser.add_argument("--hash-limit", type=int, default=1 << 20, help="bytes hashed per file")
    args = parser.parse_args(argv)

    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    payload = json.loads(pathlib.Path(args.sequence).read_text(encoding="utf-8"))
    actions = payload.get("actions", [])
    errors = replay_all(work, actions)
    result = snapshot(work, args.hash_limit)
    result["errors"] = list(result["errors"]) + errors
    pathlib.Path(args.snapshot).write_text(json.dumps(result, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())

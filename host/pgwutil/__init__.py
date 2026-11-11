# SPDX-License-Identifier: Apache-2.0
"""Shared helpers for the Passerelle host tools (standard library only).

Covers DX-01, DX-02, DX-03, VFS-06, PERF-01..PERF-07.
Subprocess invocation, atomic writes, JSON serialisation and the small
error hierarchy used by every command line entry point.
"""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
from typing import Any, Mapping, Sequence


class HostError(Exception):
    """Base class for every error raised by the host tools."""


class ValidationError(HostError):
    """Raised when user input (path, tag, model) is rejected."""


def ensure_dir(path: pathlib.Path | str) -> pathlib.Path:
    """Create *path* (and parents) if needed and return it resolved."""
    resolved = pathlib.Path(path).expanduser().resolve()
    resolved.mkdir(parents=True, exist_ok=True)
    return resolved


def write_atomic(path: pathlib.Path | str, content: str | bytes) -> pathlib.Path:
    """Write *content* to *path* through a temporary file, then rename."""
    target = pathlib.Path(path).expanduser().resolve()
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp = target.with_name(f".{target.name}.tmp.{os.getpid()}")
    binary = isinstance(content, bytes)
    try:
        with open(tmp, "wb" if binary else "w", encoding=None if binary else "utf-8") as handle:
            handle.write(content)  # type: ignore[arg-type]
        tmp.replace(target)
    except OSError:
        tmp.unlink(missing_ok=True)
        raise
    return target


def dump_json(obj: Any, indent: int = 2) -> str:
    """Serialise *obj* to a stable, human-readable JSON document."""
    return json.dumps(obj, indent=indent, sort_keys=True, default=str)


def load_json(path: pathlib.Path | str) -> Any:
    """Read a JSON document from *path*."""
    return json.loads(pathlib.Path(path).expanduser().read_text(encoding="utf-8"))


def run_cmd(
    cmd: Sequence[str],
    cwd: pathlib.Path | str | None = None,
    timeout: float | None = None,
    env: Mapping[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run *cmd* capturing text output; never raises on a non-zero status."""
    merged = os.environ.copy()
    if env:
        merged.update(env)
    return subprocess.run(
        list(cmd),
        cwd=None if cwd is None else str(cwd),
        timeout=timeout,
        capture_output=True,
        text=True,
        check=False,
        env=merged,
    )


def which(program: str, extra: Sequence[str] = ()) -> pathlib.Path | None:
    """Locate *program* on PATH, then among *extra* absolute candidates."""
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        if not directory:
            continue
        candidate = pathlib.Path(directory) / program
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    for candidate_str in extra:
        candidate = pathlib.Path(candidate_str)
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None

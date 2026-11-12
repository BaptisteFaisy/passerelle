# SPDX-License-Identifier: Apache-2.0
"""The guest-side entry point of fsproof, exercised on the PC (gap 97).

`host/fsproof/agent.sh` is what the bench asks the guest to run. It cannot be
executed inside a QNX guest here - no QNX guest runs on this machine, and QNX
Neutrino ships no Python interpreter - but the interface itself is testable:
the script runs on this PC with the real Python agent, it forwards the three
options the differential test appends, it honours PGW_AGENT_BIN for a future
native agent, and it fails loudly with a diagnosis instead of failing silently
when the guest has nothing to run the bench with.
"""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys

from fsproof.model import Action
from pgwutil import write_atomic

AGENT = pathlib.Path(__file__).resolve().parents[1] / "fsproof" / "agent.sh"
PYTHON = sys.executable
SHELL = "/bin/sh"


def _sequence(tmp_path: pathlib.Path, actions: list[Action]) -> pathlib.Path:
    sequence = tmp_path / "sequence.json"
    write_atomic(sequence, json.dumps({"actions": [action.to_json() for action in actions]}))
    return sequence


def _run(env_extra: dict[str, str], *args: str, cwd: pathlib.Path | None = None):
    env = {k: v for k, v in os.environ.items() if k != "PGW_AGENT_BIN"}
    env.update(env_extra)
    return subprocess.run(
        [SHELL, str(AGENT), *args],
        env=env,
        capture_output=True,
        text=True,
        timeout=60,
        cwd=str(cwd) if cwd else None,
    )


def test_agent_sh_runs_the_python_agent(tmp_path: pathlib.Path) -> None:
    """The whole guest protocol, through the shell entry point."""
    work = tmp_path / "work"
    snapshot = tmp_path / "snapshot.json"
    sequence = _sequence(
        tmp_path,
        [
            Action(op="mkdir", path="d", mode=0o755),
            Action(op="create", path="d/f", mode=0o644),
            Action(op="write", path="d/f", data=b"payload"),
            Action(op="symlink", path="d/l", arg="f"),
        ],
    )
    proc = _run(
        {"PGW_PYTHON": PYTHON},
        "--work", str(work),
        "--sequence", str(sequence),
        "--snapshot", str(snapshot),
    )
    assert proc.returncode == 0, proc.stderr
    payload = json.loads(snapshot.read_text(encoding="utf-8"))
    assert payload["errors"] == []
    by_path = {entry["path"]: entry for entry in payload["entries"]}
    assert by_path["d"]["kind"] == "dir"
    assert by_path["d/f"]["size"] == len(b"payload")
    assert by_path["d/l"]["target"] == "f"


def test_agent_sh_is_found_from_any_directory(tmp_path: pathlib.Path) -> None:
    """The guest runs it from the share, whatever the working directory is."""
    work = tmp_path / "elsewhere" / "work"
    snapshot = tmp_path / "snap.json"
    sequence = _sequence(tmp_path, [Action(op="create", path="a", mode=0o644)])
    proc = _run(
        {"PGW_PYTHON": PYTHON},
        "--work", str(work),
        "--sequence", str(sequence),
        "--snapshot", str(snapshot),
        cwd=tmp_path,
    )
    assert proc.returncode == 0, proc.stderr
    assert snapshot.is_file()


def test_agent_sh_prefers_a_native_agent(tmp_path: pathlib.Path) -> None:
    """PGW_AGENT_BIN is the slot a native agent built for the guest takes."""
    stub = tmp_path / "native_agent"
    stub.write_text('#!/bin/sh\necho native-agent "$@"\n', encoding="utf-8")
    stub.chmod(0o755)
    proc = _run({"PGW_AGENT_BIN": str(stub)}, "--work", "w", "--sequence", "s", "--snapshot", "n")
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout.startswith("native-agent --work w")


def test_agent_sh_says_what_is_missing_when_it_cannot_run(tmp_path: pathlib.Path) -> None:
    """No interpreter, no native agent: a diagnosis, not a silent failure."""
    proc = _run(
        {"PATH": str(tmp_path / "nothing")},
        "--work", "w", "--sequence", "s", "--snapshot", "n",
    )
    assert proc.returncode == 3
    assert "guest_agent.py" in proc.stderr
    assert "Python 3" in proc.stderr
    assert "PGW_AGENT_BIN" in proc.stderr
    assert not (tmp_path / "n").exists()

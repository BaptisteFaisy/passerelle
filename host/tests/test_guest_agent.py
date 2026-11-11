# SPDX-License-Identifier: Apache-2.0
"""End-to-end test of the guest protocol (7.2).

The guest agent is a real program that replays a JSON sequence inside the
share. Running it through a subprocess exercises the whole protocol --
sequence file, snapshot file, error collection -- without needing QEMU.
A deliberately lossy agent proves that the bench catches a client that
silently forgets operations.
"""

from __future__ import annotations

import json
import pathlib
import sys

import pytest

from fsproof.diff import GuestReplayer, run_diff
from fsproof.gen import generate_sequence
from fsproof.model import Action
from pgwutil import run_cmd, write_atomic

AGENT = pathlib.Path(__file__).resolve().parents[1] / "fsproof" / "guest_agent.py"
PYTHON = sys.executable


def test_agent_replays_and_snapshots(tmp_path: pathlib.Path) -> None:
    work = tmp_path / "work"
    sequence = tmp_path / "seq.json"
    snapshot = tmp_path / "snap.json"
    actions = [
        Action(op="mkdir", path="d", mode=0o755),
        Action(op="create", path="d/f", mode=0o644),
        Action(op="write", path="d/f", data=b"payload"),
        Action(op="symlink", path="d/l", arg="f"),
    ]
    write_atomic(sequence, json.dumps({"actions": [a.to_json() for a in actions]}))
    proc = run_cmd([
        PYTHON, str(AGENT),
        "--work", str(work),
        "--sequence", str(sequence),
        "--snapshot", str(snapshot),
    ], timeout=60)
    assert proc.returncode == 0, proc.stderr
    payload = json.loads(snapshot.read_text(encoding="utf-8"))
    paths = {entry["path"]: entry for entry in payload["entries"]}
    assert paths["d"]["kind"] == "dir"
    assert paths["d/f"]["kind"] == "file"
    assert paths["d/f"]["size"] == 7
    assert paths["d/l"]["target"] == "f"
    assert payload["errors"] == []


def test_agent_reports_errors_instead_of_dying(tmp_path: pathlib.Path) -> None:
    work = tmp_path / "work"
    sequence = tmp_path / "seq.json"
    snapshot = tmp_path / "snap.json"
    actions = [
        Action(op="unlink", path="absent"),
        Action(op="create", path="kept", mode=0o644),
    ]
    write_atomic(sequence, json.dumps({"actions": [a.to_json() for a in actions]}))
    proc = run_cmd([
        PYTHON, str(AGENT),
        "--work", str(work),
        "--sequence", str(sequence),
        "--snapshot", str(snapshot),
    ], timeout=60)
    assert proc.returncode == 0
    payload = json.loads(snapshot.read_text(encoding="utf-8"))
    assert len(payload["errors"]) == 1
    assert "step 0" in payload["errors"][0]
    assert any(entry["path"] == "kept" for entry in payload["entries"])


def test_guest_mode_diff_passes(tmp_path: pathlib.Path) -> None:
    """The full guest path: agent replays in the share, PC reads the same tree."""
    result = run_diff(
        seed=21,
        count=40,
        mode="guest",
        guest_command=[PYTHON, str(AGENT)],
        reg_dir=tmp_path / "regs",
        base=tmp_path / "work",
    )
    assert result.success, result.mismatches[:5]
    assert result.mode == "guest"
    assert result.errors == [] or all("step" in item for item in result.errors)


def test_guest_mode_detects_a_lossy_client(tmp_path: pathlib.Path) -> None:
    """A client that drops writes must be caught and reduced."""
    lossy = tmp_path / "lossy_agent.py"
    write_atomic(lossy, LOSSY_AGENT.replace("@HOST_ROOT@", str(AGENT.parents[1])))
    actions = [
        Action(op="create", path="a", mode=0o644),
        Action(op="write", path="a", data=b"hello"),
        Action(op="create", path="b", mode=0o644),
        Action(op="write", path="b", data=b"world"),
    ]
    result = run_diff(
        seed=5,
        count=len(actions),
        mode="guest",
        guest_command=[PYTHON, str(lossy)],
        reg_dir=tmp_path / "regs",
        base=tmp_path / "work",
        actions=actions,
    )
    assert not result.success
    assert any("size differs" in item or "content hash differs" in item for item in result.mismatches)
    assert result.reduced is not None and len(result.reduced) < len(actions)
    assert result.regression is not None and result.regression.is_file()


def test_replayer_requires_a_command(tmp_path: pathlib.Path) -> None:
    from pgwutil import HostError

    with pytest.raises(HostError, match="empty"):
        GuestReplayer([])


def test_generated_sequence_is_replayable_by_the_agent(tmp_path: pathlib.Path) -> None:
    actions = generate_sequence(seed=99, count=30)
    result = run_diff(
        seed=99,
        count=len(actions),
        mode="guest",
        guest_command=[PYTHON, str(AGENT)],
        reg_dir=tmp_path / "regs",
        base=tmp_path / "work",
        actions=actions,
    )
    assert result.success, result.mismatches[:5]


LOSSY_AGENT = '''# SPDX-License-Identifier: Apache-2.0
"""Test double: a guest agent that silently drops every write."""
import argparse
import json
import pathlib
import sys

sys.path.insert(0, "@HOST_ROOT@")
from fsproof.guest_agent import replay_all, snapshot  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", required=True)
    parser.add_argument("--sequence", required=True)
    parser.add_argument("--snapshot", required=True)
    parser.add_argument("--hash-limit", type=int, default=1 << 20)
    args = parser.parse_args()
    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    payload = json.loads(pathlib.Path(args.sequence).read_text(encoding="utf-8"))
    actions = [a for a in payload["actions"] if a["op"] != "write"]
    errors = replay_all(work, actions)
    result = snapshot(work, args.hash_limit)
    result["errors"] = list(result["errors"]) + errors
    pathlib.Path(args.snapshot).write_text(json.dumps(result), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
'''

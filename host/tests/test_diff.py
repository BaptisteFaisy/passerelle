# SPDX-License-Identifier: Apache-2.0
"""Tests for the differential bench: snapshots, replay, reduction (7.2).

The bench must catch a divergence, reduce it to a short sequence, and store
it as a regression. Those three behaviours are tested against a deliberately
broken replayer, because a bench that only ever says PASS proves nothing.
"""

from __future__ import annotations

import pathlib

import pytest

from fsproof.diff import (
    Divergence,
    LocalReplayer,
    compare,
    load_regression,
    reduce_sequence,
    run_diff,
    run_once,
    save_regression,
)
from fsproof.gen import SequenceGenerator, generate_sequence, long_name_actions
from fsproof.model import Action, Entry, Snapshot, hash_file, scan_tree
from fsproof.replay import replay


def test_scan_tree_captures_every_field(share: pathlib.Path) -> None:
    (share / "dir").mkdir()
    (share / "dir" / "file.txt").write_bytes(b"hello")
    (share / "link").symlink_to("dir/file.txt")
    (share / "dir" / "file.txt").chmod(0o640)

    snapshot = scan_tree(share)
    assert snapshot.entries["dir"].kind == "dir"
    entry = snapshot.entries["dir/file.txt"]
    assert entry.kind == "file"
    assert entry.size == 5
    assert entry.mode == 0o640
    assert entry.sha256 == hash_file(share / "dir" / "file.txt")
    assert snapshot.entries["link"].kind == "link"
    assert snapshot.entries["link"].target == "dir/file.txt"


def test_scan_tree_does_not_follow_links(share: pathlib.Path) -> None:
    (share / "real").mkdir()
    (share / "real" / "inside.txt").write_bytes(b"x")
    (share / "alias").symlink_to("real")
    assert "alias/inside.txt" not in scan_tree(share).entries


def test_generator_is_reproducible() -> None:
    first = [action.line() for action in generate_sequence(seed=11, count=40)]
    second = [action.line() for action in generate_sequence(seed=11, count=40)]
    assert first == second
    assert len(first) == 40


def test_generator_differs_between_seeds() -> None:
    assert generate_sequence(seed=1, count=30) != generate_sequence(seed=2, count=30)


def test_generator_respects_name_ceiling() -> None:
    actions = long_name_actions(255)
    assert all(len(action.path.encode()) <= 255 for action in actions)


def test_replay_applies_operations(share: pathlib.Path) -> None:
    actions = [
        Action(op="mkdir", path="d", mode=0o755),
        Action(op="create", path="d/f", mode=0o644),
        Action(op="write", path="d/f", data=b"payload"),
        Action(op="rename", path="d/f", arg="d/g"),
        Action(op="symlink", path="d/l", arg="g"),
        Action(op="chmod", path="d/g", mode=0o600),
    ]
    results = replay(share, actions)
    assert all(step.ok for step in results), [step.detail for step in results if not step.ok]
    assert (share / "d" / "g").read_bytes() == b"payload"
    assert (share / "d" / "g").stat().st_mode & 0o777 == 0o600
    assert (share / "d" / "l").is_symlink()


def test_replay_refuses_to_escape_the_tree(share: pathlib.Path) -> None:
    results = replay(share, [Action(op="create", path="../escape")])
    assert results[0].ok is False
    assert "escapes" in results[0].detail


def test_compare_detects_each_field() -> None:
    host = Snapshot()
    host.add(Entry("f", "file", 3, 0o644, 1000, "abc"))
    guest = Snapshot()
    guest.add(Entry("f", "file", 4, 0o600, 2000, "def"))
    reasons = {item.reason for item in compare(host, guest)}
    assert "size differs" in reasons
    assert "mode differs" in reasons
    assert "mtime differs" in reasons
    assert "content hash differs" in reasons


def test_compare_reports_missing_and_extra() -> None:
    host = Snapshot()
    host.add(Entry("only_host", "file", 1, 0o644, 0, "a"))
    guest = Snapshot()
    guest.add(Entry("only_guest", "file", 1, 0o644, 0, "a"))
    reasons = sorted(item.reason for item in compare(host, guest))
    assert reasons == ["missing in the guest view", "unexpected entry in the guest view"]


def test_compare_tolerates_coarse_mtime() -> None:
    host = Snapshot()
    host.add(Entry("f", "file", 1, 0o644, 1_500_000_000, "a"))
    guest = Snapshot()
    guest.add(Entry("f", "file", 1, 0o644, 1_000_000_000, "a"))
    assert compare(host, guest) != []
    assert compare(host, guest, mtime_slack_ns=1_000_000_000) == []


class LosingReplayer(LocalReplayer):
    """Replayer that silently drops chmod: a stand-in for a broken client."""

    def replay_into(self, work: pathlib.Path, actions: list[Action]):
        kept = [action for action in actions if action.op != "chmod"]
        return super().replay_into(work, kept)


def test_local_diff_passes(share: pathlib.Path) -> None:
    result = run_diff(seed=5, count=60, mode="local", reg_dir=share / "regs", base=share)
    assert result.success
    assert result.total_actions == 60
    assert result.divergences == []


def test_broken_client_is_detected_and_reduced(tmp_path: pathlib.Path) -> None:
    base = tmp_path / "work"
    base.mkdir()
    replayer = LosingReplayer()
    actions = [
        Action(op="create", path="a", mode=0o644),
        Action(op="chmod", path="a", mode=0o600),
        Action(op="create", path="b", mode=0o644),
        Action(op="chmod", path="b", mode=0o600),
        Action(op="create", path="c", mode=0o644),
        Action(op="chmod", path="c", mode=0o600),
    ]
    divergences, _ = run_once(replayer, actions, base)
    assert divergences, "dropping chmod must be detected"
    reduced = reduce_sequence(replayer, actions, base, budget=40)
    assert len(reduced) < len(actions)
    assert any(action.op == "chmod" for action in reduced)


def test_regression_roundtrip(tmp_path: pathlib.Path) -> None:
    actions = [Action(op="create", path="x"), Action(op="write", path="x", data=b"ab")]
    stored = save_regression(tmp_path, 9, actions, [Divergence("x", "size differs")])
    assert stored.is_file()
    restored = load_regression(stored)
    assert [action.line() for action in restored] == [action.line() for action in actions]


def test_cli_diff_json(share: pathlib.Path, tmp_path: pathlib.Path) -> None:
    from fsproof.cli import main

    output = tmp_path / "diff.json"
    status = main([
        "diff", "--local", "--seed", "3", "--count", "40",
        "--regressions", str(tmp_path / "regs"), "--json", str(output),
    ])
    assert status == 0
    assert output.is_file()
    assert '"divergences": []' in output.read_text(encoding="utf-8")

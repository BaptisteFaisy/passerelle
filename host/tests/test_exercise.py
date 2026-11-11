# SPDX-License-Identifier: Apache-2.0
"""Tests for the content exerciser (7.3).

The in-memory model is checked first, then a 10 000-operation run (the
socle figure of section 7.6) alone and with workers, then the shared-file
shape. All of it stays well under the one-minute test budget.
"""

from __future__ import annotations

import pathlib

import pytest

from fsproof.exercise import ExpectedFile, cleanup, exercise_plan, run_exercise

BIG_RUN = 10_000


def test_expected_file_write_and_truncate() -> None:
    model = ExpectedFile()
    model.write_at(0, b"hello ")
    model.write_at(6, b"world")
    assert model.read_at(0, 11) == b"hello world"
    model.truncate(5)
    assert model.read_at(0, 5) == b"hello"
    model.truncate(8)
    assert model.read_at(0, 8) == b"hello\x00\x00\x00"


def test_expected_file_zero_fills_holes() -> None:
    model = ExpectedFile()
    model.write_at(4, b"ab")
    assert model.read_at(0, 6) == b"\x00\x00\x00\x00ab"


def test_exercise_single_worker_ten_thousand_ops(tmp_path: pathlib.Path) -> None:
    result = run_exercise(tmp_path, ops=BIG_RUN, workers=1, seed=1)
    assert result.success, result.mismatch
    assert result.ops_completed == BIG_RUN
    assert result.workers == 1
    assert result.ops_per_second > 0


def test_exercise_four_workers(tmp_path: pathlib.Path) -> None:
    result = run_exercise(tmp_path, ops=BIG_RUN, workers=4, seed=2)
    assert result.success, result.mismatch
    assert result.ops_completed == BIG_RUN


def test_exercise_shared_file(tmp_path: pathlib.Path) -> None:
    result = run_exercise(tmp_path, ops=2000, workers=4, shared_file=True, seed=3)
    assert result.success, result.mismatch
    assert result.shared_file is True
    assert (tmp_path / "shared.dat").stat().st_size > 0


def test_exercise_plan_runs_three_shapes(tmp_path: pathlib.Path) -> None:
    results = exercise_plan(tmp_path, ops=600, workers=3)
    assert len(results) == 3
    assert [item.shared_file for item in results] == [False, False, True]
    assert all(item.success for item in results), [item.mismatch for item in results]


def test_exercise_is_deterministic(tmp_path: pathlib.Path) -> None:
    first = run_exercise(tmp_path / "a", ops=500, workers=1, seed=77)
    second = run_exercise(tmp_path / "b", ops=500, workers=1, seed=77)
    assert first.ops_completed == second.ops_completed
    assert (tmp_path / "a" / "worker_0.dat").read_bytes() == (
        tmp_path / "b" / "worker_0.dat"
    ).read_bytes()


def test_cleanup_removes_artifacts(tmp_path: pathlib.Path) -> None:
    run_exercise(tmp_path, ops=200, workers=2)
    cleanup(tmp_path)
    assert not list(tmp_path.glob("worker_*.dat"))


def test_cli_exercise(tmp_path: pathlib.Path, capsys: pytest.CaptureFixture[str]) -> None:
    from fsproof.cli import main

    status = main(["exercise", "--dir", str(tmp_path), "--ops", "400", "--workers", "2"])
    out = capsys.readouterr().out
    assert status == 0
    assert "PASS" in out

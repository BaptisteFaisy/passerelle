# SPDX-License-Identifier: Apache-2.0
"""Tests for the directed case table (7.4).

Every fast case runs here; the 100 000-entry directory is exercised with a
smaller count so the suite stays under a minute, and the full-size case is
only reachable through --slow on the command line.
"""

from __future__ import annotations

import pathlib

import pytest

from fsproof.cases import (
    BIG_DIR_FAST,
    CASES,
    SLOW_CASES,
    all_passed,
    case_directory_big,
    case_hardlink_identity,
    case_max_path_length,
    case_name_255_bytes,
    case_permission_matrix,
    case_rename_over_existing,
    case_sparse_5gib,
    case_symlink_loop,
    case_unlink_open_file,
    case_names,
    run_cases,
)


def test_case_table_covers_the_specification() -> None:
    names = case_names()
    for expected in (
        "rename_over_existing",
        "unlink_open_file",
        "concurrent_exclusive_create",
        "permission_matrix",
        "symlink_loop",
        "name_255_bytes",
        "sparse_5gib",
        "max_path_length",
    ):
        assert expected in names, f"missing directed case {expected}"
    assert "directory_100k" not in names
    assert "directory_100k" in case_names(include_slow=True)


def test_all_fast_cases_pass(tmp_path: pathlib.Path) -> None:
    results = run_cases(tmp_path)
    assert len(results) == len(CASES)
    for result in results:
        assert result.passed, f"{result.name}: {result.detail}"
    assert all_passed(results)


def test_rename_over_existing_checks_content(tmp_path: pathlib.Path) -> None:
    assert case_rename_over_existing(tmp_path).passed


def test_unlink_open_file(tmp_path: pathlib.Path) -> None:
    assert case_unlink_open_file(tmp_path).passed


def test_permission_matrix(tmp_path: pathlib.Path) -> None:
    assert case_permission_matrix(tmp_path).passed


def test_symlink_loop_detected(tmp_path: pathlib.Path) -> None:
    result = case_symlink_loop(tmp_path)
    assert result.passed
    assert "errno" in result.detail


def test_name_ceiling(tmp_path: pathlib.Path) -> None:
    assert case_name_255_bytes(tmp_path).passed


def test_sparse_file_is_not_materialised(tmp_path: pathlib.Path) -> None:
    result = case_sparse_5gib(tmp_path)
    assert result.passed, result.detail
    assert "size=5368709120" in result.detail


def test_max_path_length(tmp_path: pathlib.Path) -> None:
    result = case_max_path_length(tmp_path)
    assert result.passed, result.detail


def test_hardlink_identity(tmp_path: pathlib.Path) -> None:
    assert case_hardlink_identity(tmp_path).passed


def test_slow_case_with_small_count(tmp_path: pathlib.Path) -> None:
    result = case_directory_big(tmp_path, entries=2000)
    assert result.passed
    assert "entries=2000" in result.detail


def test_slow_table_entry_exists() -> None:
    assert [name for name, _ in SLOW_CASES] == ["directory_100k"]
    assert BIG_DIR_FAST < 100_000


def test_selection_runs_only_requested(tmp_path: pathlib.Path) -> None:
    results = run_cases(tmp_path, select=["symlink_loop"])
    assert [result.name for result in results] == ["symlink_loop"]


def test_cli_cases(tmp_path: pathlib.Path, capsys: pytest.CaptureFixture[str]) -> None:
    from fsproof.cli import main

    status = main(["cases", "--dir", str(tmp_path), "--only", "hardlink_identity"])
    out = capsys.readouterr().out
    assert status == 0
    assert "[PASS] hardlink_identity" in out

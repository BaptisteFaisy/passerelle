# SPDX-License-Identifier: Apache-2.0
"""Tests for the conformance figures collected by pbench (7.2-7.4, 7.6).

`pbench all --conformance` is the single command that produces every number
the report needs; these tests check the plumbing at a small scale, so the
one-command demonstration (DX-02) cannot rot silently.
"""

from __future__ import annotations

import json
import pathlib

import pytest

from bench.conformance import ConformanceFigures, measure, passed, summarize
from bench.cli import main


def test_measure_collects_every_figure(tmp_path: pathlib.Path) -> None:
    figures = measure(scale=0.0002, workers=2, seed=5, work=tmp_path / "conf")
    assert figures.diff_ops >= 10
    assert figures.exercise_ops >= 10
    assert figures.cases_total > 0
    assert figures.cases_passed == figures.cases_total
    assert passed(figures)


def test_figures_use_the_report_field_names() -> None:
    figures = ConformanceFigures(
        diff_ops=100_000,
        diff_divergences=0,
        exercise_ops=1_000_000,
        exercise_workers=16,
        exercise_mismatches=0,
        cases_passed=9,
        cases_total=9,
    )
    assert set(figures.as_dict()) == {
        "diff_ops",
        "diff_divergences",
        "exercise_ops",
        "exercise_workers",
        "exercise_mismatches",
        "cases_passed",
        "cases_total",
    }
    assert passed(figures)
    assert "PASS" in summarize(figures)


def test_failed_figures_are_reported() -> None:
    figures = ConformanceFigures(10, 1, 10, 1, 0, 8, 9)
    assert not passed(figures)
    assert "FAIL" in summarize(figures)


def test_cli_all_with_conformance_writes_figures(tmp_path: pathlib.Path) -> None:
    runs = tmp_path / "passerelle.json"
    status = main([
        "all", "--dir", str(tmp_path / "work"), "--scale", "0.002",
        "--conformance", "--conformance-scale", "0.0002", "--workers", "2",
        "--client", "passerelle", "--json", str(runs),
    ])
    assert status == 0
    payload = json.loads(runs.read_text(encoding="utf-8"))
    assert payload["diff_ops"] >= 10
    assert payload["cases_total"] > 0
    assert payload["exercise_workers"] == 2

# SPDX-License-Identifier: Apache-2.0
"""Tests for the workloads and the comparative report (7.5, 7.6).

Workloads run at a small scale (a fraction of the specification sizes) so
the suite stays fast; the report is checked on synthetic runs, including a
deliberately failing one, so a PASS is never the only outcome covered.
"""

from __future__ import annotations

import json
import pathlib

import pytest

from bench.loads import WORKLOADS, run_workload
from bench.report import (
    LEVELS,
    TargetRun,
    build_report,
    evaluate,
    render_markdown,
    write_report,
)


def _run(client: str, throughput: float = 100.0, ratio: float = 1.0, **kwargs: object) -> TargetRun:
    """Build a TargetRun with plausible workloads for report tests."""
    run = TargetRun(client=client, label=client, mount_time_ms=float(kwargs.get("mount", 20.0)))
    run.workloads["throughput"] = __import__("bench.loads", fromlist=["LoadResult"]).LoadResult(
        workload="throughput", operations=10, duration_s=1.0, bytes_moved=int(throughput * 1024 * 1024)
    )
    run.workloads["priority"] = __import__("bench.loads", fromlist=["LoadResult"]).LoadResult(
        workload="priority", operations=10, duration_s=1.0, extra={"ratio": ratio}
    )
    run.diff_ops = int(kwargs.get("diff_ops", 100_000))
    run.diff_divergences = int(kwargs.get("divergences", 0))
    run.exercise_ops = int(kwargs.get("exercise_ops", 1_000_000))
    run.exercise_workers = int(kwargs.get("workers", 1))
    run.cases_total = 9
    run.cases_passed = int(kwargs.get("cases_passed", 9))
    run.leaks = int(kwargs.get("leaks", 0))
    run.fuzz_seconds = float(kwargs.get("fuzz", 0.0))
    run.virtiofs = bool(kwargs.get("virtiofs", False))
    return run


@pytest.mark.parametrize("name", WORKLOADS)
def test_each_workload_runs_at_small_scale(tmp_path: pathlib.Path, name: str) -> None:
    result = run_workload(name, tmp_path, scale=0.01)
    assert result.workload == name
    assert result.operations > 0
    assert result.duration_s > 0


def test_throughput_reports_mib_per_second(tmp_path: pathlib.Path) -> None:
    result = run_workload("throughput", tmp_path, scale=0.02)
    assert result.throughput_mib_s > 0
    assert "MiB" in result.detail


def test_metadata_counts_three_operations_per_file(tmp_path: pathlib.Path) -> None:
    result = run_workload("metadata", tmp_path, scale=0.001)
    assert result.operations % 3 == 0


def test_priority_workload_exposes_the_ratio(tmp_path: pathlib.Path) -> None:
    result = run_workload("priority", tmp_path, scale=0.01)
    assert "ratio" in result.extra
    assert result.extra["ratio"] > 0


def test_unknown_workload_rejected(tmp_path: pathlib.Path) -> None:
    with pytest.raises(ValueError, match="unknown workload"):
        run_workload("warp-drive", tmp_path)


def test_report_passes_on_good_numbers() -> None:
    runs = [
        _run("passerelle", throughput=120.0),
        _run("qnx-fs-9p", throughput=100.0),
        _run("eclipse", throughput=110.0),
        _run("linux", throughput=200.0),
    ]
    report = build_report(runs, level="socle")
    assert report.passed, [check for check in report.checks if not check.passed]


def test_report_fails_on_slow_mount() -> None:
    report = build_report([_run("passerelle", mount=120.0)], level="socle")
    failed = [check for check in report.checks if not check.passed]
    assert any("mount time" in check.requirement for check in failed)


def test_report_fails_on_priority_regression() -> None:
    report = build_report([_run("passerelle", ratio=3.5)], level="socle")
    assert any("priority" in check.requirement for check in report.checks if not check.passed)


def test_report_fails_without_this_driver() -> None:
    checks = evaluate([_run("linux")], level="socle")
    assert checks[0].passed is False


def test_report_requires_rivals_at_complete_level() -> None:
    report = build_report([_run("passerelle")], level="complet")
    missing = [check for check in report.checks if "comparison against" in check.requirement]
    assert len(missing) == 2
    assert all(not check.passed for check in missing)


def test_report_requires_virtiofs_at_extreme_level() -> None:
    report = build_report([_run("passerelle")], level="extreme")
    virtiofs = [check for check in report.checks if "virtiofs" in check.requirement]
    assert virtiofs and not virtiofs[0].passed


def test_markdown_contains_every_target() -> None:
    report = build_report([_run("passerelle"), _run("eclipse")], level="socle")
    text = render_markdown(report)
    assert "Passerelle comparative report" in text
    assert "| passerelle |" in text
    assert "| eclipse |" in text
    assert "PASS" in text or "FAIL" in text


def test_write_report_produces_markdown_json_and_svg(tmp_path: pathlib.Path) -> None:
    report = build_report([_run("passerelle")], level="socle")
    written = write_report(report, tmp_path / "report")
    assert written["markdown"].is_file()
    assert written["json"].is_file()
    assert written["svg"].is_file()
    assert "<svg" in written["svg"].read_text(encoding="utf-8")
    payload = json.loads(written["json"].read_text(encoding="utf-8"))
    assert payload["level"] == "socle"
    assert payload["targets"][0]["client"] == "passerelle"


def test_cli_all_then_report(tmp_path: pathlib.Path) -> None:
    from bench.cli import main

    runs = tmp_path / "passerelle.json"
    assert main(["all", "--dir", str(tmp_path / "work"), "--scale", "0.005",
                 "--client", "passerelle", "--json", str(runs)]) == 0
    payload = json.loads(runs.read_text(encoding="utf-8"))
    assert set(payload["workloads"]) == set(WORKLOADS)

    prefix = tmp_path / "bench_report"
    status = main(["report", "--run", str(runs), "--level", "socle",
                   "--output", str(prefix)])
    # A workload-only run carries no conformance figures, so the socle
    # thresholds cannot pass: the report must say FAIL, not invent numbers.
    assert status == 1
    assert prefix.with_suffix(".md").is_file()
    assert "FAIL" in prefix.with_suffix(".md").read_text(encoding="utf-8")


def test_cli_all_with_conformance_passes(tmp_path: pathlib.Path) -> None:
    """A recorded run with good conformance figures must pass the socle level.

    The priority workload measures a real latency ratio, which fluctuates
    around the 2.0x threshold when the machine is busy (this suite runs next
    to a full build farm). The measurement itself is covered by
    test_each_workload_runs_at_small_scale and test_priority_workload_exposes
    _the_ratio; here the report is what is under test, so the recorded ratio
    is pinned to a representative value.
    """
    from bench.cli import main

    runs = tmp_path / "passerelle.json"
    assert main([
        "all", "--dir", str(tmp_path / "work"), "--scale", "0.005",
        "--client", "passerelle", "--json", str(runs),
        "--mount-ms", "18.0", "--diff-ops", "100000", "--diff-divergences", "0",
        "--exercise-ops", "1000000", "--exercise-workers", "1",
        "--cases-passed", "9", "--cases-total", "9", "--leaks", "0",
    ]) == 0
    _pin_priority_ratio(runs, 1.2)
    prefix = tmp_path / "bench_report"
    assert main(["report", "--run", str(runs), "--level", "socle",
                 "--output", str(prefix)]) == 0
    assert "PASS" in prefix.with_suffix(".md").read_text(encoding="utf-8")


def _pin_priority_ratio(run_file: pathlib.Path, ratio: float) -> None:
    """Record a fixed priority ratio in a run file (machine-independent tests)."""
    payload = json.loads(run_file.read_text(encoding="utf-8"))
    payload["workloads"]["priority"]["extra"]["ratio"] = ratio
    run_file.write_text(json.dumps(payload), encoding="utf-8")


def test_levels_table_matches_the_specification() -> None:
    assert LEVELS["socle"]["diff_ops"] == 100_000
    assert LEVELS["complet"]["diff_ops"] == 1_000_000
    assert LEVELS["complet"]["exercise_ops"] == 10_000_000
    assert LEVELS["complet"]["workers"] == 16

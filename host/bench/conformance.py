# SPDX-License-Identifier: Apache-2.0
"""Run the conformance bench and collect the figures the report needs.

Covers 7.2-7.4 and the "socle / complet / extreme" thresholds of 7.6:
differential operations without divergence, exerciser operations, directed
cases passed. `pbench all --conformance` calls this so one command produces
a complete run file for `pbench report`.
"""

from __future__ import annotations

import pathlib
import tempfile
from dataclasses import dataclass

from fsproof.cases import all_passed, run_cases
from fsproof.diff import run_diff
from fsproof.exercise import run_exercise


@dataclass
class ConformanceFigures:
    """Numbers measured by the conformance bench, ready for the report."""

    diff_ops: int
    diff_divergences: int
    exercise_ops: int
    exercise_workers: int
    exercise_mismatches: int
    cases_passed: int
    cases_total: int

    def as_dict(self) -> dict[str, int]:
        """Field names as used in the run JSON."""
        return {
            "diff_ops": self.diff_ops,
            "diff_divergences": self.diff_divergences,
            "exercise_ops": self.exercise_ops,
            "exercise_workers": self.exercise_workers,
            "exercise_mismatches": self.exercise_mismatches,
            "cases_passed": self.cases_passed,
            "cases_total": self.cases_total,
        }


def measure(
    scale: float = 1.0,
    workers: int = 16,
    seed: int = 1,
    guest_command: list[str] | None = None,
    work: pathlib.Path | str | None = None,
) -> ConformanceFigures:
    """Run diff, exerciser and directed cases, and return their figures.

    *scale* multiplies the specification sizes: 1.0 runs 100 000
    differential operations (socle) and the exerciser at its socle figure.
    """
    base = pathlib.Path(work) if work else pathlib.Path(tempfile.mkdtemp(prefix="pbench_conf_"))
    base.mkdir(parents=True, exist_ok=True)
    diff_ops = max(10, int(100_000 * scale))
    exercise_ops = max(10, int(1_000_000 * scale))

    diff = run_diff(
        seed=seed,
        count=diff_ops,
        mode="guest" if guest_command else "local",
        guest_command=guest_command,
        reg_dir=base / "regressions",
        base=base / "diff",
    )
    exercise = run_exercise(base / "exercise", ops=exercise_ops, workers=workers, seed=seed)
    cases = run_cases(base / "cases", include_slow=scale >= 1.0)
    return ConformanceFigures(
        diff_ops=diff.total_actions,
        diff_divergences=len(diff.divergences),
        exercise_ops=exercise.ops_completed,
        exercise_workers=exercise.workers,
        exercise_mismatches=0 if exercise.success else 1,
        cases_passed=sum(1 for result in cases if result.passed),
        cases_total=len(cases),
    )


def passed(figures: ConformanceFigures) -> bool:
    """True when the conformance bench found nothing wrong."""
    return (
        figures.diff_divergences == 0
        and figures.exercise_mismatches == 0
        and figures.cases_total > 0
        and figures.cases_passed == figures.cases_total
    )


def summarize(figures: ConformanceFigures) -> str:
    """One-line summary used by the CLI."""
    verdict = "PASS" if passed(figures) else "FAIL"
    return (
        f"conformance: diff {figures.diff_ops} ops / {figures.diff_divergences} divergences, "
        f"exercise {figures.exercise_ops} ops / {figures.exercise_workers} workers, "
        f"cases {figures.cases_passed}/{figures.cases_total} -> {verdict}"
    )


def cases_ok(results: list) -> bool:
    """Re-exported helper so callers do not import fsproof directly."""
    return all_passed(results)

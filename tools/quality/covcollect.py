#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Coverage collection on top of gcov (Python standard library only).

The repository has no coverage dependency: lcov, gcovr and coveralls are all
optional third-party tools, and the self-hosted workers have none of them.
`gcov` ships with gcc, so this module drives it directly with the JSON
intermediate format (`gcov --json-format --stdout`), which is stable since
gcc 9 and needs no parsing of the textual .gcov files.

The result is a dictionary keyed by the source path relative to the
repository root:

    {"common/src/log.c": {"lines": {39: 1, 41: 5}, "functions": {"f": 1}}}

Line counts of the same file compiled into several test binaries are merged
with the maximum, exactly like lcov does: a line executed by any test is
covered.

Requirements covered: 7.6 (the measurement of the portable stack must be
reproducible without a proprietary toolchain) and ROB-06 (the instrumented
build is a first-class build, not an afterthought).
"""

from __future__ import annotations

import json
import pathlib

import qcommon

GCOV_TIMEOUT = 60


def find_gcda(build_dir: pathlib.Path) -> list[pathlib.Path]:
    """Return every gcov data file produced by an instrumented build."""
    if not build_dir.is_dir():
        return []
    return sorted(p for p in build_dir.rglob("*.gcda") if p.is_file())


def gcov_json(gcda: pathlib.Path, gcov: str, timeout: int) -> dict | None:
    """Run gcov on one .gcda file and return its JSON report (None on error)."""
    code, out = qcommon.run(
        [gcov, "--json-format", "--stdout", gcda.name],
        cwd=gcda.parent,
        timeout=timeout,
        quiet=True,
    )
    if code != 0:
        qcommon.warning("gcov failed on %s (code %d)" % (gcda, code))
        return None
    try:
        return json.loads(out)
    except json.JSONDecodeError as exc:
        qcommon.warning("gcov output of %s is not JSON: %s" % (gcda, exc))
        return None


def _keep(path: pathlib.Path, root: pathlib.Path) -> pathlib.PurePath | None:
    """Return the relative path of a source file worth measuring, or None."""
    try:
        resolved = path.resolve()
        rel = resolved.relative_to(root)
    except (ValueError, OSError):
        return None  # system headers and files outside the tree
    if qcommon.is_skipped(rel.parts):
        return None
    return rel


def merge(agg: dict, report: dict, root: pathlib.Path) -> int:
    """Merge one gcov JSON report into agg; return the files merged."""
    merged = 0
    for entry in report.get("files", []):
        rel = _keep(pathlib.Path(entry.get("file", "")), root)
        if rel is None:
            continue
        slot = agg.setdefault(rel.as_posix(), {"lines": {}, "functions": {}})
        for line in entry.get("lines", []):
            number = line.get("line_number")
            if number is None:
                continue
            count = int(line.get("count", 0))
            slot["lines"][number] = max(slot["lines"].get(number, 0), count)
        for func in entry.get("functions", []):
            name = func.get("name", "?")
            hits = int(func.get("execution_count", 0))
            slot["functions"][name] = max(slot["functions"].get(name, 0), hits)
        merged += 1
    return merged


def collect(build_dir, root, gcov, timeout=GCOV_TIMEOUT, log=None):
    """Measure every instrumented object of build_dir; return (agg, counts)."""
    gcda_files = find_gcda(build_dir)
    agg: dict = {}
    failed = 0
    for gcda in gcda_files:
        report = gcov_json(gcda, gcov, timeout)
        if report is None:
            failed += 1
            continue
        merge(agg, report, root)
        if log:
            with open(log, "a", encoding="utf-8") as handle:
                handle.write("gcov %s -> %d file(s)\n"
                             % (gcda, len(report.get("files", []))))
    counts = {"gcda": len(gcda_files), "failed": failed, "sources": len(agg)}
    return agg, counts

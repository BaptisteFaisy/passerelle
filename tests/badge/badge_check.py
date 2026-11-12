#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the coverage badge generator and its --check mode (gap 44).

A badge that lies is worse than no badge, so the check must really fail
when the committed badge and its record disagree, and when the measurement
is too old to be shown. This script exercises the three cases:

  1. a fresh measurement produces a badge and a record that agree;
  2. a badge edited by hand is rejected (the SVG no longer matches the
     recorded percentage);
  3. a record older than the allowed age is rejected as stale.

It needs no pytest and no network: it is registered as a CTest case, so it
runs in the pull-request gate like every other test of the tree.
"""

from __future__ import annotations

import json
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools" / "quality"))

import coverage_badge  # noqa: E402  (path set above)

SUMMARY = """passerelle coverage summary
generated: 2026-11-12T10:00:00+00:00
tools: cmake 3.30.5, ctest 3.30.5, gcov 13.3.0

PRODUCTION CODE :  91.68 % lines (4838/5277, 439 uncovered)  functions 94.94 %
ALL MEASURED    :  93.82 % lines (14794/15769, 975 uncovered)
"""


def fail(message):
    """Report a failed expectation and stop the script."""
    print("badge-check: FAILED: %s" % message, file=sys.stderr)
    sys.exit(1)


def make_measurement(workdir):
    """Write a fake coverage summary and generate the badge from it."""
    summary = workdir / "summary.txt"
    summary.write_text(SUMMARY, encoding="utf-8")
    percent, tools = coverage_badge.parse_summary(summary)
    if percent != 91.68:
        fail("the summary was parsed as %.2f%%, expected 91.68%%" % percent)
    coverage_badge.write_outputs(percent, workdir / "coverage.json",
                                 workdir / "coverage.svg", "abc123",
                                 tools, "coverage")
    return percent


def check_fresh(workdir):
    """A fresh measurement passes the check."""
    problems = coverage_badge.check(workdir / "coverage.json",
                                    workdir / "coverage.svg", 30)
    if problems:
        fail("a fresh badge was rejected: %s" % problems)


def check_tampered(workdir):
    """A badge edited by hand is rejected."""
    svg = workdir / "coverage.svg"
    svg.write_text(svg.read_text(encoding="utf-8").replace("91.7%", "99.9%"),
                   encoding="utf-8")
    problems = coverage_badge.check(workdir / "coverage.json", svg, 30)
    if not problems:
        fail("a badge edited by hand was accepted")
    if not any("does not match" in p for p in problems):
        fail("the edited badge was rejected for the wrong reason: %s" % problems)


def check_stale(workdir):
    """A record older than the allowed age is rejected as stale."""
    record = workdir / "coverage.json"
    data = json.loads(record.read_text(encoding="utf-8"))
    data["measured_at"] = "1999-01-01T00:00:00Z"
    record.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    problems = coverage_badge.check(record, workdir / "coverage.svg", 30)
    if not problems:
        fail("a stale badge was accepted")
    if not any("stale" in p for p in problems):
        fail("the stale badge was rejected for the wrong reason: %s" % problems)


def check_needs_refresh(workdir):
    """The badge is refreshed when the percentage moved, not otherwise."""
    record = workdir / "coverage.json"
    if coverage_badge.needs_refresh(record, 91.68, "coverage", 7):
        fail("an unchanged measurement was considered stale")
    if not coverage_badge.needs_refresh(record, 50.0, "coverage", 7):
        fail("a moved measurement was not detected")


def main():
    """Run the four cases and report the first failure."""
    with tempfile.TemporaryDirectory(prefix="passerelle-badge-") as tmp:
        workdir = pathlib.Path(tmp)
        make_measurement(workdir)
        check_fresh(workdir)
        check_tampered(workdir)
        make_measurement(workdir)
        check_needs_refresh(workdir)
        check_stale(workdir)
    print("badge-check: the badge generator and its --check mode behave")
    return 0


if __name__ == "__main__":
    sys.exit(main())

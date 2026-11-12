# SPDX-License-Identifier: Apache-2.0
"""The trace fixtures must match what the current code produces (gap 55).

The fixtures of host/tests/fixtures/ used to be written by hand once, so a
change of the protocol library could keep passing on data that no longer
describes a real run. They are now produced by tools/regen-fixtures.py from a
real run - the production client against the reference server of tests/ref9p/
- and this test is the gate: it fails when the committed fixture no longer
matches what the code produces.

The same check runs in the CTest suite (tests/tracecap/), where the build is
already there; this pytest brings the gate to the host-tools job of the CI,
which runs on every pull request.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "regen-fixtures.py"

# The build of the capture program is the slow part; the capture itself takes
# a fraction of a second.
TIMEOUT_S = 240


def run_tool(*args: str) -> subprocess.CompletedProcess[str]:
    """Run the regeneration tool from the root of the repository."""
    return subprocess.run(
        [sys.executable, str(TOOL), *args],
        capture_output=True,
        text=True,
        timeout=TIMEOUT_S,
        cwd=str(ROOT),
        check=False,
    )


def test_the_trace_fixtures_match_the_current_code() -> None:
    """The committed fixture is what the code produces now."""
    assert TOOL.is_file(), f"missing regeneration tool: {TOOL}"
    result = run_tool("--check")
    if result.returncode == 2:
        pytest.skip(f"the capture could not run here: {result.stderr.strip()}")
    assert result.returncode == 0, (
        "the trace fixture drifted away from what the code produces:\n"
        f"{result.stdout}\n{result.stderr}\n"
        "regenerate it with: tools/regen-fixtures.py --write"
    )


def test_the_capture_is_reproducible() -> None:
    """Two captures of the same session give the same bytes."""
    result = run_tool("--verify")
    if result.returncode == 2:
        pytest.skip(f"the capture could not run here: {result.stderr.strip()}")
    assert result.returncode == 0, (
        "two captures are not byte-identical:\n"
        f"{result.stdout}\n{result.stderr}"
    )

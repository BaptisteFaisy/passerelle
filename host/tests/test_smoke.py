# SPDX-License-Identifier: Apache-2.0
"""Tests of `qshare smoke` (gap 94, DX-01).

The smoke runs the real build, the real test suites and a real guest, so
the tests here stub it: fake executables in a temporary PATH, a temporary
build tree and a dry run. Nothing here builds a guest or compiles a
project, and the suite stays fast.

Covered: the dry run (which never claims a pass), a machine without any
tool (every step is skipped with its reason), the stubbed machine where
every step passes, the reuse of an existing build tree, and the JSON
contract.
"""

from __future__ import annotations

import json
import pathlib

import pytest

from qshare import smoke
from qshare.cli import main

FAKE_CMAKE = """#!/bin/sh
if [ "$1" = "--version" ]; then echo "cmake version 3.30.5"; exit 0; fi
echo "fake cmake $*"
exit 0
"""

FAKE_CTEST = """#!/bin/sh
if [ "$1" = "--version" ]; then echo "ctest version 3.30.5"; exit 0; fi
echo "100% tests passed, 0 tests failed out of 7"
exit 0
"""

FAKE_PYTHON = """#!/bin/sh
echo "255 passed, 3 skipped in 2.98s"
exit 0
"""

ALL_TOOLS = {"cmake": FAKE_CMAKE, "ctest": FAKE_CTEST, "python3": FAKE_PYTHON}


def fake_bin(tmp_path: pathlib.Path, tools: dict[str, str]) -> pathlib.Path:
    """Create fake executables in one directory and return it."""
    bindir = tmp_path / "bin"
    bindir.mkdir(exist_ok=True)
    for name, body in tools.items():
        script = bindir / name
        script.write_text(body, encoding="utf-8")
        script.chmod(0o755)
    return bindir


@pytest.fixture()
def stub(tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch) -> pathlib.Path:
    """A machine with fake tools, and a build tree of its own."""
    monkeypatch.setenv("PASSERELLE_CONFIG", str(tmp_path / "absent.ini"))
    monkeypatch.setenv("PATH", str(fake_bin(tmp_path, ALL_TOOLS)))
    monkeypatch.setattr(smoke, "REPO", tmp_path)
    (tmp_path / "CMakeLists.txt").write_text("# stub\n", encoding="utf-8")
    (tmp_path / "host" / "tests").mkdir(parents=True)
    return tmp_path


def test_smoke_dry_run_never_claims_a_pass(stub: pathlib.Path, capsys) -> None:
    """A dry run prints the commands and reports no pass."""
    assert main(["smoke", "--dry-run", "--build-dir", str(stub / "build")]) == 0
    out = capsys.readouterr().out
    assert "DRY" in out
    assert "PASS" not in out
    assert "cmake" in out


def test_smoke_without_any_tool_is_skipped(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch, capsys
) -> None:
    """A machine with no tool: every step says why it is skipped."""
    monkeypatch.setenv("PASSERELLE_CONFIG", str(tmp_path / "absent.ini"))
    monkeypatch.setenv("PATH", str(tmp_path / "empty"))
    (tmp_path / "empty").mkdir(exist_ok=True)
    monkeypatch.setattr(smoke, "REPO", tmp_path)
    (tmp_path / "CMakeLists.txt").write_text("# stub\n", encoding="utf-8")
    assert main(["smoke", "--build-dir", str(tmp_path / "build")]) == 3
    out = capsys.readouterr().out
    assert "SKIPPED" in out
    assert "PASS" not in out
    assert "cmake not found" in out


def test_smoke_passes_with_stubbed_tools(stub: pathlib.Path, capsys) -> None:
    """Every step that can run passes, and the summary says so."""
    assert main(["smoke", "--build-dir", str(stub / "build"), "--fast"]) == 0
    out = capsys.readouterr().out
    assert "PASS" in out
    assert "FAIL" not in out
    assert "0 failed" in out


def test_smoke_reuses_an_existing_build_tree(stub: pathlib.Path, capsys) -> None:
    """An existing CMake cache is reused, and the report says so."""
    build = stub / "build"
    build.mkdir()
    (build / "CMakeCache.txt").write_text("# cache\n", encoding="utf-8")
    assert main(["smoke", "--build-dir", str(build), "--fast"]) == 0
    out = capsys.readouterr().out
    assert "reused the existing build tree" in out


def test_smoke_fast_skips_the_guest(stub: pathlib.Path, capsys) -> None:
    """--fast skips the slow part, and says that it skips it."""
    assert main(["smoke", "--build-dir", str(stub / "build"), "--fast"]) == 0
    out = capsys.readouterr().out
    assert "guest" in out
    assert "--fast" in out


def test_smoke_json_contract(stub: pathlib.Path, capsys) -> None:
    """--json answers with the documented keys, for machines."""
    assert main(["smoke", "--json", "--build-dir", str(stub / "build"), "--fast"]) == 0
    report = json.loads(capsys.readouterr().out)
    assert report["command"] == "qshare smoke"
    assert report["version"]
    names = [step["name"] for step in report["steps"]]
    assert names == ["configure", "build", "ctest", "pytest", "bench-ref", "guest"]
    for step in report["steps"]:
        assert set(step) == {"name", "verdict", "detail", "command"}
        assert step["verdict"] in ("pass", "fail", "skipped", "dry")
    assert "passed" in report["summary"]

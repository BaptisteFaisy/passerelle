# SPDX-License-Identifier: Apache-2.0
"""Tests for the CMake and Rust examples (DX-04) and the mount measurement.

The examples must be complete enough to build on the PC; the CMake one is
actually compiled and run here when cmake is installed, and skipped with a
reason otherwise. The mount measurement needs a guest console, so it is
tested on its failure path only.
"""

from __future__ import annotations

import pathlib
import shutil

import pytest

from bench.mount import MOUNT_LIMIT_MS, describe, measure_mount_ms, verdict
from pgwutil import HostError, ValidationError, run_cmd, which
from qshare.examples import (
    CMAKE_DIR,
    RUST_DIR,
    write_all,
    write_cmake_example,
    write_example,
    write_rust_example,
)

CMAKE = which("cmake")
CC = which("cc") or which("gcc")
CARGO = which("cargo")


def test_cmake_example_layout(share: pathlib.Path) -> None:
    example = write_cmake_example(share)
    assert example.directory.name == CMAKE_DIR
    names = sorted(path.name for path in example.files)
    assert names == ["CMakeLists.txt", "adder.c", "adder.h", "adder_test.c", "run_tests.sh"]
    assert example.run_command().endswith("run_tests.sh")
    assert (example.directory / "run_tests.sh").stat().st_mode & 0o111


def test_rust_example_layout(share: pathlib.Path) -> None:
    example = write_rust_example(share)
    assert example.directory.name == RUST_DIR
    assert (example.directory / "Cargo.toml").is_file()
    assert (example.directory / "src" / "lib.rs").is_file()
    assert "#[test]" in (example.directory / "src" / "lib.rs").read_text(encoding="utf-8")


def test_write_all_examples(share: pathlib.Path) -> None:
    examples = write_all(share)
    assert {example.kind for example in examples} == {"cmake", "rust"}


def test_unknown_example_rejected(share: pathlib.Path) -> None:
    with pytest.raises(ValidationError, match="unknown example"):
        write_example("meson", share)


def test_example_requires_existing_share(tmp_path: pathlib.Path) -> None:
    with pytest.raises(ValidationError, match="does not exist"):
        write_cmake_example(tmp_path / "absent")


@pytest.mark.skipif(
    CMAKE is None or CC is None, reason="cmake and a C compiler are needed to build the example"
)
def test_cmake_example_builds_and_passes(share: pathlib.Path, tmp_path: pathlib.Path) -> None:
    """The example must build and its test must pass, exactly as the guest runs it."""
    example = write_cmake_example(share)
    build = tmp_path / "build"
    configure = run_cmd([str(CMAKE), "-S", str(example.directory), "-B", str(build)], timeout=120)
    assert configure.returncode == 0, configure.stderr
    compiled = run_cmd([str(CMAKE), "--build", str(build)], timeout=120)
    assert compiled.returncode == 0, compiled.stderr
    binary = build / "adder_test"
    assert binary.is_file()
    ran = run_cmd([str(binary)], timeout=60)
    assert ran.returncode == 0
    assert "adder: ok" in ran.stdout


@pytest.mark.skipif(CARGO is None, reason="cargo is needed to build the Rust example")
def test_rust_example_compiles(share: pathlib.Path) -> None:
    """`cargo test --no-run` proves the example is a valid Cargo project."""
    example = write_rust_example(share)
    proc = run_cmd([str(CARGO), "test", "--no-run"], cwd=example.directory, timeout=300)
    assert proc.returncode == 0, proc.stderr[-800:]


def test_cli_writes_examples(share: pathlib.Path, capsys: pytest.CaptureFixture[str]) -> None:
    from qshare.cli import main

    assert main([str(share), "--example", "all"]) == 0
    out = capsys.readouterr().out
    assert "cmake:" in out and "rust:" in out
    assert (share / CMAKE_DIR).is_dir()
    assert (share / RUST_DIR).is_dir()


def test_mount_verdict_threshold() -> None:
    assert verdict(0.0) == "PASS"
    assert verdict(MOUNT_LIMIT_MS - 0.1) == "PASS"
    assert verdict(MOUNT_LIMIT_MS) == "FAIL"
    assert "ms" in describe(12.5)


def test_mount_measurement_needs_a_console(tmp_path: pathlib.Path) -> None:
    with pytest.raises(HostError, match="console socket not found"):
        measure_mount_ms(tmp_path / "absent.sock", driver="passerelle", tag="hostshare")

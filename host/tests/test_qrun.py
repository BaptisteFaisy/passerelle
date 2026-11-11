# SPDX-License-Identifier: Apache-2.0
"""Tests for qrun: guest framing, local execution and the CLI (DX-03).

The guest transcript parsing is tested on the exact wire format used by
the console transport, so a change in the markers cannot pass unnoticed.
"""

from __future__ import annotations

import pathlib

import pytest

from qshare.qrun import build_guest_line, main, parse_guest_output, run_local


def test_build_guest_line_frames_the_command() -> None:
    line = build_guest_line("/host/bin/prog", ["--fast", "a b"], "tok")
    assert "echo qrun-begin tok" in line
    assert "/host/bin/prog --fast 'a b'" in line
    assert "echo qrun-rc tok $?" in line
    assert "echo qrun-end tok" in line


def test_parse_guest_output_roundtrip() -> None:
    transcript = (
        "echo qrun-begin tok\r\n"
        "qrun-begin tok\r\n"
        "hello\r\n"
        "qrun-rc tok 3\r\n"
        "qrun-end tok\r\n"
    )
    parsed = parse_guest_output(transcript, "tok")
    assert parsed is not None
    stdout, code = parsed
    assert code == 3
    assert "hello" in stdout


def test_parse_guest_output_ignores_other_tokens() -> None:
    assert parse_guest_output("qrun-begin other\r\nqrun-end other\r\n", "tok") is None


def test_parse_guest_output_needs_the_end_marker() -> None:
    assert parse_guest_output("qrun-begin tok\r\nno end here\r\n", "tok") is None


def test_run_local_returns_output_and_status(share: pathlib.Path) -> None:
    script = share / "hello.sh"
    script.write_text("#!/bin/sh\necho bonjour\nexit 0\n", encoding="utf-8")
    script.chmod(0o755)
    result = run_local(share, "hello.sh", [])
    assert result.exit_code == 0
    assert "bonjour" in result.stdout


def test_run_local_propagates_failure(share: pathlib.Path) -> None:
    script = share / "fail.sh"
    script.write_text("#!/bin/sh\nexit 42\n", encoding="utf-8")
    script.chmod(0o755)
    assert run_local(share, "fail.sh", []).exit_code == 42


def test_run_local_passes_arguments(share: pathlib.Path) -> None:
    script = share / "args.sh"
    script.write_text("#!/bin/sh\necho \"$1\"\n", encoding="utf-8")
    script.chmod(0o755)
    assert "valeur" in run_local(share, "args.sh", ["valeur"]).stdout


def test_cli_local_mode(share: pathlib.Path, capsys: pytest.CaptureFixture[str]) -> None:
    script = share / "cli.sh"
    script.write_text("#!/bin/sh\necho cli-ok\nexit 0\n", encoding="utf-8")
    script.chmod(0o755)
    assert main(["--share", str(share), "--local", "cli.sh"]) == 0
    assert "cli-ok" in capsys.readouterr().out


def test_cli_missing_program(share: pathlib.Path, capsys: pytest.CaptureFixture[str]) -> None:
    assert main(["--share", str(share), "--local", "absent.sh"]) == 2
    assert "not found" in capsys.readouterr().err

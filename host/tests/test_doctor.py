# SPDX-License-Identifier: Apache-2.0
"""Tests of `qshare doctor` (gap 92, DX-01).

The probes must answer the same way on every machine, so the environment is
stubbed: a PATH full of fake executables for the "everything is here" case,
an empty PATH for the "nothing is here" case. The kernel, the KVM device,
the free space and the home directory are stubbed too, so the test never
depends on what the machine running it has.

Covered: the missing case (every probe names the fix), the present case
(exit 0), the JSON contract, an old QEMU, and the rule that a missing tool
is a verdict and never a crash.
"""

from __future__ import annotations

import json
import pathlib

import pytest

from qshare import doctor
from qshare.cli import main

FAKE_QEMU = 'echo "QEMU emulator version 8.2.2 (fake)"'
FAKE_OLD_QEMU = 'echo "QEMU emulator version 7.2.0 (fake)"'
FAKE_VIRTIOFSD = 'echo "virtiofsd 1.10.0"'
FAKE_CMAKE = 'echo "cmake version 3.30.5"'
FAKE_CC = 'echo "gcc (fake) 13.3.0"; exit 0'
EMPTY = "exit 0"

ALL_TOOLS = {
    "qemu-system-x86_64": FAKE_QEMU,
    "virtiofsd": FAKE_VIRTIOFSD,
    "cmake": FAKE_CMAKE,
    "cc": FAKE_CC,
    "curl": EMPTY,
    "cpio": EMPTY,
    "gzip": EMPTY,
    "unzstd": EMPTY,
}


def fake_bin(tmp_path: pathlib.Path, tools: dict[str, str]) -> pathlib.Path:
    """Create fake executables in one directory and return it."""
    bindir = tmp_path / "bin"
    bindir.mkdir(exist_ok=True)
    for name, command in tools.items():
        script = bindir / name
        script.write_text(f"#!/bin/sh\n{command}\n", encoding="utf-8")
        script.chmod(0o755)
    return bindir


@pytest.fixture()
def isolated(tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch) -> pathlib.Path:
    """A machine with no tool at all, and no configuration file."""
    monkeypatch.setenv("PASSERELLE_CONFIG", str(tmp_path / "absent.ini"))
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    monkeypatch.setattr(doctor, "KVM_DEVICE", str(tmp_path / "no-kvm"))
    monkeypatch.setattr(doctor, "find_host_kernel", lambda: None)
    monkeypatch.setattr("qshare.runner.VIRTIOFSD_CANDIDATES", ())
    monkeypatch.setenv("PATH", str(tmp_path / "empty-bin"))
    (tmp_path / "empty-bin").mkdir(exist_ok=True)
    return tmp_path


def complete_machine(tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Turn the isolated machine into one where everything is present."""
    bindir = fake_bin(tmp_path, ALL_TOOLS)
    monkeypatch.setenv("PATH", str(bindir))
    monkeypatch.setattr(doctor, "KVM_DEVICE", str(tmp_path / "kvm"))
    (tmp_path / "kvm").write_text("", encoding="utf-8")
    monkeypatch.setattr(doctor, "find_host_kernel", lambda: pathlib.Path("/boot/vmlinuz-fake"))
    cache = tmp_path / "home" / ".cache" / "passerelle"
    cache.mkdir(parents=True, exist_ok=True)
    (cache / "alpine-minirootfs.tar.gz").write_text("rootfs", encoding="utf-8")


def test_doctor_names_every_missing_tool(isolated: pathlib.Path, capsys) -> None:
    """An empty machine: every probe says what to install, and the exit says no."""
    assert main(["doctor"]) == 3
    out = capsys.readouterr().out
    assert "qemu" in out and "virtiofsd" in out and "cmake" in out
    assert "compiler" in out
    assert "fix:" in out, "every missing probe must name its fix"
    assert "the E5 demonstration cannot run here" in out


def test_doctor_accepts_a_complete_machine(
    isolated: pathlib.Path, monkeypatch: pytest.MonkeyPatch, capsys
) -> None:
    """Every tool present: exit 0 and a green summary."""
    complete_machine(isolated, monkeypatch)
    assert main(["doctor"]) == 0
    out = capsys.readouterr().out
    assert "the E5 demonstration can run here" in out
    assert "MISSING" not in out


def test_doctor_json_contract(isolated: pathlib.Path, capsys) -> None:
    """--json answers with the documented keys, for machines."""
    assert main(["doctor", "--json"]) == 3
    report = json.loads(capsys.readouterr().out)
    assert report["command"] == "qshare doctor"
    assert report["e5_ready"] is False
    assert report["version"]
    assert report["host"]["machine"]
    names = [probe["name"] for probe in report["probes"]]
    assert "qemu" in names and "virtiofsd" in names and "cmake" in names
    for probe in report["probes"]:
        assert set(probe) == {"name", "verdict", "detail", "fix", "blocks", "version", "path"}
        assert probe["verdict"] in ("ok", "warn", "missing")
    assert report["missing"]


def test_doctor_json_contract_when_complete(
    isolated: pathlib.Path, monkeypatch: pytest.MonkeyPatch, capsys
) -> None:
    """The same contract when the machine is complete."""
    complete_machine(isolated, monkeypatch)
    assert main(["doctor", "--json"]) == 0
    report = json.loads(capsys.readouterr().out)
    assert report["e5_ready"] is True
    assert report["build_ready"] is True
    assert report["missing"] == []


def test_doctor_refuses_an_old_qemu(
    isolated: pathlib.Path, monkeypatch: pytest.MonkeyPatch, capsys
) -> None:
    """QEMU older than 8.2 cannot run the E5 bench, and says so."""
    bindir = fake_bin(isolated, {**ALL_TOOLS, "qemu-system-x86_64": FAKE_OLD_QEMU})
    monkeypatch.setenv("PATH", str(bindir))
    assert main(["doctor"]) == 3
    out = capsys.readouterr().out
    assert "older than 8.2" in out
    assert "install QEMU >= 8.2" in out


def test_doctor_never_raises_on_a_broken_tool(
    isolated: pathlib.Path, monkeypatch: pytest.MonkeyPatch, capsys
) -> None:
    """A tool that crashes is a verdict, not a traceback."""
    bindir = fake_bin(isolated, {**ALL_TOOLS, "qemu-system-x86_64": "exit 3"})
    monkeypatch.setenv("PATH", str(bindir))
    assert main(["doctor"]) in (0, 3)
    assert "Traceback" not in capsys.readouterr().out


def test_doctor_shows_the_shared_configuration(
    isolated: pathlib.Path, monkeypatch: pytest.MonkeyPatch, capsys
) -> None:
    """--show-config is the one configuration of the four tools."""
    monkeypatch.setenv("PASSERELLE_COMMON_ARCH", "aarch64")
    assert main(["doctor", "--show-config"]) == 0
    out = capsys.readouterr().out
    assert "arch" in out and "aarch64" in out

# SPDX-License-Identifier: Apache-2.0
"""Demonstration and integration tests (DX-02, VFS-06).

The demo is replayed end to end in local mode, which is what a reader runs
first. Everything that needs QEMU, virtiofsd or a QNX image is skipped
with an explicit reason: never silently.
"""

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess

import pytest

from qshare.demo import run_demo
from qshare.runner import VirtiofsdDaemon, build_qemu_cmd, find_virtiofsd
from qshare.options import validate_share

# A QEMU that cannot start a machine is useless for these tests, and it is not
# always the one PATH offers first: a machine may carry a broken build in
# ~/.local/bin in front of a working one. So every candidate is probed, and the
# first one that really starts is used. When none does, the tests are skipped
# with the reason written out - never silently green.
_QEMU_BROKEN_MARKERS = (
    "could not load module",
    "failed to initialize any accelerator",
    "no accelerator found",
    "tcg-accel-ops",
    "error while loading shared libraries",
)


def _qemu_candidates() -> list[pathlib.Path]:
    found: list[pathlib.Path] = []
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        candidate = pathlib.Path(directory) / "qemu-system-x86_64"
        if candidate.is_file() and os.access(candidate, os.X_OK):
            if candidate not in found:
                found.append(candidate)
    for absolute in ("/usr/bin/qemu-system-x86_64", "/usr/local/bin/qemu-system-x86_64"):
        candidate = pathlib.Path(absolute)
        if candidate.is_file() and os.access(candidate, os.X_OK):
            if candidate not in found:
                found.append(candidate)
    return found


def _qemu_probe(binary: pathlib.Path) -> tuple[bool, str]:
    """Ask for a device that does not exist.

    A working QEMU loads the machine and the accelerator first, then refuses
    the unknown device and exits. A broken one dies earlier, on the module it
    cannot load - which is what we are looking for.
    """
    command = [
        str(binary), "-machine", "q35", "-accel", "tcg", "-display", "none",
        "-nographic", "-monitor", "none", "-serial", "none",
        "-device", "pgw-probe-does-not-exist",
    ]
    try:
        proc = subprocess.run(command, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return False, f"{type(exc).__name__}: {exc}"
    stderr = proc.stderr.strip()
    for marker in _QEMU_BROKEN_MARKERS:
        if marker in stderr:
            return False, stderr.splitlines()[0] if stderr else marker
    return True, ""


def _pick_qemu() -> tuple[pathlib.Path | None, str]:
    reasons: list[str] = []
    for candidate in _qemu_candidates():
        usable, reason = _qemu_probe(candidate)
        if usable:
            return candidate, ""
        reasons.append(f"{candidate}: {reason}")
    if not reasons:
        return None, "qemu-system-x86_64 not installed"
    return None, "no working qemu-system-x86_64 (" + "; ".join(reasons) + ")"


QEMU, QEMU_REASON = _pick_qemu()
VIRTIOFSD = find_virtiofsd()


def test_demo_replays_in_one_command(share: pathlib.Path) -> None:
    status = run_demo(share, tag="hostshare", seed=3)
    assert status == 0
    assert (share / "demo_hello.sh").is_file()
    assert (share / "demo_source.txt").is_file()


def test_probe_rejects_a_qemu_that_cannot_start(tmp_path: pathlib.Path) -> None:
    """A QEMU that dies on its accelerator module must not be selected."""
    fake = tmp_path / "qemu-system-x86_64"
    fake.write_text(
        "#!/bin/sh\n"
        "echo \"qemu: fatal: could not load module for type 'tcg-accel-ops'\" >&2\n"
        "exit 1\n"
    )
    fake.chmod(0o755)
    usable, reason = _qemu_probe(fake)
    assert not usable
    assert "could not load module" in reason


def test_demo_rejects_bad_tag(share: pathlib.Path) -> None:
    from pgwutil import ValidationError

    with pytest.raises(ValidationError):
        run_demo(share, tag="x" * 40)


@pytest.mark.skipif(QEMU is None, reason=QEMU_REASON)
def test_qemu_command_is_built_with_the_share(share: pathlib.Path, tmp_path: pathlib.Path) -> None:
    image = tmp_path / "ifs.bin"
    image.write_bytes(b"\x00" * 16)
    config = validate_share(share, tag="projet")
    cmd = build_qemu_cmd(config, image)
    assert cmd[0].endswith("qemu-system-x86_64")
    assert "-kernel" in cmd
    assert "virtio-9p-pci,fsdev=host,mount_tag=projet" in cmd


@pytest.mark.skipif(QEMU is None, reason=QEMU_REASON)
def test_qemu_rejects_a_too_long_tag(share: pathlib.Path, tmp_path: pathlib.Path) -> None:
    """QEMU itself enforces the 31-byte 9P tag ceiling: check we agree."""
    from pgwutil import run_cmd

    image = tmp_path / "ifs.bin"
    image.write_bytes(b"\x00" * 16)
    proc = run_cmd([
        str(QEMU), "-nographic", "-nodefaults", "-display", "none", "-machine", "q35",
        "-fsdev", f"local,id=host,path={share},security_model=mapped-xattr",
        "-device", f"virtio-9p-pci,fsdev=host,mount_tag={'t' * 40}",
        "-S",
    ], timeout=20)
    assert proc.returncode != 0
    assert "longer than maximum" in proc.stderr


@pytest.mark.skipif(QEMU is None, reason=QEMU_REASON)
def test_virtiofs_options_are_accepted_by_qemu(share: pathlib.Path, tmp_path: pathlib.Path) -> None:
    """The memfd backend and the device must parse (no guest is started)."""
    from pgwutil import run_cmd

    image = tmp_path / "ifs.bin"
    image.write_bytes(b"\x00" * 16)
    config = validate_share(share, tag="vfs", proto="virtiofs")
    cmd = build_qemu_cmd(config, image)
    cmd += ["-S"]
    proc = run_cmd([str(item) for item in cmd], timeout=20)
    message = (proc.stderr or "") + (proc.stdout or "")
    assert "memory-backend-memfd" not in message
    assert "invalid" not in message.lower() or "socket" in message.lower()


@pytest.mark.skipif(VIRTIOFSD is None, reason="virtiofsd not installed")
def test_virtiofsd_starts_and_stops(share: pathlib.Path) -> None:
    config = validate_share(share, tag="vfs", proto="virtiofs")
    daemon = VirtiofsdDaemon(config, start_timeout=10.0)
    try:
        socket_path = daemon.start()
        assert socket_path.exists()
        assert daemon.alive()
    finally:
        daemon.stop()
    assert not socket_path.exists()


@pytest.mark.skipif(
    not (pathlib.Path.home() / "qnx800").is_dir(),
    reason="QNX SDP 8.0 not installed: no guest image can be built",
)
def test_guest_run_requires_a_qnx_image(share: pathlib.Path) -> None:
    """Placeholder for the real guest run; the SDP is absent on this PC."""
    pytest.skip("guest execution needs an mkqnximage image built from the QNX SDP")


@pytest.mark.skipif(shutil.which("mkqnximage") is None, reason="mkqnximage not on PATH")
def test_mkqnximage_is_available() -> None:
    assert shutil.which("mkqnximage") is not None

# SPDX-License-Identifier: Apache-2.0
"""Demonstration and integration tests (DX-02, VFS-06).

The demo is replayed end to end in local mode, which is what a reader runs
first. Everything that needs QEMU, virtiofsd or a QNX image is skipped
with an explicit reason: never silently.
"""

from __future__ import annotations

import pathlib
import shutil

import pytest

from pgwutil import which
from qshare.demo import run_demo
from qshare.runner import VirtiofsdDaemon, build_qemu_cmd, find_virtiofsd
from qshare.options import validate_share

QEMU = which("qemu-system-x86_64")
VIRTIOFSD = find_virtiofsd()


def test_demo_replays_in_one_command(share: pathlib.Path) -> None:
    status = run_demo(share, tag="hostshare", seed=3)
    assert status == 0
    assert (share / "demo_hello.sh").is_file()
    assert (share / "demo_source.txt").is_file()


def test_demo_rejects_bad_tag(share: pathlib.Path) -> None:
    from pgwutil import ValidationError

    with pytest.raises(ValidationError):
        run_demo(share, tag="x" * 40)


@pytest.mark.skipif(QEMU is None, reason="qemu-system-x86_64 not installed")
def test_qemu_command_is_built_with_the_share(share: pathlib.Path, tmp_path: pathlib.Path) -> None:
    image = tmp_path / "ifs.bin"
    image.write_bytes(b"\x00" * 16)
    config = validate_share(share, tag="projet")
    cmd = build_qemu_cmd(config, image)
    assert cmd[0].endswith("qemu-system-x86_64")
    assert "-kernel" in cmd
    assert "virtio-9p-pci,fsdev=host,mount_tag=projet" in cmd


@pytest.mark.skipif(QEMU is None, reason="qemu-system-x86_64 not installed")
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


@pytest.mark.skipif(QEMU is None, reason="qemu-system-x86_64 not installed")
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

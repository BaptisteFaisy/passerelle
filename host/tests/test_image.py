# SPDX-License-Identifier: Apache-2.0
"""Tests for the mkqnximage fragments (DX-01) and the CLI wiring.

The fragments are the contract with mkqnximage: a system layout puts the
driver in /system/bin and mounts it from post_start, an IFS layout puts it
in /proc/boot and mounts it from the startup script.
"""

from __future__ import annotations

import pathlib

import pytest

from pgwutil import ValidationError
from qshare.cli import main
from qshare.image import (
    IFS_FILES,
    IFS_START,
    POST_START,
    SYSTEM_FILES,
    build_fragment,
    install_fragment,
    render_fragment,
)


def test_system_layout_fragment() -> None:
    fragment = build_fragment(
        driver_bin="/opt/pgw/sbin/passerelle", tag="projet", mount_point="/host"
    )
    assert fragment.names() == [POST_START, SYSTEM_FILES]
    assert fragment.snippets[SYSTEM_FILES] == "[perms=0755] bin/passerelle=/opt/pgw/sbin/passerelle\n"
    body = fragment.snippets[POST_START]
    assert "passerelle projet /host" in body
    assert "waitfor /host" in body


def test_ifs_layout_fragment() -> None:
    fragment = build_fragment(
        driver_bin="/opt/pgw/sbin/passerelle", tag="projet", layout="ifs"
    )
    assert fragment.names() == [IFS_FILES, IFS_START]
    assert fragment.snippets[IFS_FILES].startswith("[perms=0755] passerelle=")
    assert "passerelle projet /host" in fragment.snippets[IFS_START]


def test_fragment_rejects_relative_driver() -> None:
    with pytest.raises(ValidationError, match="absolute host path"):
        build_fragment(driver_bin="sbin/passerelle", tag="projet")


def test_fragment_rejects_bad_layout() -> None:
    with pytest.raises(ValidationError, match="layout must be"):
        build_fragment(driver_bin="/opt/passerelle", tag="t", layout="uefi")


def test_install_fragment_writes_snippets(tmp_path: pathlib.Path) -> None:
    vm = tmp_path / "vm"
    (vm / "local" / "snippets").mkdir(parents=True)
    fragment = build_fragment(driver_bin="/opt/pgw/passerelle", tag="projet")
    written = install_fragment(vm, fragment)
    assert len(written) == 2
    for path in written:
        assert path.is_file()
        assert path.parent.name == "snippets"


def test_install_fragment_requires_vm_dir(tmp_path: pathlib.Path) -> None:
    with pytest.raises(ValidationError, match="does not exist"):
        install_fragment(tmp_path / "absent", build_fragment("/opt/x", "t"))


def test_render_fragment_has_headers() -> None:
    text = render_fragment(build_fragment("/opt/x", "t"))
    assert "local/snippets/" in text


def test_cli_emit_prints_options(share: pathlib.Path, capsys: pytest.CaptureFixture[str]) -> None:
    status = main([str(share), "--tag", "projet", "--emit"])
    out = capsys.readouterr().out
    assert status == 0
    assert "virtio-9p-pci,fsdev=host,mount_tag=projet" in out


def test_cli_virtiofs_emit(share: pathlib.Path, capsys: pytest.CaptureFixture[str]) -> None:
    status = main([str(share), "--proto", "virtiofs", "--emit"])
    out = capsys.readouterr().out
    assert status == 0
    assert "memory-backend-memfd" in out
    assert "vhost-user-fs-pci" in out


def test_cli_rejects_bad_tag(share: pathlib.Path, capsys: pytest.CaptureFixture[str]) -> None:
    status = main([str(share), "--tag", "x" * 40])
    assert status == 2
    assert "invalid input" in capsys.readouterr().err


def test_cli_mkqnximage(share: pathlib.Path, tmp_path: pathlib.Path) -> None:
    vm = tmp_path / "vm"
    (vm / "local" / "snippets").mkdir(parents=True)
    status = main([str(share), "--mkqnximage", str(vm), "--tag", "projet"])
    assert status == 0
    assert (vm / "local" / "snippets" / SYSTEM_FILES).is_file()


def test_cli_security_help(capsys: pytest.CaptureFixture[str]) -> None:
    assert main(["--security-help"]) == 0
    assert "mapped-xattr" in capsys.readouterr().out

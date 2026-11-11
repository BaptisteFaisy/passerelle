# SPDX-License-Identifier: Apache-2.0
"""Tests for the QEMU option generator (DX-01, SEC-03, VFS-06).

The 31-byte 9P tag ceiling and the 36-byte virtiofs ceiling are asserted
here because QEMU 8.2 refuses longer tags (9P) and because the virtio
specification fixes the virtiofs tag at 36 bytes (struct virtio_fs_config);
the exact option strings are asserted verbatim because they are the
contract of `qshare`.
"""

from __future__ import annotations

import pathlib

import pytest

from qshare.options import (
    DEFAULT_TAG,
    RECOMMENDED_SECURITY,
    SECURITY_MODELS,
    TAG_MAX_9P,
    TAG_MAX_VIRTIOFS,
    build_9p_args,
    build_qemu_args,
    build_virtiofs_args,
    describe_security_models,
    tag_limit,
    validate_share,
)
from pgwutil import ValidationError


def test_validate_share_defaults(share: pathlib.Path) -> None:
    config = validate_share(share)
    assert config.path == share.resolve()
    assert config.tag == DEFAULT_TAG
    assert config.security_model == RECOMMENDED_SECURITY
    assert config.proto == "9p"


def test_validate_share_rejects_missing_directory(tmp_path: pathlib.Path) -> None:
    with pytest.raises(ValidationError, match="does not exist"):
        validate_share(tmp_path / "absent")


def test_validate_share_rejects_file(share: pathlib.Path) -> None:
    target = share / "file.txt"
    target.write_text("x", encoding="utf-8")
    with pytest.raises(ValidationError, match="not a directory"):
        validate_share(target)


@pytest.mark.parametrize("proto,limit", [("9p", TAG_MAX_9P), ("virtiofs", TAG_MAX_VIRTIOFS)])
def test_tag_ceiling(share: pathlib.Path, proto: str, limit: int) -> None:
    assert tag_limit(proto) == limit
    accepted = "t" * limit
    assert validate_share(share, tag=accepted, proto=proto).tag == accepted
    with pytest.raises(ValidationError, match="at most"):
        validate_share(share, tag="t" * (limit + 1), proto=proto)


def test_tag_rejects_empty_and_whitespace(share: pathlib.Path) -> None:
    with pytest.raises(ValidationError, match="empty"):
        validate_share(share, tag="")
    with pytest.raises(ValidationError, match="whitespace"):
        validate_share(share, tag="host share")


def test_unknown_security_model_rejected(share: pathlib.Path) -> None:
    with pytest.raises(ValidationError, match="unknown security model"):
        validate_share(share, security_model="passthrough-plus")


def test_recommended_model_needs_no_root() -> None:
    needs_root, _, _ = SECURITY_MODELS[RECOMMENDED_SECURITY]
    assert needs_root is False, "SEC-03: the recommended model must not need root"
    assert SECURITY_MODELS["passthrough"][0] is True


def test_security_help_mentions_root(share: pathlib.Path) -> None:
    text = describe_security_models()
    assert RECOMMENDED_SECURITY in text
    assert "no root" in text
    assert "passthrough" in text


def test_build_9p_args_exact(share: pathlib.Path) -> None:
    config = validate_share(share, tag="projet", security_model="mapped-xattr")
    assert build_9p_args(config) == [
        "-fsdev",
        f"local,id=host,path={share.resolve()},security_model=mapped-xattr",
        "-device",
        "virtio-9p-pci,fsdev=host,mount_tag=projet",
    ]


def test_build_virtiofs_args_exact(share: pathlib.Path) -> None:
    socket = share / "vhost.sock"
    config = validate_share(
        share, tag="vfs", proto="virtiofs", socket_path=socket, mem_size="2G"
    )
    args = build_virtiofs_args(config)
    assert args[args.index("-object") + 1] == "memory-backend-memfd,id=mem,size=2G,share=on"
    assert "memdev=mem" in args[args.index("-numa") + 1]
    assert args[args.index("-chardev") + 1] == f"socket,id=char0,path={socket.resolve()}"
    assert args[args.index("-device") + 1] == "vhost-user-fs-pci,chardev=char0,tag=vfs"


def test_build_qemu_args_dispatch(share: pathlib.Path) -> None:
    assert build_qemu_args(validate_share(share))[0] == "-fsdev"
    virtiofs = validate_share(share, proto="virtiofs")
    assert build_qemu_args(virtiofs)[0] == "-object"


def test_socket_path_too_long_rejected(share: pathlib.Path) -> None:
    with pytest.raises(ValidationError, match="socket path too long"):
        validate_share(share, proto="virtiofs", socket_path="/tmp/" + "s" * 120)

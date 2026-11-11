# SPDX-License-Identifier: Apache-2.0
"""Exact QEMU share options for virtio-9p and virtiofs.

Covers DX-01 (QEMU options), VFS-06 (virtiofsd + shared memory) and
SEC-03 (security models documented, the recommended one needs no root).

Both tag ceilings were checked against the real implementation, not guessed:

* 9P -- qemu-system-x86_64 8.2.2 refuses a longer tag with
  "mount tag '<tag>' (40 bytes) is longer than maximum (31 bytes)".
* virtiofs -- the virtio specification fixes the tag at 36 bytes
  (Linux UAPI: struct virtio_fs_config { __u8 tag[36]; } in
  include/uapi/linux/virtio_fs.h), and QEMU 8.2.2 accepts a 36-byte tag
  on a vhost-user-fs-pci device.
"""

from __future__ import annotations

import pathlib
from dataclasses import dataclass, field

from pgwutil import ValidationError

DEFAULT_TAG = "hostshare"
TAG_MAX_9P = 31
TAG_MAX_VIRTIOFS = 36
PROTOCOLS = ("9p", "virtiofs")

# security_model=<value> -> (needs root, keeps host uid/gid, note)
SECURITY_MODELS: dict[str, tuple[bool, bool, str]] = {
    "mapped-xattr": (
        False,
        False,
        "guest uid/gid/mode stored in user.virtfs.* xattrs on the host file; "
        "QEMU never needs root (recommended, SEC-03)",
    ),
    "mapped-file": (
        False,
        False,
        "like mapped-xattr but stores the metadata in a .virtfs_metadata "
        "side file; needs a filesystem without xattr support",
    ),
    "passthrough": (
        True,
        True,
        "host ownership is used verbatim; QEMU must run as root, so a guest "
        "root can change host files: avoid (SEC-03)",
    ),
    "none": (
        False,
        True,
        "every guest user is treated as the QEMU user; no privilege split "
        "between guests, fine for a single-user share",
    ),
}
RECOMMENDED_SECURITY = "mapped-xattr"


@dataclass(frozen=True)
class ShareConfig:
    """Validated description of one host directory shared with a guest."""

    path: pathlib.Path
    tag: str = DEFAULT_TAG
    security_model: str = RECOMMENDED_SECURITY
    proto: str = "9p"
    socket_path: pathlib.Path | None = None
    mem_size: str = "1G"
    extra_fsdev: tuple[str, ...] = field(default_factory=tuple)


def tag_limit(proto: str) -> int:
    """Return the mount tag byte ceiling enforced by QEMU for *proto*."""
    return TAG_MAX_VIRTIOFS if proto == "virtiofs" else TAG_MAX_9P


def validate_share(
    path: pathlib.Path | str,
    tag: str = DEFAULT_TAG,
    security_model: str = RECOMMENDED_SECURITY,
    proto: str = "9p",
    socket_path: pathlib.Path | str | None = None,
    mem_size: str = "1G",
) -> ShareConfig:
    """Validate every input and return an immutable ShareConfig."""
    directory = pathlib.Path(path).expanduser()
    if not directory.exists():
        raise ValidationError(f"share directory does not exist: {directory}")
    if not directory.is_dir():
        raise ValidationError(f"share path is not a directory: {directory}")
    if proto not in PROTOCOLS:
        raise ValidationError(f"protocol must be one of {PROTOCOLS}, got '{proto}'")
    if security_model not in SECURITY_MODELS:
        raise ValidationError(
            f"unknown security model '{security_model}'; valid: "
            f"{', '.join(SECURITY_MODELS)}"
        )
    _validate_tag(tag, proto)
    socket = None if socket_path is None else pathlib.Path(socket_path).expanduser().resolve()
    if socket is not None and len(str(socket).encode("utf-8")) > 107:
        raise ValidationError(f"socket path too long for a unix socket (max 107 bytes): {socket}")
    return ShareConfig(
        path=directory.resolve(),
        tag=tag,
        security_model=security_model,
        proto=proto,
        socket_path=socket,
        mem_size=mem_size,
    )


def _validate_tag(tag: str, proto: str) -> None:
    """Reject empty, oversized or whitespace-bearing mount tags."""
    raw = tag.encode("utf-8")
    if not raw:
        raise ValidationError("mount tag cannot be empty")
    if any(ch.isspace() for ch in tag):
        raise ValidationError(f"mount tag cannot contain whitespace: '{tag}'")
    limit = tag_limit(proto)
    if len(raw) > limit:
        raise ValidationError(
            f"mount tag is {len(raw)} bytes, QEMU allows at most {limit} for {proto}: '{tag}'"
        )


def build_9p_args(cfg: ShareConfig) -> list[str]:
    """QEMU arguments for a 9P2000.L share over virtio-9p-pci (DX-01)."""
    fsdev = (
        f"local,id=host,path={cfg.path},security_model={cfg.security_model}"
    )
    for option in cfg.extra_fsdev:
        fsdev += f",{option}"
    device = f"virtio-9p-pci,fsdev=host,mount_tag={cfg.tag}"
    return ["-fsdev", fsdev, "-device", device]


def build_virtiofs_args(cfg: ShareConfig) -> list[str]:
    """QEMU arguments for virtiofs over vhost-user-fs-pci (VFS-06).

    The memory backend must be shared (memfd with share=on) because the
    daemon maps guest RAM directly; the device talks to virtiofsd through
    a vhost-user unix socket.
    """
    socket = cfg.socket_path or default_socket_path(cfg.tag)
    return [
        "-object",
        f"memory-backend-memfd,id=mem,size={cfg.mem_size},share=on",
        "-numa",
        "node,memdev=mem",
        "-chardev",
        f"socket,id=char0,path={socket}",
        "-device",
        f"vhost-user-fs-pci,chardev=char0,tag={cfg.tag}",
    ]


def default_socket_path(tag: str) -> pathlib.Path:
    """Deterministic vhost-user socket path for *tag*."""
    return pathlib.Path("/tmp") / f"pgw-vhost-fs-{tag}.sock"


def build_qemu_args(cfg: ShareConfig) -> list[str]:
    """Dispatch to the argument builder of the selected protocol."""
    return build_virtiofs_args(cfg) if cfg.proto == "virtiofs" else build_9p_args(cfg)


def describe_security_models() -> str:
    """Human-readable table of the QEMU security models (SEC-03)."""
    lines = ["QEMU security models for -fsdev local (SEC-03):"]
    for name, (needs_root, shared_ids, note) in SECURITY_MODELS.items():
        marker = " (recommended)" if name == RECOMMENDED_SECURITY else ""
        root = "needs root" if needs_root else "no root"
        lines.append(f"  {name}{marker} [{root}]: {note}")
        if shared_ids and not needs_root:
            lines.append("      guest and host share the same credentials")
    return "\n".join(lines)

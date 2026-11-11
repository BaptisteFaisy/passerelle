# SPDX-License-Identifier: Apache-2.0
"""mkqnximage fragments that embed the driver and mount the share at boot.

Covers DX-01: one command emits the snippets an mkqnximage image needs so
that the guest mounts the host directory during startup, and DX-06 (image
that only carries the kernel and the driver).

Two layouts are produced:
  * system snippets  -- the driver lands in /system/bin and a post_start
    command mounts the share once the system is up (default);
  * IFS snippets     -- the driver goes into /proc/boot and the startup
    script mounts the share as early as possible.
"""

from __future__ import annotations

import pathlib
import shlex
from dataclasses import dataclass

from pgwutil import ValidationError, write_atomic

DEFAULT_DRIVER = "passerelle"
DEFAULT_MOUNT_POINT = "/host"
DEFAULT_WAIT_SECONDS = 10

SYSTEM_FILES = "system_files.custom"
POST_START = "post_start.custom"
IFS_FILES = "ifs_files.custom"
IFS_START = "ifs_start.custom"


@dataclass(frozen=True)
class ImageFragment:
    """Set of mkqnximage snippets for one image layout."""

    layout: str
    snippets: dict[str, str]

    def names(self) -> list[str]:
        """Return the snippet file names in stable order."""
        return sorted(self.snippets)


def _mount_commands(driver: str, tag: str, mount_point: str, wait_s: int) -> list[str]:
    """Commands the guest runs to mount the share (driver + waitfor)."""
    return [
        f"{driver} {shlex.quote(tag)} {shlex.quote(mount_point)}",
        f"waitfor {shlex.quote(mount_point)} {wait_s}",
    ]


def build_fragment(
    driver_bin: str,
    tag: str,
    mount_point: str = DEFAULT_MOUNT_POINT,
    driver_name: str = DEFAULT_DRIVER,
    layout: str = "system",
    wait_s: int = DEFAULT_WAIT_SECONDS,
) -> ImageFragment:
    """Build the snippet contents for *layout* ('system' or 'ifs')."""
    if not driver_bin.startswith("/"):
        raise ValidationError(
            f"mkqnximage needs an absolute host path for the driver binary, got '{driver_bin}'"
        )
    if not mount_point.startswith("/"):
        raise ValidationError(f"mount point must be absolute, got '{mount_point}'")
    if layout not in ("system", "ifs"):
        raise ValidationError(f"layout must be 'system' or 'ifs', got '{layout}'")

    commands = _mount_commands(driver_name, tag, mount_point, wait_s)
    script = "\n".join(commands) + "\n"
    if layout == "ifs":
        snippets = {
            IFS_FILES: f"[perms=0755] {driver_name}={driver_bin}\n",
            IFS_START: script,
        }
    else:
        snippets = {
            SYSTEM_FILES: f"[perms=0755] bin/{driver_name}={driver_bin}\n",
            POST_START: f"# mount the Passerelle host share\ntrue\n" + script,
        }
    return ImageFragment(layout=layout, snippets=snippets)


def install_fragment(
    vm_dir: pathlib.Path | str, fragment: ImageFragment
) -> list[pathlib.Path]:
    """Write *fragment* into <vm_dir>/local/snippets and return the paths."""
    base = pathlib.Path(vm_dir).expanduser()
    if not base.is_dir():
        raise ValidationError(f"mkqnximage VM directory does not exist: {base}")
    target = base / "local" / "snippets"
    target.mkdir(parents=True, exist_ok=True)
    return [write_atomic(target / name, text) for name, text in sorted(fragment.snippets.items())]


def render_fragment(fragment: ImageFragment) -> str:
    """Render *fragment* as one copy-pasteable block (used by --emit-image)."""
    blocks = [f"# --- local/snippets/{name} ---\n{text}" for name, text in sorted(fragment.snippets.items())]
    return "\n".join(blocks)

# SPDX-License-Identifier: Apache-2.0
"""What the host machine offers: the kernel a guest can boot (gap 92).

One lookup, shared by `qshare doctor` and by the E5 harness
(tests/qemu/run_share_check.py): the newest readable host kernel that has
a matching /lib/modules tree. Recent distributions install the kernel
0600 (root only) and QEMU runs unprivileged here, so readability is part
of the choice, not only the version.

Standard library only: os, pathlib.
"""

from __future__ import annotations

import os
import pathlib

BOOT_DIR = "/boot"
MODULES_DIR = "/lib/modules"


def find_host_kernel(boot: str = BOOT_DIR, modules: str = MODULES_DIR) -> pathlib.Path | None:
    """Newest readable host kernel with a matching modules tree, else None.

    The kernel image is skipped when this user cannot read it, and when
    the modules of that version are not installed: without them the guest
    cannot mount a 9P or a virtiofs share.
    """
    for kernel in sorted(pathlib.Path(boot).glob("vmlinuz-*"), reverse=True):
        if not os.access(kernel, os.R_OK):
            continue
        version = kernel.name[len("vmlinuz-"):]
        if pathlib.Path(modules, version).is_dir():
            return kernel
    return None

# SPDX-License-Identifier: Apache-2.0
"""`qshare init`: scaffold a guest project ready to be shared (gap 93, DX-01).

One command that prepares a directory to be shared with a QNX guest: the
source tree of an example project (the same one `qshare --example` writes,
so there is one generator and not two), the build recipe, the exact QEMU
options of the share, the guest snippet that installs the driver, and a
README that says what to run on the host and in the guest.

The command is idempotent: a file that is already there with the same
content is left alone, so running it twice changes nothing the second
time. A file that exists with a *different* content is never overwritten
without `--force`, which is reported as a list of the files in question.

Every option comes from the one configuration file of the four tools
(`pgwutil.config`): the tag, the protocol, the mount point and the guest
driver are the values the share will use.
"""

from __future__ import annotations

import argparse
import pathlib

from pgwutil import ValidationError, write_if_changed
from pgwutil import config as pgwconfig
from qshare.examples import EXECUTABLE, TREES, plan_example
from qshare.image import build_fragment, render_fragment
from qshare.options import build_qemu_args, validate_share

TOOL = "qshare"
README = "README.md"
QEMU_OPTIONS = "qemu-options.txt"
GUEST_SETUP = "guest-setup.txt"

DESCRIPTION = """Scaffold a guest project ready to be shared.

qshare init <dir> writes the source tree of an example project, its build
recipe, the exact QEMU options of the share, the guest snippet that
installs the driver, and a README that says what to run. Running it twice
changes nothing; an existing file with a different content is only
replaced with --force.

Exit status: 0 when the project is ready, 2 when a file would be
overwritten without --force.
"""


def _readme(share: pathlib.Path, kinds: list[str], tag: str, proto: str,
            mount_point: str, driver: str) -> str:
    """The README of the scaffolded project: what to run, host and guest."""
    trees = ", ".join(f"{kind}_example/" for kind in kinds)
    lines = [
        f"# Guest project shared with {tag}",
        "",
        "This directory is meant to be shared with a QNX guest under QEMU.",
        "",
        "## What is here",
        "",
        f"- `{trees}` - the example project(s), with their build recipe "
        "(`run_tests.sh` builds then runs the tests).",
        f"- `{QEMU_OPTIONS}` - the exact QEMU options of the share.",
        f"- `{GUEST_SETUP}` - the guest snippet that installs and mounts the driver.",
        "",
        "## On the host",
        "",
        "```sh",
        f"qshare {share} --tag {tag} --proto {proto}   # the QEMU options, again",
        "qemu-system-x86_64 $(cat qemu-options.txt)   # boot the guest",
        "```",
        "",
        "## In the guest",
        "",
        "```sh",
        f"{driver} {tag} {mount_point}    # mount the share (see {GUEST_SETUP})",
        f"qrun --console /tmp/ser.sock --share {share} "
        f"{kinds[0]}_example/run_tests.sh",
        "```",
        "",
        "## Build recipe",
        "",
        "```sh",
        "cmake -S cmake_example -B cmake_example/build",
        "cmake --build cmake_example/build",
        "ctest --test-dir cmake_example/build --output-on-failure",
        "```",
        "",
    ]
    return "\n".join(lines)


def _guest_setup(tag: str, mount_point: str, driver: str, driver_bin: str) -> str:
    """The guest snippet: the mkqnximage fragments that mount the share."""
    fragment = build_fragment(
        driver_bin=driver_bin,
        tag=tag,
        mount_point=mount_point,
        driver_name=driver,
        layout="system",
    )
    return render_fragment(fragment)


def _plan(
    share: pathlib.Path, kinds: list[str], tag: str, proto: str,
    mount_point: str, driver: str, driver_bin: str
) -> list[tuple[pathlib.Path, str, bool]]:
    """Every file the scaffold writes, as (path, content, executable)."""
    planned: list[tuple[pathlib.Path, str, bool]] = []
    for kind in kinds:
        for path, content in plan_example(kind, share):
            planned.append((path, content, path.name in EXECUTABLE))
    cfg = validate_share(share, tag=tag, proto=proto)
    planned.append((share / QEMU_OPTIONS, " ".join(build_qemu_args(cfg)) + "\n", False))
    planned.append(
        (share / GUEST_SETUP, _guest_setup(tag, mount_point, driver, driver_bin), False)
    )
    planned.append(
        (share / README,
         _readme(share, kinds, tag, proto, mount_point, driver), False)
    )
    return planned


def _conflicting(planned: list[tuple[pathlib.Path, str, bool]]) -> list[pathlib.Path]:
    """Files that exist with a different content: they must not be replaced."""
    conflicts = []
    for path, content, _executable in planned:
        if not path.is_file():
            continue
        try:
            same = path.read_text(encoding="utf-8") == content
        except (OSError, UnicodeDecodeError):
            same = False
        if not same:
            conflicts.append(path)
    return conflicts


def register(parser: argparse.ArgumentParser) -> None:
    """Register the options of `qshare init`."""
    parser.add_argument("directory", help="directory to scaffold (created if needed)")
    parser.add_argument(
        "--example", choices=("cmake", "rust", "all"), default="cmake",
        help="which example project to write (default: cmake)",
    )
    parser.add_argument(
        "--force", action="store_true", help="replace the files whose content differs"
    )
    parser.add_argument(
        "--tag", default=None, help=pgwconfig.help_text("tag", "mount tag of the share")
    )
    parser.add_argument(
        "--proto", choices=("9p", "virtiofs"), default=None,
        help=pgwconfig.help_text("proto", "share protocol"),
    )
    parser.add_argument(
        "--mount-point", default=None,
        help=pgwconfig.help_text("mount-point", "guest mount point"),
    )
    parser.add_argument(
        "--driver", default=None, help=pgwconfig.help_text("driver", "guest driver name")
    )
    parser.add_argument(
        "--driver-bin", default=None,
        help=pgwconfig.help_text("driver-bin", "absolute host path of the driver binary"),
    )
    pgwconfig.add_show_config(parser)


def run(args: argparse.Namespace) -> int:
    """Scaffold the project and report what was written."""
    config = pgwconfig.apply(args, pgwconfig.load(TOOL))
    if getattr(args, "show_config", None):
        print(pgwconfig.render(config, args.show_config))
        return 0
    share = pathlib.Path(args.directory).expanduser()
    if share.exists() and not share.is_dir():
        raise ValidationError(f"the share path is not a directory: {share}")
    share.mkdir(parents=True, exist_ok=True)
    kinds = list(TREES) if args.example == "all" else [args.example]
    planned = _plan(
        share, kinds, config.get("tag"), config.get("proto"), config.get("mount-point"),
        config.get("driver"), config.get("driver-bin"),
    )
    conflicts = _conflicting(planned)
    if conflicts and not args.force:
        names = ", ".join(str(path) for path in conflicts)
        raise ValidationError(
            f"refusing to overwrite {len(conflicts)} file(s) with a different content: "
            f"{names}; use --force to replace them"
        )
    written = []
    for path, content, executable in planned:
        if write_if_changed(path, content, executable=executable):
            written.append(path)
    print(f"project ready in {share}")
    for path in written:
        print(f"  wrote {path}")
    if not written:
        print("  nothing to write: the project is already up to date")
    print(f"  next: qshare {share} --tag {config.get('tag')} --proto {config.get('proto')}")
    return 0

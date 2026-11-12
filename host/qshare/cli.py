# SPDX-License-Identifier: Apache-2.0
"""Command line interface of qshare (DX-01, VFS-06, SEC-03, DX-02).

`qshare <dir>` prints the exact QEMU options for a 9P or virtiofs share;
`--emit` prints them as one shell-ready line, `--emit-image` or
`--mkqnximage` produce the mkqnximage snippets, `--run` starts QEMU (and
virtiofsd) itself, and `--demo` replays the whole demonstration (DX-02).

Three subcommands answer the everyday questions (gaps 92, 93, 94):
`qshare doctor` says what is missing on this machine, `qshare init`
scaffolds a guest project ready to be shared, and `qshare smoke` proves the
whole chain here. Every option of the share itself is resolved through the
one configuration file of the four tools (gap 99, `--show-config`).
"""

from __future__ import annotations

import argparse
import importlib
import pathlib
import sys

from pgwutil import HostError, ValidationError
from pgwutil import config as pgwconfig
from qshare.image import build_fragment, install_fragment, render_fragment
from qshare.options import (
    PROTOCOLS,
    SECURITY_MODELS,
    build_qemu_args,
    describe_security_models,
    validate_share,
)
from qshare.runner import QemuInstance, build_qemu_cmd, share_backend

TOOL = "qshare"
# Subcommands that take the first positional word instead of a share directory.
COMMANDS = ("doctor", "init", "smoke")


def _version_string() -> str:
    """Version of the installed package, else the repository's VERSION file.

    The package metadata is authoritative once installed; when running straight
    from a checkout there is no metadata, so the VERSION file at the repository
    root answers instead. Both must agree - `tools/release/test_version_sync.py`
    is what enforces it.
    """
    try:
        from importlib.metadata import version

        return version("passerelle-host")
    except Exception:  # noqa: BLE001 - not installed as a package
        pass
    candidate = pathlib.Path(__file__).resolve().parents[2] / "VERSION"
    if candidate.is_file():
        return candidate.read_text(encoding="utf-8").strip()
    return "0.0.0"


def _add_share_options(parser: argparse.ArgumentParser) -> None:
    """Register the share-definition options (directory, tag, protocol, model).

    Every configurable option defaults to None: `pgwutil.config.apply` fills
    the ones the user did not type, which is what makes the precedence
    command line > environment > file > built-in default hold (gap 99).
    """
    help_text = pgwconfig.help_text
    parser.add_argument("directory", nargs="?", help="host directory to share")
    parser.add_argument(
        "--tag", "-t", default=None,
        help=help_text("tag", "mount tag (9P <=31 bytes, virtiofs <=36)"),
    )
    parser.add_argument(
        "--proto", choices=PROTOCOLS, default=None, help=help_text("proto", "share protocol")
    )
    parser.add_argument(
        "--security", "-S", choices=tuple(SECURITY_MODELS), default=None,
        help=help_text("security", "QEMU security model, no root needed"),
    )
    parser.add_argument(
        "--mount-point", default=None, help=help_text("mount-point", "guest mount point")
    )
    parser.add_argument("--driver", default=None, help=help_text("driver", "guest driver name"))
    parser.add_argument(
        "--driver-bin", default=None,
        help=help_text("driver-bin", "absolute host path of the driver binary"),
    )


def _add_run_options(parser: argparse.ArgumentParser) -> None:
    """Register the options that control a scripted QEMU run (--run)."""
    parser.add_argument(
        "--qemu-arg",
        action="append",
        default=[],
        metavar="ARG",
        help=(
            "extra QEMU argument, repeatable (appended after the share options). "
            "Used for a direct-kernel boot, e.g. --qemu-arg=-initrd "
            "--qemu-arg=/tmp/initramfs.cpio.gz"
        ),
    )
    parser.add_argument(
        "--wait-for",
        metavar="MARKER",
        help="consider the run successful when MARKER appears on the console",
    )
    parser.add_argument(
        "--console-log",
        metavar="FILE",
        help="write the guest console here (default: /tmp/pgw-qemu-console.log)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=None,
        help=pgwconfig.help_text("timeout", "seconds to wait for the console marker"),
    )


def _add_action_options(parser: argparse.ArgumentParser) -> None:
    """Register the action options (emit, image, run, demo, examples)."""
    parser.add_argument("--emit", action="store_true", help="print one shell-ready option line")
    parser.add_argument("--emit-image", action="store_true", help="print the mkqnximage snippets")
    parser.add_argument(
        "--mkqnximage", metavar="VM_DIR", help="write snippets into <VM_DIR>/local/snippets"
    )
    parser.add_argument("--ifs", action="store_true", help="use the IFS layout for the snippets")
    parser.add_argument("--run", metavar="IMAGE", help="start QEMU on IMAGE (kernel or disk)")
    parser.add_argument(
        "--arch", default=None,
        help=pgwconfig.help_text("arch", "guest architecture for --run"),
    )
    _add_run_options(parser)
    parser.add_argument(
        "--trace", metavar="LOG", help="write the 9P trace to LOG (pbench roundtrips)"
    )
    parser.add_argument("--demo", action="store_true", help="replay the demonstration (DX-02)")
    parser.add_argument(
        "--example",
        choices=("cmake", "rust", "all"),
        help="write the CMake/Rust example projects into the share (DX-04)",
    )
    parser.add_argument(
        "--security-help", action="store_true", help="explain the security models (SEC-03)"
    )


def _parser() -> argparse.ArgumentParser:
    """Argument parser for qshare."""
    parser = argparse.ArgumentParser(
        prog="qshare",
        description="Share a host directory with a QNX guest under QEMU (DX-01)",
        epilog="subcommands: " + ", ".join(f"qshare {name}" for name in COMMANDS),
    )
    parser.add_argument(
        "--version",
        action="version",
        version=f"qshare {_version_string()}",
        help="print the version and exit",
    )
    _add_share_options(parser)
    _add_action_options(parser)
    pgwconfig.add_show_config(parser)
    return parser


def run_command(name: str, argv: list[str]) -> int:
    """Run one `qshare <name>` subcommand (doctor, init, smoke).

    Each subcommand lives in its own module and brings its own parser, its
    options and its exit statuses; this dispatcher only maps the two errors
    every host tool reports the same way (invalid input, host failure).
    """
    module = importlib.import_module(f"qshare.{name}")
    parser = argparse.ArgumentParser(
        prog=f"qshare {name}", description=module.DESCRIPTION.splitlines()[0]
    )
    module.register(parser)
    args = parser.parse_args(argv)
    try:
        return module.run(args)
    except ValidationError as err:
        sys.stderr.write(f"qshare {name}: invalid input: {err}\n")
        return 2
    except HostError as err:
        sys.stderr.write(f"qshare {name}: {err}\n")
        return 3


def _emit_line(args: argparse.Namespace) -> int:
    """Print the QEMU options for the share."""
    cfg = validate_share(
        args.directory, tag=args.tag, security_model=args.security, proto=args.proto
    )
    argv = build_qemu_args(cfg)
    if args.emit:
        print(" ".join(argv))
        return 0
    print(f"# Passerelle share: {cfg.path}")
    print(f"# protocol={cfg.proto} tag={cfg.tag} security_model={cfg.security_model}")
    print(f"# guest: {args.driver} {cfg.tag} {args.mount_point}")
    print(" ".join(argv))
    if cfg.proto == "virtiofs":
        print("# virtiofsd must listen on the socket above; --run starts it for you")
    return 0


def _image_output(args: argparse.Namespace) -> int:
    """Emit or install the mkqnximage snippets."""
    validate_share(args.directory, tag=args.tag, proto=args.proto)
    fragment = build_fragment(
        driver_bin=args.driver_bin,
        tag=args.tag,
        mount_point=args.mount_point,
        driver_name=args.driver,
        layout="ifs" if args.ifs else "system",
    )
    if args.mkqnximage:
        for path in install_fragment(args.mkqnximage, fragment):
            print(f"wrote {path}")
        return 0
    print(render_fragment(fragment))
    return 0


def _qemu_command(args: argparse.Namespace, cfg: object) -> list[str]:
    """Build the QEMU command line used by the --run and --demo paths."""
    target_arch = args.arch
    return build_qemu_cmd(
        cfg=cfg,
        image=args.run,
        arch=target_arch,
        trace_log=args.trace,
        extra_args=tuple(args.qemu_arg),
    )


def _run(args: argparse.Namespace) -> int:
    """Start QEMU on the produced image, with virtiofsd when needed."""
    cfg = validate_share(
        args.directory, tag=args.tag, security_model=args.security, proto=args.proto
    )
    cmd = _qemu_command(args, cfg)
    print("+ " + " ".join(cmd))
    console_log = pathlib.Path(args.console_log or "/tmp/pgw-qemu-console.log")
    marker = args.wait_for
    with share_backend(cfg):
        instance = QemuInstance(cmd, console_log=console_log)
        instance.start()
        try:
            if marker:
                # Scripted boot (tests/qemu): the guest prints a marker and powers
                # off, so success is the marker, not a fixed banner.
                if not instance.wait_for_console(marker, timeout=args.timeout):
                    raise HostError(
                        f"marker {marker!r} not seen within {args.timeout}s; "
                        f"console: {console_log}"
                    )
                print(f"marker seen: {marker}")
            else:
                instance.wait_for_console("Startup complete", timeout=args.timeout)
        finally:
            instance.stop()
    return 0


def _demo(args: argparse.Namespace) -> int:
    """Replay the whole demonstration, one command (DX-02)."""
    from qshare.demo import run_demo

    target_arch = args.arch
    return run_demo(
        directory=args.directory or ".",
        tag=args.tag,
        proto=args.proto,
        image=args.run,
        arch=target_arch,
    )


def _examples(args: argparse.Namespace) -> int:
    """Write the CMake and/or Rust example projects into the share."""
    from qshare.examples import write_all, write_example

    validate_share(args.directory)
    if args.example == "all":
        written = write_all(args.directory)
    else:
        written = [write_example(args.example, args.directory)]
    for example in written:
        print(f"{example.kind}: {len(example.files)} files in {example.directory}")
        print(f"  guest: {example.run_command()}")
    return 0


def main(argv: list[str] | None = None) -> int:
    """Entry point of the qshare command."""
    words = list(sys.argv[1:] if argv is None else argv)
    if words and words[0] in COMMANDS:
        return run_command(words[0], words[1:])
    args = _parser().parse_args(words)
    try:
        cfg = pgwconfig.apply(args, pgwconfig.load(TOOL))
        if args.show_config:
            print(pgwconfig.render(cfg, args.show_config))
            return 0
        if args.security_help:
            print(describe_security_models())
            return 0
        if args.demo:
            return _demo(args)
        if not args.directory:
            sys.stderr.write("qshare: a share directory is required (or use --demo)\n")
            return 2
        if args.example:
            return _examples(args)
        if args.mkqnximage or args.emit_image:
            return _image_output(args)
        if args.run:
            return _run(args)
        return _emit_line(args)
    except ValidationError as err:
        sys.stderr.write(f"qshare: invalid input: {err}\n")
        return 2
    except HostError as err:
        sys.stderr.write(f"qshare: {err}\n")
        return 3


if __name__ == "__main__":  # pragma: no cover - console entry point
    raise SystemExit(main())

# SPDX-License-Identifier: Apache-2.0
"""Command line interface of qshare (DX-01, VFS-06, SEC-03, DX-02).

`qshare <dir>` prints the exact QEMU options for a 9P or virtiofs share;
`--emit` prints them as one shell-ready line, `--emit-image` or
`--mkqnximage` produce the mkqnximage snippets, `--run` starts QEMU (and
virtiofsd) itself, and `--demo` replays the whole demonstration (DX-02).
"""

from __future__ import annotations

import argparse
import pathlib
import sys

from pgwutil import HostError, ValidationError
from qshare.image import build_fragment, install_fragment, render_fragment
from qshare.options import (
    DEFAULT_TAG,
    PROTOCOLS,
    RECOMMENDED_SECURITY,
    SECURITY_MODELS,
    build_qemu_args,
    describe_security_models,
    validate_share,
)
from qshare.runner import QemuInstance, build_qemu_cmd, share_backend


def _add_share_options(parser: argparse.ArgumentParser) -> None:
    """Register the share-definition options (directory, tag, protocol, model)."""
    parser.add_argument("directory", nargs="?", help="host directory to share")
    parser.add_argument(
        "--tag", "-t", default=DEFAULT_TAG, help="mount tag (9P <=31 bytes, virtiofs <=36)"
    )
    parser.add_argument(
        "--proto", choices=PROTOCOLS, default="9p", help="share protocol (default: 9p)"
    )
    parser.add_argument(
        "--security",
        "-S",
        choices=tuple(SECURITY_MODELS),
        default=RECOMMENDED_SECURITY,
        help=f"QEMU security model (default: {RECOMMENDED_SECURITY}, no root needed)",
    )
    parser.add_argument("--mount-point", default="/host", help="guest mount point")
    parser.add_argument("--driver", default="passerelle", help="guest driver name")
    parser.add_argument(
        "--driver-bin",
        default="/opt/passerelle/sbin/passerelle",
        help="absolute host path of the driver binary",
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
        default=120.0,
        help="seconds to wait for the console marker (default: 120)",
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
    parser.add_argument("--arch", default="x86_64", help="guest architecture for --run")
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
    )
    _add_share_options(parser)
    _add_action_options(parser)
    return parser


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
    args = _parser().parse_args(argv)
    if args.security_help:
        print(describe_security_models())
        return 0
    if args.demo:
        return _demo(args)
    if not args.directory:
        sys.stderr.write("qshare: a share directory is required (or use --demo)\n")
        return 2
    try:
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

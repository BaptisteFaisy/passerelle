# SPDX-License-Identifier: Apache-2.0
"""qrun - run a program from the share inside the guest (DX-03).

The command sends one line to the guest console, marks the end of the
output with a unique sentinel, and returns the guest's stdout, stderr and
exit status. Two transports are supported:

  * console -- the guest console is a unix socket served by QEMU
    (-serial unix:<path>,server=on,wait=off); the guest prints through it;
  * local   -- the same program is executed on the PC, used by the demo
    and by the tests when no guest is running.

The wire format between qrun and the guest helper is deliberately trivial
so that a shell script is enough on the QNX side:

    qrun-begin <token>
    <command>
    qrun-rc <token> <status>
    qrun-end <token>
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import shlex
import socket
import sys
import time
from dataclasses import dataclass

from pgwutil import HostError, ValidationError, run_cmd

DEFAULT_TIMEOUT = 120.0
DEFAULT_PROMPT_TIMEOUT = 60.0


@dataclass(frozen=True)
class QRunResult:
    """Outcome of one guest (or local) program execution."""

    exit_code: int
    stdout: str
    stderr: str
    duration_s: float
    timed_out: bool = False


def build_guest_line(program: str, args: list[str], token: str) -> str:
    """Command line sent to the guest, framed by the qrun markers."""
    inner = " ".join([shlex.quote(program)] + [shlex.quote(a) for a in args])
    return (
        f"echo qrun-begin {token}\r"
        f"{inner}\r"
        f"echo qrun-rc {token} $?\r"
        f"echo qrun-end {token}\r"
    )


def parse_guest_output(text: str, token: str) -> tuple[str, int] | None:
    """Extract (stdout, exit code) from a console transcript, or None."""
    begin = re.search(rf"qrun-begin {token}\r?\n", text)
    if begin is None:
        return None
    rest = text[begin.end():]
    end = re.search(rf"qrun-end {token}", rest)
    rc = re.search(rf"qrun-rc {token} (\d+)", rest)
    if end is None or rc is None:
        return None
    body = rest[: end.start()]
    rc_line = re.search(rf"qrun-rc {token} \d+", body)
    if rc_line is not None:
        body = body[: rc_line.start()]
    return body, int(rc.group(1))


def run_local(share_dir: pathlib.Path, program: str, args: list[str]) -> QRunResult:
    """Execute the program directly on the PC (demo and tests)."""
    target = pathlib.Path(program)
    if not target.is_absolute():
        target = share_dir / program
    if not target.exists():
        raise ValidationError(f"program not found in the share: {target}")
    started = time.monotonic()
    proc = run_cmd([str(target), *args], cwd=share_dir)
    return QRunResult(
        exit_code=proc.returncode,
        stdout=proc.stdout,
        stderr=proc.stderr,
        duration_s=time.monotonic() - started,
    )


class ConsoleClient:
    """Minimal line-oriented client for a QEMU unix console socket."""

    def __init__(self, socket_path: pathlib.Path, read_timeout: float = 1.0) -> None:
        self.socket_path = socket_path
        self.read_timeout = read_timeout

    def transact(self, payload: str, token: str, timeout: float) -> tuple[str, int]:
        """Send *payload* and read the transcript until the end marker."""
        deadline = time.monotonic() + timeout
        buffer = ""
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(self.read_timeout)
            sock.connect(str(self.socket_path))
            sock.sendall(payload.encode("utf-8"))
            while time.monotonic() < deadline:
                try:
                    chunk = sock.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                buffer += chunk.decode("utf-8", errors="replace")
                parsed = parse_guest_output(buffer, token)
                if parsed is not None:
                    return parsed
            raise HostError(
                f"guest did not answer within {timeout:.0f}s (markers not seen): "
                f"{buffer[-200:]!r}"
            )


def run_guest(
    socket_path: pathlib.Path, program: str, args: list[str], timeout: float
) -> QRunResult:
    """Run *program* inside the guest through its console socket."""
    token = f"{os.getpid()}-{int(time.time() * 1000) % 100000}"
    payload = build_guest_line(program, args, token)
    started = time.monotonic()
    try:
        stdout, rc = ConsoleClient(socket_path).transact(payload, token, timeout)
    except socket.timeout:
        return QRunResult(124, "", f"qrun: timeout after {timeout:.0f}s", time.monotonic() - started, True)
    return QRunResult(rc, stdout, "", time.monotonic() - started)


def _parser() -> argparse.ArgumentParser:
    """Argument parser for the qrun command."""
    parser = argparse.ArgumentParser(
        prog="qrun", description="Run a program from the share in the guest (DX-03)"
    )
    parser.add_argument("program", help="program path, relative to the share")
    parser.add_argument("args", nargs=argparse.REMAINDER, help="arguments for the program")
    parser.add_argument("--share", "-s", default=".", help="shared directory (default: .)")
    parser.add_argument(
        "--console",
        help="guest console unix socket served by QEMU (-serial unix:<path>)",
    )
    parser.add_argument(
        "--local",
        action="store_true",
        help="run the program on the PC instead of inside the guest",
    )
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="timeout in seconds")
    return parser


def main(argv: list[str] | None = None) -> int:
    """Entry point of the qrun command."""
    args = _parser().parse_args(argv)
    share = pathlib.Path(args.share).expanduser()
    if not share.is_dir():
        sys.stderr.write(f"qrun: share directory does not exist: {share}\n")
        return 2
    try:
        if args.local or not args.console:
            if not args.local:
                sys.stderr.write(
                    "qrun: no --console given, running locally (use --console for the guest)\n"
                )
            result = run_local(share.resolve(), args.program, args.args)
        else:
            result = run_guest(
                pathlib.Path(args.console), args.program, args.args, args.timeout
            )
    except (ValidationError, HostError, OSError) as err:
        sys.stderr.write(f"qrun: {err}\n")
        return 2
    sys.stdout.write(result.stdout)
    if result.stderr:
        sys.stderr.write(result.stderr)
    return result.exit_code

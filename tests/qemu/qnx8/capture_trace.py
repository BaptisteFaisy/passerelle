#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive the QNX 8.0 guest and record what it answers (measurements, trace).

The guest boots with `run-guest.sh`, whose console can be wired to a unix socket
(`SERIAL=unix:...`). This harness connects to that socket, waits for the mount to
be up, sends the commands it is given and records everything the guest prints.
It is the piece the measurement runs were missing: until now a measurement meant
typing commands at a console by hand, and a number nobody can rerun is not a
measurement.

Usage:
    python3 capture_trace.py --cmd '/proc/boot/qtrace.sh' \
            --out /tmp/pgw-trace/console.log
    python3 capture_trace.py --cmd 'qbench rate /host/big.bin' --repeat 3

The socket driving reuses console.py (connect, read-until-quiet), so the wire
format of the guest console is defined once.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
import time
import tty
from typing import Sequence

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import console as guest_console  # noqa: E402  (same directory, by design)

BOOT_MARKER = "Passerelle: startup complete"
READY_PROBE = "cat /host/from-host.txt"
READY_NEEDLE = "hello-from-the-pc"
RESULT_MARKER = "PGW-TRACE RESULT"  # what a measurement script ends with
QUIET = 1.5  # seconds of silence that mean the guest has finished answering


def send(sock, log, cmd: str, quiet: float = QUIET) -> str:
    """One command, and everything the guest answers before going quiet."""
    sock.sendall((cmd + "\n").encode("utf-8"))
    return guest_console.read_until_quiet(sock, log, quiet)


def workdir(path: str | None) -> pathlib.Path:
    """The directory the run writes into (a temp one by default)."""
    if path:
        w = pathlib.Path(path).expanduser()
        w.mkdir(parents=True, exist_ok=True)
        return w.resolve()
    return pathlib.Path(subprocess.run(
        ["python3", "-c", "import tempfile; print(tempfile.mkdtemp())"],
        capture_output=True, text=True, check=True).stdout.strip())


def start_guest(work: pathlib.Path, timeout: int, tag: str,
                image: pathlib.Path, share: pathlib.Path) -> subprocess.Popen:
    """Boot the guest with an interactive console, in the background.

    Only the process this harness started is ever signalled: the machine runs
    other projects' guests and none of them is ours to stop.
    """
    env = dict(os.environ)
    env["SERIAL"] = "pty"
    env["RUN_TIMEOUT"] = str(timeout)
    env["TAG"] = tag
    env["QEMU_PID_FILE"] = str(work / "qemu.pid")
    log = open(work / "guest.log", "wb")
    proc = subprocess.Popen(
        [str(HERE / "run-guest.sh"), str(image), str(share),
         str(work / "qemu-console.log")],
        stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
    proc._work = work  # type: ignore[attr-defined]
    return proc


class PtyConsole:
    """A pty master that reads like the socket console.py already drives.

    QEMU's `-serial unix:` drops every byte written before a client connects, so
    a guest that boots faster than the harness connects prints nothing at all
    (measured 14 November 2026). The kernel's pty buffers instead, so the harness
    can open the master late and still read the boot banner.

    It speaks the socket's interface (`recv`, `settimeout`, `sendall`) and raises
    `socket.timeout` so console.py's read-until-quiet rule stays in one place.
    """

    def __init__(self, path: str) -> None:
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        self._timeout = 0.4

    def settimeout(self, secs: float) -> None:
        self._timeout = secs

    def recv(self, size: int) -> bytes:
        ready, _, _ = select.select([self.fd], [], [], self._timeout)
        if not ready:
            raise socket.timeout()
        return os.read(self.fd, size)

    def sendall(self, data: bytes) -> None:
        os.write(self.fd, data)

    def close(self) -> None:
        os.close(self.fd)


def wait_for_pty(guest_log: pathlib.Path, deadline: float) -> str:
    """Read QEMU's own message for the pty it created (`/dev/pts/N`)."""
    while time.time() < deadline:
        try:
            text = guest_log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            text = ""
        m = re.search(r"/dev/pts/\d+", text)
        if m:
            return m.group(0)
        time.sleep(0.1)
    return ""


def console_read(sock, log, quiet: float) -> str:
    """One read-until-quiet round, with the log kept open for the caller."""
    return guest_console.read_until_quiet(sock, log, quiet)


def wait_ready(sock, log, deadline: float, skip_ready: bool) -> tuple[str, bool]:
    """Wait for the guest to boot and the share to answer; return the transcript.

    The mount is asynchronous and the driver runs in the foreground of a
    background script, so "ready" cannot be a line someone printed: it is the
    share answering a read. The probe is what says the driver is serving.
    """
    text = ""
    while time.time() < deadline and BOOT_MARKER not in text:
        text += console_read(sock, log, QUIET)
    if BOOT_MARKER not in text:
        log.write("\nCAPTURE: the guest never finished booting\n")
        return text, False
    if skip_ready:
        return text, True
    while time.time() < deadline:
        text += send(sock, log, READY_PROBE)
        if READY_NEEDLE in text:
            return text, True
    log.write("\nCAPTURE: /host never answered a read\n")
    return text, False


def run_commands(sock, log, commands: Sequence[str], deadline: float,
                 marker: str) -> tuple[str, bool]:
    """Run the commands and wait for the run's own last line."""
    text = ""
    for cmd in commands:
        text += send(sock, log, cmd)
    # Silence is not an answer: a measurement script prints nothing for seconds
    # while its benchmark runs, so the run ends on the script's own last line,
    # not on a gap in the console.
    if not marker:
        return text, True
    while time.time() < deadline and marker not in text:
        text += console_read(sock, log, QUIET)
    if marker not in text:
        log.write(f"\nCAPTURE: no {marker!r} before the deadline\n")
        return text, False
    return text, True


def drive(work: pathlib.Path, commands: Sequence[str], out: pathlib.Path,
          deadline: float, skip_ready: bool = False,
          marker: str = RESULT_MARKER) -> int:
    """Boot the guest, talk to it, and record everything it answered.

    `skip_ready` runs the commands anyway, for a guest booted without a share
    that must still be talked to (an A/B measurement with the driver absent).
    """
    pty_path = wait_for_pty(work / "guest.log", deadline)
    if not pty_path:
        with open(out, "w", encoding="utf-8") as log:
            log.write("CAPTURE: QEMU announced no pty\n")
        return 1
    with open(out, "w", encoding="utf-8") as log:
        sock = PtyConsole(pty_path)
        try:
            _, ready = wait_ready(sock, log, deadline, skip_ready)
            if not ready:
                return 1
            _, done = run_commands(sock, log, commands, deadline, marker)
            return 0 if done else 1
        finally:
            sock.close()


def ensure_rate_file(share: pathlib.Path, mib: int) -> pathlib.Path:
    """The benchmark needs a bulk file in the share (PERF-03, QNX-04).

    Created once and left in place: a rerun measures the same bytes, and the
    host's page cache is warm for both guests, which is what makes the QNX row
    and the Linux row comparable.
    """
    f = share / "big.bin"
    if f.exists() and f.stat().st_size == mib * 1024 * 1024:
        return f
    share.mkdir(parents=True, exist_ok=True)
    with open(f, "wb") as h:
        block = bytes((i & 0xFF) for i in range(64 * 1024))
        for _ in range(mib * 16):
            h.write(block)
    return f


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cmd", action="append", default=[],
                    help="command to send to the guest (repeatable)")
    ap.add_argument("--out", default="", help="where to write the transcript")
    ap.add_argument("--workdir", default="", help="artifacts directory")
    ap.add_argument("--tag", default="hostshare", help="mount tag")
    ap.add_argument("--image", default="",
                    help="IFS to boot (default: <repo>/build/qnx8/passerelle.ifs)")
    ap.add_argument("--share", default="",
                    help="host directory to share (default: <repo>/build/qnx8/share)")
    ap.add_argument("--size-mib", type=int, default=64,
                    help="size of the bulk file the benchmark reads (default: 64)")
    ap.add_argument("--timeout", type=int, default=180,
                    help="seconds the whole run may take")
    ap.add_argument("--keep", action="store_true",
                    help="leave the artifacts directory in place")
    ap.add_argument("--skip-ready", action="store_true",
                    help="do not wait for the share to answer (a guest booted "
                         "without a driver, for an A/B measurement)")
    ap.add_argument("--marker", default=RESULT_MARKER,
                    help="line the run ends on (empty: end on console silence)")
    return ap.parse_args()


def main() -> int:
    args = parse_args()
    if shutil.which("qemu-system-x86_64") is None:
        print("capture: qemu-system-x86_64 is missing", file=sys.stderr)
        return 2
    root = HERE.parents[2]
    image = pathlib.Path(args.image) if args.image else root / "build/qnx8/passerelle.ifs"
    share = pathlib.Path(args.share) if args.share else root / "build/qnx8/share"
    if not image.exists():
        print(f"capture: no image at {image} (run build-image.sh)", file=sys.stderr)
        return 2
    return run_capture(args, image, share)


def run_capture(args: argparse.Namespace, image: pathlib.Path,
                share: pathlib.Path) -> int:
    """Prepare the shared file, boot the guest and leave the transcript."""
    work = workdir(args.workdir or None)
    out = pathlib.Path(args.out) if args.out else work / "transcript.log"
    commands = args.cmd or ["/proc/boot/qtrace.sh"]
    deadline = time.time() + args.timeout
    ensure_rate_file(share, args.size_mib)
    proc = start_guest(work, args.timeout + 15, args.tag, image, share)
    try:
        rc = drive(work, commands, out, deadline, args.skip_ready, args.marker)
    finally:
        stop_guest(proc)
    print(f"capture: transcript in {out}")
    if not args.keep and not args.workdir:
        shutil.rmtree(work, ignore_errors=True)
    return rc


def stop_guest(proc: subprocess.Popen) -> None:
    """Stop only the guest this harness started, then reap the shell wrapper.

    QEMU writes its own pidfile, so the pid read here belongs to this guest and
    to no other. The file can still be missing (the guest never started), and
    then the wrapper is signalled instead.
    """
    work = pathlib.Path(getattr(proc, "_work"))
    pid_file = work / "qemu.pid"
    if pid_file.exists():
        try:
            os.kill(int(pid_file.read_text(encoding="ascii").strip()), signal.SIGTERM)
        except (ValueError, ProcessLookupError, PermissionError):
            pass
    if proc.poll() is None:
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())

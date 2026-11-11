# SPDX-License-Identifier: Apache-2.0
"""Process control for virtiofsd and QEMU.

Covers DX-01 (`--run`), DX-02 (one-command demo), DX-03 (qrun needs a live
guest) and VFS-06 (qshare starts virtiofsd and shares guest memory).

Everything here is standard library only: Popen, sockets, signals.
"""

from __future__ import annotations

import contextlib
import pathlib
import signal
import subprocess
import time
from typing import Iterator, Sequence

from pgwutil import HostError, which
from qshare.options import ShareConfig, build_qemu_args, default_socket_path

VIRTIOFSD_CANDIDATES = (
    "/usr/lib/qemu/virtiofsd",
    "/usr/libexec/virtiofsd",
    "/usr/bin/virtiofsd",
    "/usr/local/bin/virtiofsd",
)


def find_virtiofsd() -> pathlib.Path | None:
    """Locate the virtiofsd daemon (distribution paths, then PATH)."""
    return which("virtiofsd", VIRTIOFSD_CANDIDATES)


def wait_for_path(path: pathlib.Path, timeout: float) -> bool:
    """Poll *path* until it exists; return False on timeout."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return True
        time.sleep(0.05)
    return False


class VirtiofsdDaemon:
    """Owns one virtiofsd process listening on a vhost-user socket."""

    def __init__(self, cfg: ShareConfig, start_timeout: float = 10.0) -> None:
        if cfg.proto != "virtiofs":
            raise HostError("VirtiofsdDaemon requires a virtiofs ShareConfig")
        self.cfg = cfg
        self.socket_path = cfg.socket_path or default_socket_path(cfg.tag)
        self.start_timeout = start_timeout
        self.process: subprocess.Popen[bytes] | None = None
        self.log_path = pathlib.Path("/tmp") / f"pgw-virtiofsd-{cfg.tag}.log"

    def command(self) -> list[str]:
        """Return the virtiofsd command line."""
        binary = find_virtiofsd()
        if binary is None:
            raise HostError(
                "virtiofsd not found; install qemu-system-common or qemu-virtiofsd"
            )
        return [
            str(binary),
            f"--socket-path={self.socket_path}",
            f"--shared-dir={self.cfg.path}",
            f"--tag={self.cfg.tag}",
            "--cache=never",
        ]

    def start(self) -> pathlib.Path:
        """Start virtiofsd and wait until its socket is ready."""
        self.socket_path.unlink(missing_ok=True)
        with open(self.log_path, "wb") as log:
            self.process = subprocess.Popen(
                self.command(), stdout=log, stderr=log, start_new_session=True
            )
        if not wait_for_path(self.socket_path, self.start_timeout):
            detail = self.log_tail()
            self.stop()
            raise HostError(f"virtiofsd did not create {self.socket_path}: {detail}")
        return self.socket_path

    def log_tail(self, lines: int = 5) -> str:
        """Return the last lines of the daemon log, for error messages."""
        if not self.log_path.exists():
            return ""
        text = self.log_path.read_text(encoding="utf-8", errors="replace")
        return " | ".join(text.strip().splitlines()[-lines:])

    def alive(self) -> bool:
        """True while the daemon process runs."""
        return self.process is not None and self.process.poll() is None

    def stop(self) -> None:
        """Terminate the daemon and remove its socket."""
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5.0)
        self.process = None
        self.socket_path.unlink(missing_ok=True)

    def __enter__(self) -> VirtiofsdDaemon:
        self.start()
        return self

    def __exit__(self, *exc: object) -> None:
        self.stop()


@contextlib.contextmanager
def share_backend(cfg: ShareConfig) -> Iterator[None]:
    """Start virtiofsd for a virtiofs config, and stop it afterwards."""
    if cfg.proto != "virtiofs":
        yield
        return
    daemon = VirtiofsdDaemon(cfg)
    daemon.start()
    try:
        yield
    finally:
        daemon.stop()


def build_qemu_cmd(
    cfg: ShareConfig,
    image: pathlib.Path | str,
    arch: str = "x86_64",
    trace_log: pathlib.Path | str | None = None,
    extra_args: Sequence[str] = (),
    memory: str = "1G",
) -> list[str]:
    """Build the full QEMU command line for *image* with the share attached."""
    target = pathlib.Path(image).expanduser().resolve()
    if not target.exists():
        raise HostError(f"image not found: {target}")
    cmd = [
        f"qemu-system-{arch}",
        "-m", memory,
        "-smp", "2",
        "-machine", "q35",
        "-nodefaults",
        "-nographic",
        "-serial", "mon:stdio",
    ]
    if target.suffix in (".bin", ".ifs") or "ifs" in target.name:
        cmd += ["-kernel", str(target)]
    else:
        cmd += ["-drive", f"file={target},if=virtio,format=raw"]
    cmd += build_qemu_args(cfg)
    if trace_log is not None:
        trace_path = pathlib.Path(trace_log).resolve()
        trace_path.parent.mkdir(parents=True, exist_ok=True)
        # Ubuntu's QEMU 8.2 builds trace events through the `log` backend,
        # which writes plain text to the file named by `-D`: `-trace file=...`
        # is the simpletrace binary format, not text, and the rest of the
        # pipeline expects text lines (`v9fs_*`). We pass both: `-D` for the
        # text log that pbench reads, `-trace enable=v9fs_*` to enable the
        # events.
        cmd += [
            "-D", str(trace_path),
            "-trace", "enable=v9fs_*",
        ]
    cmd += list(extra_args)
    return cmd


class QemuInstance:
    """A headless QEMU process whose console is captured to a file."""

    def __init__(self, cmd: Sequence[str], console_log: pathlib.Path | str) -> None:
        self.cmd = list(cmd)
        self.console_log = pathlib.Path(console_log).expanduser().resolve()
        self.process: subprocess.Popen[bytes] | None = None

    def start(self) -> None:
        """Start QEMU detached from the current terminal."""
        self.console_log.parent.mkdir(parents=True, exist_ok=True)
        handle = open(self.console_log, "wb")
        try:
            self.process = subprocess.Popen(
                self.cmd, stdout=handle, stderr=subprocess.STDOUT, start_new_session=True
            )
        finally:
            handle.close()

    def wait_for_console(self, needle: str, timeout: float = 60.0) -> bool:
        """Wait until *needle* appears in the console log."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.console_log.exists():
                text = self.console_log.read_text(encoding="utf-8", errors="replace")
                if needle in text:
                    return True
            if self.process is not None and self.process.poll() is not None:
                return False
            time.sleep(0.1)
        return False

    def alive(self) -> bool:
        """True while QEMU runs."""
        return self.process is not None and self.process.poll() is None

    def stop(self) -> None:
        """Send SIGTERM, then SIGKILL after a grace period."""
        if self.process is None:
            return
        if self.process.poll() is None:
            with contextlib.suppress(ProcessLookupError):
                self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=10.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5.0)
        self.process = None

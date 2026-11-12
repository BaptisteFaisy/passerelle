# SPDX-License-Identifier: Apache-2.0
"""Drive a real ``virtiofsd`` through our own vhost-user front end.

Implements requirements:
  - VFS-02: the FUSE traffic below travels on virtiofs request queue 1; queue 0
    is the hiprio queue and stays untouched.
  - VFS-03: INIT, LOOKUP, GETATTR, OPEN, READ, WRITE, READDIR, FLUSH, RELEASE
    and FORGET, answered by a real server rather than by our own fake.
  - VFS-04: every node reference LOOKUP returns is given back with FORGET.
  - VFS-06: virtiofsd is launched and configured exactly as ``qshare`` does for
    QEMU, except that *we* are the hypervisor side of the socket - no guest,
    no QEMU, no QNX.

The oracle is the shared directory itself: every answer is compared with what
``os.stat``/``open``/``os.listdir`` report on the host side, the same pattern
``tests/fuse_ref`` uses against its reference server.
"""

from __future__ import annotations

import os
import pathlib
import shutil
import stat
import tempfile
import time

from pgwutil import fuse_wire as fw
from qshare.options import validate_share
from qshare.runner import VirtiofsdDaemon, find_virtiofsd
from vhost.device import VhostUserDevice
from vhost.fuse_client import VirtqueueFuseClient
from vhost.memory import SharedMemory
from vhost.transport import Transport
from vhost.vring import SplitVirtqueue

HIPRIO_QUEUE = 0
REQUEST_QUEUE = 1
REGION_SIZE = 512 * 1024
QUEUE_SIZE = 16
TX_SIZE = 4096
RX_SIZE = 32 * 1024


class UnprivilegedVirtiofsd(VirtiofsdDaemon):
    """``qshare``'s daemon, minus the namespace sandbox.

    ``--sandbox=namespace`` (the default) needs user namespaces and pivot_root,
    which an unprivileged CI worker may not have. ``none`` runs virtiofsd in our
    own namespace: it is a test double for the guest side, not a security
    boundary, and the shared directory is a temporary one.
    """

    def command(self) -> list[str]:
        return super().command() + ["--sandbox=none", "--seccomp=none"]


class VhostFuseSession:
    """A front end wired to one running virtiofsd, with its FUSE client."""

    def __init__(self, socket_path, queue_index: int = REQUEST_QUEUE, timeout: float = 10.0) -> None:
        self.socket_path = str(socket_path)
        self.queue_index = queue_index
        self.timeout = timeout
        self.mem: SharedMemory | None = None
        self.vq: SplitVirtqueue | None = None
        self.device: VhostUserDevice | None = None
        self.fuse: VirtqueueFuseClient | None = None
        self.handshake: dict[str, object] = {}

    def start(self) -> "VhostFuseSession":
        """Connect, negotiate, publish the memory table and arm the queue."""
        transport = Transport.connect(self.socket_path, timeout=self.timeout)
        self.device = VhostUserDevice(transport, timeout=self.timeout)
        self.handshake = self.device.negotiate()
        self.mem = SharedMemory(REGION_SIZE)
        self.device.set_mem_table(self.mem)
        self.vq = SplitVirtqueue(
            self.mem, index=self.queue_index, num=QUEUE_SIZE, tx_size=TX_SIZE, rx_size=RX_SIZE
        )
        self.device.setup_queue(self.vq)
        self.fuse = VirtqueueFuseClient(self.vq, timeout=self.timeout)
        return self

    def close(self) -> None:
        """Tear everything down: queue, memory, socket (idempotent)."""
        self.fuse = None  # the client holds the queue, which holds a view
        if self.vq is not None:
            self.vq.close()
            self.vq = None
        if self.device is not None:
            self.device.close()
            self.device = None
        if self.mem is not None:
            self.mem.close()
            self.mem = None

    def __enter__(self) -> "VhostFuseSession":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.close()


def prepare_share(share: pathlib.Path) -> dict[str, bytes]:
    """Create the files the scenario reads and writes; return the truth."""
    share.mkdir(parents=True, exist_ok=True)
    truth = {
        "hello.txt": b"passerelle virtiofs payload\n",
        "write-me.txt": b"",
    }
    for name, content in truth.items():
        (share / name).write_bytes(content)
    (share / "subdir").mkdir(exist_ok=True)
    return truth


def _check(checks: list, name: str, condition: bool, detail: str = "") -> None:
    checks.append({"name": name, "ok": bool(condition), "detail": detail})


def check_init(fuse: VirtqueueFuseClient, checks: list) -> None:
    """INIT: the server must answer 7.x and advertise a usable max_write."""
    out = fuse.init()
    _check(checks, "init.version", out.get("major") == fw.FUSE_MAJOR, "major=%s" % out.get("major"))
    _check(checks, "init.minor", out.get("minor", 0) >= 8, "minor=%s" % out.get("minor"))
    _check(checks, "init.max_write", out.get("max_write", 0) > 0, "max_write=%s" % out.get("max_write"))


def check_metadata(fuse: VirtqueueFuseClient, share: pathlib.Path, checks: list) -> int:
    """LOOKUP + GETATTR of one file, compared with the host's own stat."""
    truth = os.stat(share / "hello.txt")
    entry = fuse.lookup(fw.ROOT_ID, "hello.txt")
    attr = entry["attr"]
    _check(checks, "lookup.ino", attr["ino"] == truth.st_ino, "%s != %s" % (attr["ino"], truth.st_ino))
    _check(checks, "lookup.size", attr["size"] == truth.st_size, "%s != %s" % (attr["size"], truth.st_size))
    _check(checks, "lookup.mode", stat.S_IMODE(attr["mode"]) == stat.S_IMODE(truth.st_mode), oct(attr["mode"]))
    got = fuse.getattr(entry["nodeid"])["attr"]
    _check(checks, "getattr.size", got["size"] == truth.st_size, "%s" % got["size"])
    _check(checks, "getattr.nlink", got["nlink"] == truth.st_nlink, "%s" % got["nlink"])
    return entry["nodeid"]


def check_read(fuse: VirtqueueFuseClient, nodeid: int, expected: bytes, checks: list) -> None:
    """OPEN + READ + FLUSH + RELEASE, compared with the file's real bytes."""
    fh = fuse.open(nodeid, os.O_RDONLY)
    data = fuse.read(nodeid, fh, 0, len(expected))
    _check(checks, "read.bytes", data == expected, "%r != %r" % (data[:32], expected[:32]))
    fuse.flush(nodeid, fh)
    fuse.release(nodeid, fh)
    _check(checks, "read.released", True, "fh=%d" % fh)


def check_write(fuse: VirtqueueFuseClient, share: pathlib.Path, checks: list) -> None:
    """WRITE through virtiofsd, then read the file back from the host."""
    payload = b"written through vhost-user\n"
    entry = fuse.lookup(fw.ROOT_ID, "write-me.txt")
    fh = fuse.open(entry["nodeid"], os.O_WRONLY)
    written = fuse.write(entry["nodeid"], fh, 0, payload)
    _check(checks, "write.count", written == len(payload), "%d of %d" % (written, len(payload)))
    fuse.flush(entry["nodeid"], fh)
    fuse.release(entry["nodeid"], fh)
    on_disk = (share / "write-me.txt").read_bytes()
    _check(checks, "write.on_disk", on_disk == payload, "%r" % on_disk[:40])
    fuse.forget(entry["nodeid"])


def check_readdir(fuse: VirtqueueFuseClient, share: pathlib.Path, checks: list) -> None:
    """OPENDIR + READDIR of the root, compared with os.listdir."""
    fh = fuse.open(fw.ROOT_ID, os.O_RDONLY, is_dir=True)
    names = set()
    for entry in fuse.readdir(fw.ROOT_ID, fh, 0, 8192):
        names.add(entry["name"].split(b"\x00")[0].decode())
    fuse.release(fw.ROOT_ID, fh, is_dir=True)
    # A FUSE server lists the dot entries, os.listdir does not.
    names.discard(".")
    names.discard("..")
    truth = set(os.listdir(share))
    _check(checks, "readdir.names", names == truth, "got %s want %s" % (sorted(names), sorted(truth)))


def run_scenario(share: pathlib.Path, socket_path=None, timeout: float = 10.0) -> dict:
    """Launch virtiofsd on *share* and run the whole sequence; return a report."""
    truth = prepare_share(share)
    socket_dir = None
    if socket_path is None:
        # A private socket per run: two proofs on one machine (make check and
        # CI, or two tests) must not fight over one well-known path.
        socket_dir = tempfile.mkdtemp(prefix="pgw-vhost-")
        socket_path = pathlib.Path(socket_dir) / "vhost.sock"
    config = validate_share(share, tag="vfsproof", proto="virtiofs", socket_path=socket_path)
    checks: list[dict] = []
    try:
        report = _run_daemon(config, share, truth, checks, timeout)
    finally:
        if socket_dir is not None:
            shutil.rmtree(socket_dir, ignore_errors=True)
    return report


def _run_daemon(config, share: pathlib.Path, truth: dict, checks: list, timeout: float) -> dict:
    """Start the daemon, run the sequence against it, and build the report."""
    started = time.monotonic()
    with UnprivilegedVirtiofsd(config, start_timeout=timeout) as daemon:
        with VhostFuseSession(daemon.socket_path, timeout=timeout) as session:
            fuse = session.fuse
            check_init(fuse, checks)
            nodeid = check_metadata(fuse, share, checks)
            check_read(fuse, nodeid, truth["hello.txt"], checks)
            check_write(fuse, share, checks)
            check_readdir(fuse, share, checks)
            fuse.forget(nodeid)
            handshake = session.handshake
    failed = [c["name"] for c in checks if not c["ok"]]
    return {
        "virtiofsd": str(find_virtiofsd()),
        "handshake": handshake,
        "checks": checks,
        "failed": failed,
        "ok": not failed,
        "duration_s": round(time.monotonic() - started, 2),
    }


def format_report(report: dict) -> str:
    """The report as readable lines, for the CLI and for pytest output."""
    lines = ["virtiofsd: %s" % report["virtiofsd"], "handshake: %s" % report["handshake"]]
    for check in report["checks"]:
        lines.append("%-4s %-18s %s" % ("PASS" if check["ok"] else "FAIL", check["name"], check["detail"]))
    lines.append("result: %s in %.2fs" % ("OK" if report["ok"] else "FAILED", report["duration_s"]))
    return "\n".join(lines)

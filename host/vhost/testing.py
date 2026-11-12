# SPDX-License-Identifier: Apache-2.0
"""Wiring helpers that put the front end face to face with the fake backend.

Implements requirements:
  - VFS-01, VFS-02, VFS-03: the unit tests of ``host/vhost`` need a complete
    session (socket, memory table, one request queue, FUSE client) without
    ``virtiofsd`` being installed; this module builds it in one call.
  - ROB-06: the same wiring is reused by every test, so a framing regression
    shows up everywhere at once instead of being hidden by a local shortcut.

Standard library only.
"""

from __future__ import annotations

import pathlib
import tempfile
from typing import Iterator

from vhost.device import VhostUserDevice
from vhost.fake_backend import FakeVhostBackend, default_responder
from vhost.fuse_client import VirtqueueFuseClient
from vhost.memory import SharedMemory
from vhost.transport import Transport
from vhost.vring import SplitVirtqueue

REGION_SIZE = 512 * 1024
SOCKET_DIR_PREFIX = "pgw-vhost-test-"
QUEUE_SIZE = 16
TX_SIZE = 4096
RX_SIZE = 32 * 1024


class FakeSession:
    """A front end and a fake backend connected over a temporary socket."""

    def __init__(
        self,
        socket_path: pathlib.Path | str,
        responder=default_responder,
        queue_index: int = 1,
        num_queues: int = 2,
        region_size: int = REGION_SIZE,
        timeout: float = 5.0,
    ) -> None:
        self.socket_path = str(socket_path)
        self.queue_index = queue_index
        self.region_size = region_size
        self.timeout = timeout
        self.backend = FakeVhostBackend(
            self.socket_path, responder=responder, num_queues=num_queues, queue_size=QUEUE_SIZE
        )
        self.device: VhostUserDevice | None = None
        self.mem: SharedMemory | None = None
        self.vq: SplitVirtqueue | None = None
        self.fuse: VirtqueueFuseClient | None = None

    def start(self) -> "FakeSession":
        """Start the backend, then negotiate and arm one request queue."""
        self.backend.start()
        transport = Transport.connect(self.socket_path, timeout=self.timeout)
        self.device = VhostUserDevice(transport, timeout=self.timeout)
        self.device.negotiate()
        self.mem = SharedMemory(self.region_size)
        self.device.set_mem_table(self.mem)
        self.vq = SplitVirtqueue(
            self.mem, index=self.queue_index, num=QUEUE_SIZE, tx_size=TX_SIZE, rx_size=RX_SIZE
        )
        self.device.setup_queue(self.vq)
        self.fuse = VirtqueueFuseClient(self.vq, timeout=self.timeout)
        return self

    def close(self) -> None:
        """Tear the session down, backend last (idempotent)."""
        self.fuse = None
        if self.vq is not None:
            self.vq.close()
            self.vq = None
        if self.device is not None:
            self.device.close()
            self.device = None
        if self.mem is not None:
            self.mem.close()
            self.mem = None
        self.backend.stop()
        self._remove_socket_dir()

    def _remove_socket_dir(self) -> None:
        """Drop the private directory ``temp_socket_path()`` made, if empty."""
        parent = pathlib.Path(self.socket_path).parent
        if not parent.name.startswith(SOCKET_DIR_PREFIX):
            return
        try:
            if not any(parent.iterdir()):
                parent.rmdir()
        except OSError:
            pass

    def __enter__(self) -> "FakeSession":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.close()


def temp_socket_path(prefix: str = SOCKET_DIR_PREFIX) -> pathlib.Path:
    """A short socket path in a private temporary directory (sun_path is 108).

    ``FakeSession.close()`` removes that directory again, so a test run leaves
    nothing behind in /tmp.
    """
    directory = pathlib.Path(tempfile.mkdtemp(prefix=prefix))
    return directory / "vhost.sock"


def session(responder=default_responder, **kwargs) -> Iterator[FakeSession]:
    """Context manager form of :class:`FakeSession` on a temporary socket."""
    path = temp_socket_path()
    with FakeSession(path, responder=responder, **kwargs) as fake:
        yield fake

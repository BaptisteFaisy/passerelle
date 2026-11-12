# SPDX-License-Identifier: Apache-2.0
"""UNIX socket transport for vhost-user messages, including SCM_RIGHTS fds.

Implements requirements:
  - VFS-06: the socket ``qshare``/the test driver and ``virtiofsd`` talk over.
  - ROB-06: a truncated header, an implausible size or a lost ancillary
    descriptor raises ``ProtocolError`` instead of being silently accepted.

One framing rule matters and is easy to get wrong: file descriptors travel as
ancillary data attached to *some* segment of the message, so both the header
read and the payload read use ``recvmsg`` and the descriptors of the two reads
are merged. Reading the header with a plain ``recv`` would discard them.
"""

from __future__ import annotations

import os
import select
import socket
import struct
from dataclasses import dataclass, field

from vhost.protocol import (
    FLAG_NEED_REPLY,
    FLAG_REPLY,
    FLAG_VERSION,
    HEADER_SIZE,
    MAX_ATTACHED_FDS,
    MAX_MSG_SIZE,
    ProtocolError,
    decode_header,
    decode_u64,
    encode_header,
)

_ANC_SPACE = socket.CMSG_SPACE(4 * MAX_ATTACHED_FDS)


@dataclass
class Message:
    """One vhost-user message as it crosses the socket."""

    request: int
    flags: int
    payload: bytes
    fds: list[int] = field(default_factory=list)

    @property
    def is_reply(self) -> bool:
        return bool(self.flags & FLAG_REPLY)

    @property
    def is_need_reply(self) -> bool:
        return bool(self.flags & FLAG_NEED_REPLY)

    def is_reply_for(self, request: int) -> bool:
        """True when this message answers the request *request*."""
        return self.is_reply and int(self.request) == int(request)

    def u64(self) -> int:
        """The payload read as one 64-bit value."""
        return decode_u64(self.payload)

    def close_fds(self) -> None:
        """Close every descriptor this message carried (avoids leaks)."""
        for fd in self.fds:
            try:
                os.close(fd)
            except OSError:
                pass
        self.fds = []


def _ancillary_fds(ancdata: list, flags: int) -> list[int]:
    """Extract the SCM_RIGHTS descriptors of one recvmsg call."""
    if flags & getattr(socket, "MSG_CTRUNC", 0):
        raise ProtocolError("ancillary data truncated: too many descriptors")
    fds: list[int] = []
    for level, ctype, data in ancdata:
        if level == socket.SOL_SOCKET and ctype == socket.SCM_RIGHTS:
            count = len(data) // 4
            fds.extend(struct.unpack("%di" % count, data[: 4 * count]))
    return fds


class Transport:
    """A connected vhost-user UNIX socket, both directions."""

    def __init__(self, sock: socket.socket) -> None:
        self._sock = sock
        self._timeout = 5.0

    @classmethod
    def connect(cls, path: str | os.PathLike, timeout: float = 10.0) -> "Transport":
        """Connect to a listening backend, waiting up to *timeout* seconds."""
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        try:
            sock.connect(os.fspath(path))
        except OSError:
            sock.close()
            raise
        sock.settimeout(None)
        return cls(sock)

    @classmethod
    def listen(cls, path: str | os.PathLike) -> socket.socket:
        """Create a listening UNIX socket at *path* (used by the fake backend)."""
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(os.fspath(path))
        listener.listen(1)
        return listener

    @property
    def fileno(self) -> int:
        return self._sock.fileno()

    def send(
        self,
        request: int,
        payload: bytes = b"",
        fds: tuple[int, ...] | list[int] = (),
        need_reply: bool = False,
        reply: bool = False,
    ) -> None:
        """Send one message, optionally passing descriptors over SCM_RIGHTS."""
        if len(payload) > MAX_MSG_SIZE:
            raise ProtocolError("payload of %d bytes exceeds %d" % (len(payload), MAX_MSG_SIZE))
        flags = FLAG_VERSION | (FLAG_NEED_REPLY if need_reply else 0) | (FLAG_REPLY if reply else 0)
        data = encode_header(request, flags, len(payload)) + payload
        if fds:
            if len(fds) > MAX_ATTACHED_FDS:
                raise ProtocolError("cannot pass %d descriptors at once" % len(fds))
            anc = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack("%di" % len(fds), *fds))]
            self._sock.sendmsg([data], anc)
        else:
            self._sock.sendall(data)

    def _recv_chunk(self, count: int) -> tuple[bytes, list[int]]:
        if not select.select([self._sock], [], [], self._timeout)[0]:
            raise TimeoutError("no vhost-user message within %ss" % self._timeout)
        data, ancdata, flags, _ = self._sock.recvmsg(count, _ANC_SPACE)
        if not data:
            raise ProtocolError("peer closed the vhost-user socket")
        return data, _ancillary_fds(ancdata, flags)

    def recv(self, timeout: float = 5.0) -> Message:
        """Read one message, waiting at most *timeout* seconds."""
        self._timeout = timeout
        header = b""
        fds: list[int] = []
        while len(header) < HEADER_SIZE:
            chunk, more = self._recv_chunk(HEADER_SIZE - len(header))
            header += chunk
            fds += more
        request, flags, size = decode_header(header)
        payload = b""
        while len(payload) < size:
            chunk, more = self._recv_chunk(size - len(payload))
            payload += chunk
            fds += more
        return Message(request, flags, payload, fds)

    def close(self) -> None:
        """Shut the socket down (idempotent)."""
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._sock.close()

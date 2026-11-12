# SPDX-License-Identifier: Apache-2.0
"""FUSE over a virtqueue: post requests, harvest replies, decode them.

Implements requirements:
  - VFS-02: requests go on the standard request queue, never on the hiprio one.
  - VFS-03: INIT negotiation plus LOOKUP, GETATTR, OPEN/OPENDIR, READ, WRITE,
    READDIR, FLUSH, RELEASE/RELEASEDIR and FORGET, encoded with the single
    Python FUSE table of ``pgwutil.fuse_wire`` (the C twin lives in
    ``protofuse/src/fusep_abi.h``).
  - ROB-06: a reply with the wrong ``unique``, a positive error field or a
    truncated body raises instead of being trusted.

Each request occupies one virtqueue slot: a read-only descriptor holding the
40-byte ``fuse_in_header`` followed by the body, and a write-only descriptor
the backend fills with the 16-byte ``fuse_out_header`` and its body.
"""

from __future__ import annotations

import errno as errno_mod
import time

from pgwutil import fuse_wire as fw
from vhost.protocol import ProtocolError
from vhost.vring import SplitVirtqueue

# Replies we never expect to be bigger than this (READ/READDIR pass their own).
DEFAULT_REPLY_SIZE = 4096


class FuseReplyError(Exception):
    """The backend answered with a FUSE error, or with a malformed reply."""

    def __init__(self, message: str, error: int = 0) -> None:
        super().__init__(message)
        self.error = error


class VirtqueueFuseClient:
    """A synchronous FUSE client on top of one split virtqueue."""

    def __init__(self, vq: SplitVirtqueue, timeout: float = 5.0) -> None:
        self.vq = vq
        self.timeout = timeout
        self._unique = 0
        self._free = list(range(vq.slots))
        self.unexpected: list[int] = []
        self.minor = 0
        self.max_write = 0

    # --- request plumbing --------------------------------------------------
    def next_unique(self) -> int:
        self._unique += 1
        return self._unique

    def _alloc_slot(self) -> int:
        if not self._free:
            raise FuseReplyError("every virtqueue slot is in flight")
        return self._free.pop(0)

    def call(self, opcode: int, nodeid: int, body: bytes = b"", reply_size: int = DEFAULT_REPLY_SIZE) -> bytes:
        """Send one FUSE request and return its reply body, header stripped."""
        unique = self.next_unique()
        request = fw.pack_in_header(opcode, unique, nodeid, fw.IN_HDR + len(body)) + body
        slot = self._alloc_slot()
        try:
            return self._exchange(slot, request, unique, reply_size)
        finally:
            self._free.append(slot)

    def _exchange(self, slot: int, request: bytes, unique: int, reply_size: int) -> bytes:
        if len(request) > self.vq.tx_size:
            raise FuseReplyError("request of %d bytes does not fit the slot" % len(request))
        reply_size = min(reply_size, self.vq.rx_size)
        self.vq.tx_buffer(slot)[: len(request)] = request
        head = self.vq.post(slot, len(request), reply_size)
        self.vq.kick()
        reply = self._wait_for(head)
        if reply is None:
            raise TimeoutError("no FUSE reply for descriptor %d within %ss" % (head, self.timeout))
        return self._decode(reply, unique)

    def _wait_for(self, head: int, timeout: float | None = None) -> bytes | None:
        """Block until the backend publishes *head* on the used ring.

        A completion for another descriptor cannot happen with one request in
        flight; when it does (a late FORGET, a confused backend) it is recorded
        in ``unexpected`` instead of being mistaken for our reply, so the caller
        can assert on it once the sequence is over.
        """
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while True:
            for used_head, length in self.vq.harvest():
                if used_head == head:
                    return bytes(self.vq.rx_buffer(used_head // 2)[:length])
                self.unexpected.append(used_head)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.vq.wait_call(min(remaining, 1.0))

    def _decode(self, reply: bytes, unique: int) -> bytes:
        length, error, reply_unique = fw.parse_out_header(reply)
        if reply_unique != unique:
            raise FuseReplyError("reply unique %d does not match request %d" % (reply_unique, unique))
        if error > 0:
            raise FuseReplyError("positive error field %d in reply" % error, error)
        if error < 0:
            raise FuseReplyError(_errno_text(error), error)
        if length > len(reply):
            raise ProtocolError("reply declares %d bytes, slot holds %d" % (length, len(reply)))
        return reply[fw.OUT_HDR:length]

    # --- operations --------------------------------------------------------
    def init(self, flags: int = 0, max_readahead: int = 0) -> dict:
        """FUSE INIT; keeps the negotiated minor version and max_write."""
        body = self.call(fw.OP_INIT, 0, fw.pack_init_in(max_readahead, flags))
        out = fw.unpack_init_out(body)
        self.minor = out.get("minor", 0)
        self.max_write = out.get("max_write", 0)
        return out

    def lookup(self, parent: int, name: str) -> dict:
        body = self.call(fw.OP_LOOKUP, parent, fw.pack_lookup_in(name))
        return fw.unpack_entry(body)

    def getattr(self, nodeid: int) -> dict:
        return fw.unpack_attr_out(self.call(fw.OP_GETATTR, nodeid, fw.pack_getattr_in()))

    def open(self, nodeid: int, flags: int, is_dir: bool = False) -> int:
        opcode = fw.OP_OPENDIR if is_dir else fw.OP_OPEN
        body = self.call(opcode, nodeid, fw.pack_open_in(flags))
        return fw.unpack_open_out(body)[0]

    def read(self, nodeid: int, fh: int, offset: int, size: int) -> bytes:
        return self.call(fw.OP_READ, nodeid, fw.pack_read_in(fh, offset, size), fw.OUT_HDR + size)

    def write(self, nodeid: int, fh: int, offset: int, data: bytes) -> int:
        body = fw.pack_write_in(fh, offset, len(data)) + data
        return fw.unpack_write_out(self.call(fw.OP_WRITE, nodeid, body))

    def readdir(self, nodeid: int, fh: int, offset: int = 0, size: int = 8192) -> list[dict]:
        body = self.call(fw.OP_READDIR, nodeid, fw.pack_read_in(fh, offset, size), fw.OUT_HDR + size)
        return fw.unpack_dirents(body)

    def flush(self, nodeid: int, fh: int) -> None:
        self.call(fw.OP_FLUSH, nodeid, fw.pack_flush_in(fh))

    def release(self, nodeid: int, fh: int, is_dir: bool = False) -> None:
        opcode = fw.OP_RELEASEDIR if is_dir else fw.OP_RELEASE
        self.call(opcode, nodeid, fw.pack_release_in(fh))

    def forget(self, nodeid: int, nlookup: int = 1, timeout: float = 2.0) -> bool:
        """Give a lookup reference back (VFS-04). FORGET carries no reply.

        The buffer still has to come back on the used ring for the slot to be
        reusable; when the backend keeps it, the slot stays in flight and this
        returns False rather than corrupting the next request.
        """
        unique = self.next_unique()
        request = fw.pack_in_header(fw.OP_FORGET, unique, nodeid, fw.IN_HDR + 8)
        request += fw.pack_forget_in(nlookup)
        slot = self._alloc_slot()
        self.vq.tx_buffer(slot)[: len(request)] = request
        head = self.vq.post(slot, len(request), fw.OUT_HDR)
        self.vq.kick()
        if self._wait_for(head, timeout) is None:
            return False
        self._free.append(slot)
        return True


def _errno_text(error: int) -> str:
    """``-ENOENT`` becomes a readable message with the number kept."""
    code = -error
    name = errno_mod.errorcode.get(code, "errno %d" % code)
    return "backend refused the request: -%s (%d)" % (name, code)

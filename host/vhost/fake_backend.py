# SPDX-License-Identifier: Apache-2.0
"""A vhost-user backend written in Python, for tests that need no virtiofsd.

Implements requirements:
  - VFS-01, VFS-02, VFS-03: the device side of the protocol, so the front end
    of ``host/vhost`` can be unit-tested end to end (framing, negotiation,
    memory table, virtqueue layout, kick/call signalling) on any machine.
  - ROB-06: malformed requests from the front end are refused with a non-zero
    acknowledgement rather than crashing the backend thread.

This is a test double, not a filesystem: FUSE requests are answered by a
*responder* callback, by default a handful of canned replies. The real thing
is ``/usr/libexec/virtiofsd``, exercised by ``host/vhost/driver.py``.

It deliberately behaves like the ``vhost`` crate: it maps the memfd wherever it
likes and translates the front end's virtual addresses through the
``userspace_addr`` of the memory table, exactly as ``vmm_va_to_gpa`` does.
"""

from __future__ import annotations

import errno
import mmap
import os
import selectors
import struct
import threading
from typing import NamedTuple

from pgwutil import fuse_wire as fw
from vhost.protocol import (
    PROTO_F_MQ,
    PROTO_F_REPLY_ACK,
    VHOST_USER_F_PROTOCOL_FEATURES,
    VIRTIO_F_VERSION_1,
    ProtocolError,
    Request,
    decode_memory_table,
    decode_u64,
    decode_vring_addr,
    decode_vring_state,
    encode_u64,
    encode_vring_state,
)
from vhost.transport import Message, Transport
from vhost.vring import DESC_F_NEXT, DESC_F_WRITE, DESC_SIZE

_DESC = struct.Struct("<QIHH")
_USED_ELEM = struct.Struct("<II")
_IDX = struct.Struct("<H")

ENOENT = errno.ENOENT


class _Region(NamedTuple):
    """One entry of the memory table as the backend sees it."""

    gpa: int
    user_addr: int
    mapping: mmap.mmap

DEFAULT_FEATURES = VIRTIO_F_VERSION_1 | VHOST_USER_F_PROTOCOL_FEATURES
DEFAULT_PROTOCOL_FEATURES = PROTO_F_REPLY_ACK | PROTO_F_MQ


def default_responder(opcode: int, nodeid: int, body: bytes) -> tuple[int, bytes]:
    """Canned FUSE replies: INIT, LOOKUP, GETATTR, OPEN, READ, WRITE, READDIR."""
    attr = fw.pack_attr(2, 5, 0o100644, 1, 0, 0, 0)
    if opcode == fw.OP_INIT:
        return 0, fw.pack_init(36, 131072)
    if opcode == fw.OP_LOOKUP:
        return 0, fw.pack_entry(2, attr)
    if opcode == fw.OP_GETATTR:
        return 0, struct.pack("<QII", 1, 0, 0) + attr
    if opcode in (fw.OP_OPEN, fw.OP_OPENDIR):
        return 0, fw.pack_open_out(7)
    if opcode == fw.OP_READ:
        return 0, b"hello"
    if opcode == fw.OP_WRITE:
        return 0, fw.pack_write_out(len(body) - 40)
    if opcode == fw.OP_READDIR:
        raw = b"file"  # a dirent name carries no NUL, as in tests/fuse_ref
        return 0, fw.pack_dirent(2, 1, raw, fw.DT_REG, (24 + len(raw) + 7) & ~7)
    if opcode in (fw.OP_FLUSH, fw.OP_RELEASE, fw.OP_RELEASEDIR):
        return 0, b""
    return -ENOENT, b""  # anything this double does not implement


class FakeVhostBackend:
    """Serves one front end on a UNIX socket, in a background thread."""

    def __init__(
        self,
        socket_path: str,
        responder=default_responder,
        features: int = DEFAULT_FEATURES,
        protocol_features: int = DEFAULT_PROTOCOL_FEATURES,
        num_queues: int = 2,
        queue_size: int = 64,
    ) -> None:
        self.socket_path = socket_path
        self.responder = responder
        self.features = features
        self.protocol_features = protocol_features
        self.num_queues = num_queues
        self.max_queue_size = queue_size
        self.log: list[tuple[str, object]] = []
        self.acked_features = 0
        self.acked_protocol_features = 0
        self.owner_set = False
        self.regions: list[_Region] = []
        self.queues: dict[int, dict] = {}
        self.errors: list[str] = []
        self._mmaps: list[mmap.mmap] = []
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._selector = selectors.DefaultSelector()
        self._transport: Transport | None = None

    # --- lifecycle ---------------------------------------------------------
    def start(self) -> "FakeVhostBackend":
        if os.path.exists(self.socket_path):
            os.unlink(self.socket_path)
        self._listener = Transport.listen(self.socket_path)
        self._selector.register(self._listener, selectors.EVENT_READ, ("accept", None))
        self._thread = threading.Thread(target=self._run, daemon=True, name="fake-vhost-backend")
        self._thread.start()
        return self

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5.0)
        self._selector.close()
        for mapping in self._mmaps:
            mapping.close()
        if os.path.exists(self.socket_path):
            os.unlink(self.socket_path)

    def __enter__(self) -> "FakeVhostBackend":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()

    # --- event loop --------------------------------------------------------
    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                events = self._selector.select(0.05)
            except (OSError, ValueError):
                return
            for key, _mask in events:
                if not self._dispatch(key.data):
                    return

    def _dispatch(self, data) -> bool:
        """Handle one selector event; False stops the loop (socket gone)."""
        kind, index = data
        try:
            if kind == "accept":
                self._accept()
            elif kind == "message":
                self._handle_message(self._transport.recv(1.0))
            elif kind == "kick":
                os.eventfd_read(index[0])
                self.service_queue(index[1])
        except TimeoutError:
            return True  # a spurious wakeup, nothing to serve yet
        except (ProtocolError, ValueError) as exc:
            self.errors.append("%s: %s" % (kind, exc))
            return True
        except OSError as exc:
            self.errors.append("%s: %s" % (kind, exc))
            return False
        return True

    def _accept(self) -> None:
        conn, _ = self._listener.accept()
        self._transport = Transport(conn)
        self._selector.unregister(self._listener)
        self._selector.register(conn, selectors.EVENT_READ, ("message", None))

    # --- protocol ----------------------------------------------------------
    def _reply(self, code: Request, payload: bytes) -> None:
        self._transport.send(code, payload, reply=True)

    def _ack(self, msg: Message, ok: bool = True) -> None:
        if msg.is_need_reply and self.acked_protocol_features & PROTO_F_REPLY_ACK:
            self._reply(Request(msg.request), encode_u64(0 if ok else 1))

    def _handle_message(self, msg: Message) -> None:
        code = Request(msg.request)
        self.log.append((code.name, msg.payload))
        handler = getattr(self, "_on_" + code.name.lower(), None)
        if handler is None:
            self.errors.append("unsupported request %s" % code.name)
            self._ack(msg, ok=False)
            return
        try:
            handler(msg)
        except (ProtocolError, ValueError, OSError) as exc:
            self.errors.append("%s: %s" % (code.name, exc))
            self._ack(msg, ok=False)

    def _on_get_features(self, msg: Message) -> None:
        self._reply(Request.GET_FEATURES, encode_u64(self.features))

    def _on_set_features(self, msg: Message) -> None:
        value = decode_u64(msg.payload)
        if value & ~self.features:
            raise ProtocolError("front end asked for features we do not offer")
        self.acked_features = value
        self._ack(msg)

    def _on_get_protocol_features(self, msg: Message) -> None:
        self._reply(Request.GET_PROTOCOL_FEATURES, encode_u64(self.protocol_features))

    def _on_set_protocol_features(self, msg: Message) -> None:
        self.acked_protocol_features = decode_u64(msg.payload)
        self._ack(msg)

    def _on_get_queue_num(self, msg: Message) -> None:
        self._reply(Request.GET_QUEUE_NUM, encode_u64(self.num_queues))

    def _on_set_owner(self, msg: Message) -> None:
        self.owner_set = True
        self._ack(msg)

    def _on_set_mem_table(self, msg: Message) -> None:
        regions = decode_memory_table(msg.payload)
        if len(regions) != len(msg.fds):
            msg.close_fds()
            raise ProtocolError("%d regions but %d descriptors" % (len(regions), len(msg.fds)))
        self.regions = []
        for region, fd in zip(regions, msg.fds):
            mapping = mmap.mmap(fd, region.memory_size, offset=region.mmap_offset)
            os.close(fd)
            self._mmaps.append(mapping)
            self.regions.append(_Region(region.guest_phys_addr, region.user_addr, mapping))
        self._ack(msg)

    def _on_set_vring_num(self, msg: Message) -> None:
        index, num = decode_vring_state(msg.payload)
        self._queue(index)["size"] = num
        self._ack(msg)

    def _on_set_vring_base(self, msg: Message) -> None:
        index, base = decode_vring_state(msg.payload)
        queue = self._queue(index)
        queue["next_avail"] = base
        queue["last_used"] = base
        self._ack(msg)

    def _on_set_vring_addr(self, msg: Message) -> None:
        addr = decode_vring_addr(msg.payload)
        queue = self._queue(addr["index"])
        queue["mem"], queue["desc"] = self._locate_va(addr["desc"])
        _mem, queue["used"] = self._locate_va(addr["used"])
        _mem, queue["avail"] = self._locate_va(addr["avail"])
        self._ack(msg)

    def _on_set_vring_enable(self, msg: Message) -> None:
        index, enable = decode_vring_state(msg.payload)
        self._queue(index)["enabled"] = bool(enable)
        self._ack(msg)

    def _on_set_vring_kick(self, msg: Message) -> None:
        self._set_vring_fd(msg, "kick")

    def _on_set_vring_call(self, msg: Message) -> None:
        self._set_vring_fd(msg, "call")

    def _set_vring_fd(self, msg: Message, kind: str) -> None:
        index = decode_u64(msg.payload) & 0xFF
        queue = self._queue(index)
        if not msg.fds:
            raise ProtocolError("SET_VRING_%s carried no descriptor" % kind.upper())
        queue[kind] = msg.fds[0]
        msg.fds = msg.fds[1:]
        if kind == "kick":
            self._selector.register(
                queue["kick"], selectors.EVENT_READ, ("kick", (queue["kick"], index))
            )
        self._ack(msg)

    def _queue(self, index: int) -> dict:
        if not 0 <= index < self.num_queues:
            raise ProtocolError("queue index %d out of range" % index)
        return self.queues.setdefault(
            int(index), {"enabled": False, "next_avail": 0, "last_used": 0}
        )

    # --- memory translation ------------------------------------------------
    def _locate_va(self, front_end_va: int, length: int = 0) -> tuple[mmap.mmap, int]:
        """Translate a front-end virtual address, as ``vmm_va_to_gpa`` does."""
        for region in self.regions:
            end = region.user_addr + len(region.mapping)
            if region.user_addr <= front_end_va and front_end_va + length <= end:
                return region.mapping, front_end_va - region.user_addr
        raise ProtocolError("address 0x%x is outside every shared region" % front_end_va)

    def _locate_gpa(self, gpa: int, length: int = 0) -> tuple[mmap.mmap, int]:
        """Translate a guest physical address (what descriptors carry)."""
        for region in self.regions:
            end = region.gpa + len(region.mapping)
            if region.gpa <= gpa and gpa + length <= end:
                return region.mapping, gpa - region.gpa
        raise ProtocolError("guest address 0x%x is outside every shared region" % gpa)

    @staticmethod
    def _ring(queue: dict, name: str, offset: int, length: int) -> memoryview:
        """A view of *length* bytes inside one of the queue's rings."""
        start = queue[name] + offset
        return memoryview(queue["mem"])[start:start + length]

    # --- virtqueue service -------------------------------------------------
    def service_queue(self, index: int) -> int:
        """Process every available buffer of *index*; returns how many."""
        queue = self._queue(index)
        if not queue.get("enabled"):
            return 0
        processed = 0
        while True:
            head = self._next_available(queue)
            if head is None:
                break
            self._serve_one(queue, head)
            processed += 1
        if processed and queue.get("call"):
            os.eventfd_write(queue["call"], 1)
        return processed

    def _next_available(self, queue: dict) -> int | None:
        avail_idx = _IDX.unpack_from(self._ring(queue, "avail", 2, 2), 0)[0]
        if queue["next_avail"] == avail_idx:
            return None
        slot = queue["next_avail"] % queue["size"]
        head = _IDX.unpack_from(self._ring(queue, "avail", 4 + 2 * slot, 2), 0)[0]
        queue["next_avail"] = (queue["next_avail"] + 1) & 0xFFFF
        return head

    def _serve_one(self, queue: dict, head: int) -> None:
        request, reply = self._read_chain(queue, head)
        opcode, unique, nodeid = fw.parse_request(request)
        error, reply_body = self.responder(opcode, nodeid, request[fw.IN_HDR:])
        answer = fw.pack_reply(unique, reply_body, error)
        reply[: len(answer)] = answer[: len(reply)]
        self._add_used(queue, head, len(answer))

    def _read_chain(self, queue: dict, head: int) -> tuple[bytes, memoryview]:
        """Walk the descriptor chain: read-only bytes plus the reply buffer."""
        request = b""
        reply: memoryview | None = None
        index = head
        for _ in range(queue["size"]):
            desc = self._ring(queue, "desc", index * DESC_SIZE, DESC_SIZE)
            addr, length, flags, nxt = _DESC.unpack_from(desc, 0)
            mem, buf_off = self._locate_gpa(addr, length)
            if flags & DESC_F_WRITE:
                reply = memoryview(mem)[buf_off:buf_off + length]
            else:
                request += bytes(memoryview(mem)[buf_off:buf_off + length])
            if not flags & DESC_F_NEXT:
                break
            index = nxt
        if reply is None:
            raise ProtocolError("descriptor chain %d has no writable buffer" % head)
        return request, reply

    def _add_used(self, queue: dict, head: int, length: int) -> None:
        used_idx = _IDX.unpack_from(self._ring(queue, "used", 2, 2), 0)[0]
        slot = used_idx % queue["size"]
        _USED_ELEM.pack_into(self._ring(queue, "used", 4 + 8 * slot, 8), 0, head, length)
        _IDX.pack_into(self._ring(queue, "used", 2, 2), 0, (used_idx + 1) & 0xFFFF)

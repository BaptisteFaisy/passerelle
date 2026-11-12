# SPDX-License-Identifier: Apache-2.0
"""The vhost-user front end: negotiation, memory table and virtqueue setup.

Implements requirements:
  - VFS-01: the front end reads what the device offers (features, protocol
    features, queue count) and only ever acks bits it implements itself.
  - VFS-02: one virtqueue per role - queue 0 stays the hiprio queue, file I/O
    goes through the request queue the caller picks.
  - VFS-06: this is the host side of what ``qshare`` asks QEMU to do for a
    virtiofs device, done here directly so that no guest is needed.
  - ROB-06: a missing reply, a reply for another request or a non-zero ack
    raises instead of being ignored.

Message sequence, identical to the one the ``vhost`` crate's front end uses:
GET_FEATURES, SET_FEATURES, GET/SET_PROTOCOL_FEATURES, SET_OWNER,
SET_MEM_TABLE, then per queue SET_VRING_NUM, SET_VRING_ADDR, SET_VRING_BASE,
SET_VRING_KICK, SET_VRING_CALL and SET_VRING_ENABLE. The two SET_* messages of
the negotiation are sent before REPLY_ACK is acked, so they carry no
NEED_REPLY flag and are answered by nothing.
"""

from __future__ import annotations

from vhost.memory import SharedMemory
from vhost.protocol import (
    PROTO_F_MQ,
    PROTO_F_REPLY_ACK,
    VHOST_USER_F_PROTOCOL_FEATURES,
    VIRTIO_F_VERSION_1,
    ProtocolError,
    Request,
    decode_u64,
    describe_features,
    describe_protocol_features,
    encode_u64,
    encode_vring_addr,
    encode_vring_state,
    request_name,
)
from vhost.transport import Message, Transport
from vhost.vring import SplitVirtqueue

# Only the bits this front end implements: a modern split virtqueue and the
# protocol-feature handshake. EVENT_IDX is deliberately left out, so the kick
# and call eventfds carry every notification (legacy signalling).
SUPPORTED_FEATURES = VIRTIO_F_VERSION_1 | VHOST_USER_F_PROTOCOL_FEATURES
SUPPORTED_PROTOCOL_FEATURES = PROTO_F_REPLY_ACK


class BackendError(Exception):
    """The backend answered, but refused the request (non-zero ack)."""


class VhostUserDevice:
    """Drives one vhost-user backend over an already connected socket."""

    def __init__(self, transport: Transport, timeout: float = 5.0) -> None:
        self.transport = transport
        self.timeout = timeout
        self.device_features = 0
        self.acked_features = 0
        self.device_protocol_features = 0
        self.acked_protocol_features = 0
        self.num_queues = 0

    # --- low level request/reply -------------------------------------------
    def _send(self, code: Request, payload: bytes = b"", fds: tuple[int, ...] = ()) -> None:
        need_reply = bool(self.acked_protocol_features & PROTO_F_REPLY_ACK)
        self.transport.send(code, payload, fds, need_reply=need_reply)

    def _recv_reply(self, code: Request) -> Message:
        msg = self.transport.recv(self.timeout)
        if not msg.is_reply_for(code):
            msg.close_fds()
            raise ProtocolError(
                "expected a reply to %s, got %s with flags 0x%x"
                % (code.name, request_name(msg.request), msg.flags)
            )
        return msg

    def request(self, code: Request, payload: bytes = b"", fds: tuple[int, ...] = ()) -> Message:
        """Send a request that expects an answer, and return that answer."""
        self._send(code, payload, fds)
        msg = self._recv_reply(code)
        if self._expects_ack(code) and decode_u64(msg.payload) != 0:
            raise BackendError("%s was refused (ack %d)" % (code.name, decode_u64(msg.payload)))
        return msg

    @staticmethod
    def _expects_ack(code: Request) -> bool:
        """True for the SET_* requests whose reply is a plain success ack."""
        return code.name.startswith("SET_")

    # --- negotiation -------------------------------------------------------
    def get_features(self) -> int:
        self.transport.send(Request.GET_FEATURES)
        self.device_features = self._recv_reply(Request.GET_FEATURES).u64()
        return self.device_features

    def set_features(self, features: int) -> int:
        """Ack *features*, refusing anything the device did not offer."""
        if features & ~self.device_features:
            raise ProtocolError("asking for unsupported features 0x%x" % (features & ~self.device_features))
        self.acked_features = features
        self.transport.send(Request.SET_FEATURES, encode_u64(features))
        return self.acked_features

    def get_protocol_features(self) -> int:
        self.transport.send(Request.GET_PROTOCOL_FEATURES)
        self.device_protocol_features = self._recv_reply(Request.GET_PROTOCOL_FEATURES).u64()
        return self.device_protocol_features

    def set_protocol_features(self, features: int) -> int:
        """Ack protocol features; REPLY_ACK turns acknowledgements on."""
        self.acked_protocol_features = features
        self.transport.send(Request.SET_PROTOCOL_FEATURES, encode_u64(features))
        return self.acked_protocol_features

    def negotiate(self) -> dict[str, object]:
        """The whole handshake up to and including SET_OWNER."""
        self.get_features()
        features = self.device_features & SUPPORTED_FEATURES
        self.set_features(features)
        if features & VHOST_USER_F_PROTOCOL_FEATURES:
            self.get_protocol_features()
            self.set_protocol_features(self.device_protocol_features & SUPPORTED_PROTOCOL_FEATURES)
            self.num_queues = self._query_queue_num()
        self.request(Request.SET_OWNER)
        return self.summary()

    def _query_queue_num(self) -> int:
        """GET_QUEUE_NUM is only legal once MQ has been negotiated."""
        if not self.acked_protocol_features & PROTO_F_MQ:
            return 0
        return self.request(Request.GET_QUEUE_NUM).u64()

    def summary(self) -> dict[str, object]:
        """What the handshake produced, for logs and test reports."""
        return {
            "device_features": describe_features(self.device_features),
            "acked_features": describe_features(self.acked_features),
            "device_protocol_features": describe_protocol_features(self.device_protocol_features),
            "acked_protocol_features": describe_protocol_features(self.acked_protocol_features),
            "num_queues": self.num_queues,
        }

    # --- memory and virtqueues ---------------------------------------------
    def set_mem_table(self, mem: SharedMemory) -> None:
        """Hand the backend our memfd and the region that describes it."""
        self.request(Request.SET_MEM_TABLE, mem.memory_table(), (mem.fd,))

    def set_vring_num(self, index: int, num: int) -> None:
        self.request(Request.SET_VRING_NUM, encode_vring_state(index, num))

    def set_vring_addr(self, index: int, desc: int, used: int, avail: int) -> None:
        self.request(Request.SET_VRING_ADDR, encode_vring_addr(index, 0, desc, used, avail))

    def set_vring_base(self, index: int, base: int = 0) -> None:
        self.request(Request.SET_VRING_BASE, encode_vring_state(index, base))

    def set_vring_kick(self, index: int, fd: int) -> None:
        self.request(Request.SET_VRING_KICK, encode_u64(index), (fd,))

    def set_vring_call(self, index: int, fd: int) -> None:
        self.request(Request.SET_VRING_CALL, encode_u64(index), (fd,))

    def set_vring_enable(self, index: int, enable: bool = True) -> None:
        if not self.acked_features & VHOST_USER_F_PROTOCOL_FEATURES:
            return  # without PROTOCOL_FEATURES the backend enables every ring
        self.request(Request.SET_VRING_ENABLE, encode_vring_state(index, 1 if enable else 0))

    def setup_queue(self, vq: SplitVirtqueue) -> None:
        """Configure, wire and enable one virtqueue, in the protocol's order."""
        self.set_vring_num(vq.index, vq.num)
        self.set_vring_addr(vq.index, vq.desc_addr, vq.used_addr, vq.avail_addr)
        self.set_vring_base(vq.index, 0)
        self.set_vring_kick(vq.index, vq.kick_fd)
        self.set_vring_call(vq.index, vq.call_fd)
        self.set_vring_enable(vq.index, True)

    def close(self) -> None:
        self.transport.close()

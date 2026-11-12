# SPDX-License-Identifier: Apache-2.0
"""vhost-user wire constants, message framing and payload codecs.

This is the device ("front end" in vhost-user vocabulary) half of the
vhost-user protocol: the numbers, the 12-byte message header and the payload
structures that travel over the UNIX socket.

Implements requirements:
  - VFS-01: the transport a virtiofs device is reached through, including the
    queue count the front end may query.
  - VFS-06: launching and configuring ``virtiofsd`` over a vhost-user socket.
  - ROB-06: every decoder validates its input and raises ``ProtocolError``
    instead of trusting a malformed message.

Field order and sizes are restated in our own words from the vhost-user
specification (QEMU ``docs/interop/vhost-user.rst``) and cross-checked against
the ``vhost`` crate 0.10 that ``virtiofsd`` 1.10 is built with. The protocol
feature bit numbers are the crate's, which differ from the Linux kernel's:
bit 3 is REPLY_ACK here, not bit 34.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum

HEADER_SIZE = 12
_HEADER = struct.Struct("<III")
MAX_MSG_SIZE = 0x1000
MAX_ATTACHED_FDS = 8

# --- message header flags ---------------------------------------------------
FLAG_VERSION = 0x1
FLAG_REPLY = 0x4
FLAG_NEED_REPLY = 0x8

# --- virtio feature bits (device feature mask) ------------------------------
VIRTIO_RING_F_INDIRECT_DESC = 1 << 28
VIRTIO_RING_F_EVENT_IDX = 1 << 29
VHOST_USER_F_PROTOCOL_FEATURES = 1 << 30
VIRTIO_F_VERSION_1 = 1 << 32

# --- vhost-user protocol feature bits (vhost crate numbering) ---------------
PROTO_F_MQ = 0x1
PROTO_F_LOG_SHMFD = 0x2
PROTO_F_RARP = 0x4
PROTO_F_REPLY_ACK = 0x8
PROTO_F_MTU = 0x10
PROTO_F_BACKEND_REQ = 0x20
PROTO_F_CROSS_ENDIAN = 0x40
PROTO_F_CRYPTO_SESSION = 0x80
PROTO_F_PAGEFAULT = 0x100
PROTO_F_CONFIG = 0x200
PROTO_F_BACKEND_SEND_FD = 0x400
PROTO_F_HOST_NOTIFIER = 0x800
PROTO_F_INFLIGHT_SHMFD = 0x1000
PROTO_F_RESET_DEVICE = 0x2000
PROTO_F_INBAND_NOTIFICATIONS = 0x4000
PROTO_F_CONFIGURE_MEM_SLOTS = 0x8000
PROTO_F_STATUS = 0x10000

PROTOCOL_FEATURE_NAMES = {
    PROTO_F_MQ: "MQ",
    PROTO_F_LOG_SHMFD: "LOG_SHMFD",
    PROTO_F_RARP: "RARP",
    PROTO_F_REPLY_ACK: "REPLY_ACK",
    PROTO_F_MTU: "MTU",
    PROTO_F_BACKEND_REQ: "BACKEND_REQ",
    PROTO_F_CROSS_ENDIAN: "CROSS_ENDIAN",
    PROTO_F_CRYPTO_SESSION: "CRYPTO_SESSION",
    PROTO_F_PAGEFAULT: "PAGEFAULT",
    PROTO_F_CONFIG: "CONFIG",
    PROTO_F_BACKEND_SEND_FD: "BACKEND_SEND_FD",
    PROTO_F_HOST_NOTIFIER: "HOST_NOTIFIER",
    PROTO_F_INFLIGHT_SHMFD: "INFLIGHT_SHMFD",
    PROTO_F_RESET_DEVICE: "RESET_DEVICE",
    PROTO_F_INBAND_NOTIFICATIONS: "INBAND_NOTIFICATIONS",
    PROTO_F_CONFIGURE_MEM_SLOTS: "CONFIGURE_MEM_SLOTS",
    PROTO_F_STATUS: "STATUS",
}


class Request(IntEnum):
    """Requests a front end may send to a vhost-user backend."""

    GET_FEATURES = 1
    SET_FEATURES = 2
    SET_OWNER = 3
    RESET_OWNER = 4
    SET_MEM_TABLE = 5
    SET_LOG_BASE = 6
    SET_LOG_FD = 7
    SET_VRING_NUM = 8
    SET_VRING_ADDR = 9
    SET_VRING_BASE = 10
    GET_VRING_BASE = 11
    SET_VRING_KICK = 12
    SET_VRING_CALL = 13
    SET_VRING_ERR = 14
    GET_PROTOCOL_FEATURES = 15
    SET_PROTOCOL_FEATURES = 16
    GET_QUEUE_NUM = 17
    SET_VRING_ENABLE = 18
    SEND_RARP = 19
    NET_SET_MTU = 20
    SET_BACKEND_REQ_FD = 21
    IOTLB_MSG = 22
    SET_VRING_ENDIAN = 23
    GET_CONFIG = 24
    SET_CONFIG = 25
    RESET_DEVICE = 34
    VRING_KICK = 35
    GET_MAX_MEM_SLOTS = 36
    ADD_MEM_REG = 37
    REM_MEM_REG = 38
    SET_STATUS = 39
    GET_STATUS = 40


# Payload structs. The memory table is the only variable-length one: an 8-byte
# header followed by 32 bytes per region.
_U64 = struct.Struct("<Q")
_VRING_STATE = struct.Struct("<II")
_VRING_ADDR = struct.Struct("<IIQQQQ")
_MEM_HEADER = struct.Struct("<II")
_MEM_REGION = struct.Struct("<QQQQ")


class ProtocolError(Exception):
    """Raised on any malformed or implausible vhost-user message."""


def encode_header(request: int, flags: int, size: int) -> bytes:
    """The 12-byte message header: request, flags, payload size."""
    if size > MAX_MSG_SIZE:
        raise ProtocolError("payload of %d bytes exceeds %d" % (size, MAX_MSG_SIZE))
    return _HEADER.pack(int(request), flags, size)


def decode_header(data: bytes) -> tuple[int, int, int]:
    """Split a 12-byte header into (request, flags, size)."""
    if len(data) != HEADER_SIZE:
        raise ProtocolError("header must be %d bytes, got %d" % (HEADER_SIZE, len(data)))
    request, flags, size = _HEADER.unpack(data)
    if size > MAX_MSG_SIZE:
        raise ProtocolError("implausible payload size %d" % size)
    return request, flags, size


def encode_u64(value: int) -> bytes:
    """One 64-bit payload (features, protocol features, vring file index)."""
    return _U64.pack(value)


def decode_u64(payload: bytes) -> int:
    """Read one 64-bit payload, rejecting anything that is not exactly 8 bytes."""
    if len(payload) != 8:
        raise ProtocolError("u64 payload must be 8 bytes, got %d" % len(payload))
    return _U64.unpack(payload)[0]


def encode_vring_state(index: int, num: int) -> bytes:
    """SET_VRING_NUM / SET_VRING_BASE / SET_VRING_ENABLE payload."""
    return _VRING_STATE.pack(index, num)


def decode_vring_state(payload: bytes) -> tuple[int, int]:
    """Read a vring state payload as (index, num)."""
    if len(payload) != _VRING_STATE.size:
        raise ProtocolError("vring state payload must be 8 bytes, got %d" % len(payload))
    return _VRING_STATE.unpack(payload)


def encode_vring_addr(
    index: int, flags: int, desc: int, used: int, avail: int, log: int = 0
) -> bytes:
    """SET_VRING_ADDR payload; addresses are front-end virtual addresses."""
    return _VRING_ADDR.pack(index, flags, desc, used, avail, log)


def decode_vring_addr(payload: bytes) -> dict[str, int]:
    """Read a SET_VRING_ADDR payload into a named dictionary."""
    if len(payload) != _VRING_ADDR.size:
        raise ProtocolError("vring addr payload must be 40 bytes, got %d" % len(payload))
    index, flags, desc, used, avail, log = _VRING_ADDR.unpack(payload)
    return {
        "index": index, "flags": flags, "desc": desc, "used": used,
        "avail": avail, "log": log,
    }


@dataclass(frozen=True)
class MemoryRegion:
    """One entry of the SET_MEM_TABLE region array."""

    guest_phys_addr: int
    memory_size: int
    user_addr: int
    mmap_offset: int = 0

    def encode(self) -> bytes:
        return _MEM_REGION.pack(
            self.guest_phys_addr, self.memory_size, self.user_addr, self.mmap_offset
        )


def encode_memory_table(regions: list[MemoryRegion]) -> bytes:
    """The SET_MEM_TABLE payload: count, padding, then one region per entry."""
    if not regions or len(regions) > 32:
        raise ProtocolError("a memory table carries 1..32 regions, got %d" % len(regions))
    out = bytearray(_MEM_HEADER.pack(len(regions), 0))
    for region in regions:
        out += region.encode()
    return bytes(out)


def decode_memory_table(payload: bytes) -> list[MemoryRegion]:
    """Parse a SET_MEM_TABLE payload; rejects truncated or inconsistent tables."""
    if len(payload) < _MEM_HEADER.size:
        raise ProtocolError("memory table payload shorter than its header")
    count, padding = _MEM_HEADER.unpack_from(payload, 0)
    if padding != 0:
        raise ProtocolError("memory table padding must be zero, got %d" % padding)
    if count == 0 or count > 32:
        raise ProtocolError("implausible region count %d" % count)
    need = _MEM_HEADER.size + count * _MEM_REGION.size
    if len(payload) < need:
        raise ProtocolError("memory table truncated: %d of %d bytes" % (len(payload), need))
    regions = []
    for i in range(count):
        off = _MEM_HEADER.size + i * _MEM_REGION.size
        gpa, size, user_addr, mmap_offset = _MEM_REGION.unpack_from(payload, off)
        if size == 0:
            raise ProtocolError("region %d has a zero size" % i)
        regions.append(MemoryRegion(gpa, size, user_addr, mmap_offset))
    return regions


def request_name(value: int) -> str:
    """The name of a request number, or the number itself when unknown."""
    try:
        return Request(value).name
    except ValueError:
        return "request(%d)" % value


def describe_features(value: int) -> str:
    """Human-readable list of the set bits of a feature mask (for reports)."""
    names = {
        VIRTIO_RING_F_INDIRECT_DESC: "INDIRECT_DESC",
        VIRTIO_RING_F_EVENT_IDX: "EVENT_IDX",
        VHOST_USER_F_PROTOCOL_FEATURES: "PROTOCOL_FEATURES",
        VIRTIO_F_VERSION_1: "VERSION_1",
    }
    bits = [names.get(1 << i, "bit%d" % i) for i in range(64) if value >> i & 1]
    return "|".join(bits) if bits else "none"


def describe_protocol_features(value: int) -> str:
    """Human-readable list of the set protocol feature bits (for reports)."""
    bits = [name for bit, name in sorted(PROTOCOL_FEATURE_NAMES.items()) if value & bit]
    return "|".join(bits) if bits else "none"

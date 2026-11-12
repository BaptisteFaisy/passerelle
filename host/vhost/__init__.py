# SPDX-License-Identifier: Apache-2.0
"""host/vhost - a vhost-user front end, and the first end-to-end virtiofs proof.

Implements requirements:
  - VFS-01, VFS-02, VFS-03, VFS-06: the device side of the vhost-user
    protocol, driving a real ``virtiofsd`` from Python with our own split
    virtqueue in shared memory.
  - ROB-06: every decoder of this package validates its input.

Modules:
  protocol      vhost-user constants, framing and payload codecs
  transport     UNIX socket with SCM_RIGHTS descriptors
  memory        memfd + mmap shared region, the SET_MEM_TABLE payload
  vring         split virtqueue layout and the driver side of it
  device        the front end: negotiation, memory table, queue setup
  fuse_client   FUSE requests posted on a virtqueue, replies harvested
  fake_backend  a Python vhost-user backend, so unit tests need no virtiofsd
  driver        launches virtiofsd and runs a whole FUSE sequence through it

Standard library only.
"""

from __future__ import annotations

from vhost.device import SUPPORTED_FEATURES, SUPPORTED_PROTOCOL_FEATURES, BackendError, VhostUserDevice
from vhost.memory import SharedMemory
from vhost.protocol import ProtocolError, Request
from vhost.transport import Message, Transport
from vhost.vring import SplitVirtqueue

__all__ = [
    "BackendError",
    "Message",
    "ProtocolError",
    "Request",
    "SUPPORTED_FEATURES",
    "SUPPORTED_PROTOCOL_FEATURES",
    "SharedMemory",
    "SplitVirtqueue",
    "Transport",
    "VhostUserDevice",
]

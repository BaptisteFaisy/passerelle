#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# fuse_ref_wire.py - socket framing, request parsing and fault injection for
# the reference FUSE server (tests/fuse_ref/fuse_ref_server.py).
#
# Implements requirements:
#   - VFS-03: the FUSE 7.x wire layout. The ABI itself is declared exactly
#     once for Python, in host/pgwutil/fuse_wire.py, and re-exported here so
#     that the server, its operations mixin and the host-side vhost-user
#     front end all agree. This file only adds the length-prefixed socket
#     framing and the --fault injection modes of this test server.
#
# Standard library only.
from __future__ import annotations

import pathlib
import struct
import sys

# host/pgwutil is the shared Python home of the FUSE wire table.
_HOST_DIR = pathlib.Path(__file__).resolve().parents[2] / "host"
if str(_HOST_DIR) not in sys.path:
    sys.path.insert(0, str(_HOST_DIR))

from pgwutil.fuse_wire import (  # noqa: E402  (the path is set just above)
    DT_DIR,
    DT_LNK,
    DT_REG,
    DT_UNKNOWN,
    FATTR_MODE,
    FATTR_SIZE,
    IN_HDR,
    NO_REPLY_OPS,
    OP_BATCH_FORGET,
    OP_CREATE,
    OP_DESTROY,
    OP_FLUSH,
    OP_FORGET,
    OP_FSYNC,
    OP_FSYNCDIR,
    OP_GETATTR,
    OP_INIT,
    OP_INTERRUPT,
    OP_LINK,
    OP_LOOKUP,
    OP_MKDIR,
    OP_MKNOD,
    OP_OPEN,
    OP_OPENDIR,
    OP_READ,
    OP_READDIR,
    OP_READLINK,
    OP_RELEASE,
    OP_RELEASEDIR,
    OP_RENAME,
    OP_RENAME2,
    OP_RMDIR,
    OP_SETATTR,
    OP_STATFS,
    OP_SYMLINK,
    OP_UNLINK,
    OP_WRITE,
    OUT_HDR,
    FrameError,
    dirent_type,
    pack_attr,
    pack_dirent,
    pack_entry,
    pack_init,
    pack_open_out,
    pack_reply,
    pack_statfs,
    pack_write_out,
    parse_request,
    read_cstr,
    u32_at,
    u64_at,
)

# Fault-injection names honoured by --fault (see README).
FAULTS = ("unknown-errno", "positive-error", "bad-unique", "bad-length",
          "short-header")


def be_u32(n):
    """4-byte big-endian framing length."""
    return struct.pack(">I", n)


def recv_exact(conn, n):
    """Read exactly n bytes from conn, raising FrameError on EOF/short read."""
    out = bytearray()
    while len(out) < n:
        chunk = conn.recv(n - len(out))
        if not chunk:
            raise FrameError("peer closed")
        out += chunk
    return bytes(out)


def read_frame(conn):
    """Read one length-prefixed message. Returns b"" on clean EOF."""
    head = conn.recv(4)
    if head == b"":
        return b""
    while len(head) < 4:
        more = conn.recv(4 - len(head))
        if not more:
            raise FrameError("truncated frame header")
        head += more
    (length,) = struct.unpack(">I", head)
    if length == 0 or length > (1 << 20):
        raise FrameError("implausible frame length %d" % length)
    return recv_exact(conn, length)


def _patch_len(reply, delta):
    return struct.pack("<I", u32_at(reply, 0) + delta) + reply[4:]


def apply_fault(fault, reply, unique):
    """Rewrite `reply` to honour a --fault-injection mode (or not)."""
    if fault == "unknown-errno":
        return pack_reply(unique, b"", -4242)
    if fault == "positive-error":
        return pack_reply(unique, b"", +5)
    if fault == "short-header":
        return reply[:8]
    if fault == "bad-length":
        return _patch_len(reply, 8)
    if fault == "bad-unique":
        return reply[:8] + struct.pack("<Q", unique + 1) + reply[16:]
    return reply

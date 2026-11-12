#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# fuse_ref_wire.py - wire constants, struct helpers and reply builders for the
# reference FUSE server (tests/fuse_ref/fuse_ref_server.py). Imported by the
# server; not run on its own. Standard library only.
from __future__ import annotations

import stat
import struct

# --- FUSE opcodes (wire numbers) --------------------------------------------
OP_LOOKUP = 1
OP_FORGET = 2
OP_GETATTR = 3
OP_SETATTR = 4
OP_READLINK = 5
OP_SYMLINK = 6
OP_MKNOD = 8
OP_MKDIR = 9
OP_UNLINK = 10
OP_RMDIR = 11
OP_RENAME = 12
OP_LINK = 13
OP_OPEN = 14
OP_READ = 15
OP_WRITE = 16
OP_STATFS = 17
OP_RELEASE = 18
OP_FSYNC = 20
OP_FLUSH = 25
OP_INIT = 26
OP_OPENDIR = 27
OP_READDIR = 28
OP_RELEASEDIR = 29
OP_FSYNCDIR = 30
OP_DESTROY = 38
OP_CREATE = 35
OP_INTERRUPT = 36
OP_BATCH_FORGET = 42
OP_RENAME2 = 45

IN_HDR = 40
OUT_HDR = 16

FATTR_MODE = 1 << 0
FATTR_SIZE = 1 << 3

NO_REPLY_OPS = frozenset((OP_FORGET, OP_BATCH_FORGET, OP_INTERRUPT))

DT_DIR = 4
DT_REG = 8
DT_LNK = 10
DT_UNKNOWN = 0

# Fault-injection names honoured by --fault (see README).
FAULTS = ("unknown-errno", "positive-error", "bad-unique", "bad-length",
          "short-header")


class FrameError(Exception):
    """Raised when the client framing or the peer misbehaves."""


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


def u32_at(msg, off):
    return struct.unpack_from("<I", msg, off)[0]


def u64_at(msg, off):
    return struct.unpack_from("<Q", msg, off)[0]


def parse_request(msg):
    """Return (opcode, unique, nodeid); body follows at offset 40."""
    if len(msg) < IN_HDR:
        raise ValueError("short request header")
    if u32_at(msg, 0) != len(msg):
        raise ValueError("length field mismatch")
    return u32_at(msg, 4), u64_at(msg, 8), u64_at(msg, 16)


def read_cstr(data, off):
    """Read one NUL-terminated name at off; return (str, next_off)."""
    end = data.find(b"\x00", off)
    if end < 0:
        raise ValueError("unterminated name")
    name = data[off:end]
    if b"/" in name or name in (b"", b".", b".."):
        raise ValueError("invalid name")
    return name.decode("utf-8", "surrogateescape"), end + 1


def pack_attr(ino, size, mode, nlink, mtime_ns, atime_ns, ctime_ns):
    """One 88-byte fuse_attr (7.32+, trailing flags word present)."""
    blocks = (size + 511) // 512
    (atime_s, atime_n) = divmod(atime_ns, 1_000_000_000)
    (mtime_s, mtime_n) = divmod(mtime_ns, 1_000_000_000)
    (ctime_s, ctime_n) = divmod(ctime_ns, 1_000_000_000)
    return struct.pack(
        "<QQQQQQIIIIIIIIII",
        ino, size, blocks, atime_s, mtime_s, ctime_s,
        atime_n, mtime_n, ctime_n, mode, nlink, 0, 0, 0, 4096, 0,
    )


def dirent_type(mode):
    if stat.S_ISDIR(mode):
        return DT_DIR
    if stat.S_ISREG(mode):
        return DT_REG
    if stat.S_ISLNK(mode):
        return DT_LNK
    return DT_UNKNOWN


# --- reply builders (module level, no state) --------------------------------


def pack_reply(unique, body=b"", error=0):
    return struct.pack("<iiQ", OUT_HDR + len(body), error, unique) + body


def pack_init(minor, max_write):
    return struct.pack(
        "<IIIIHHIIHHI", 7, minor, 131072, 0, 16, 8, max_write, 1, 256, 0, 0,
    ) + b"\x00" * 28


def pack_entry(nodeid, attr):
    return struct.pack("<QQQQII", nodeid, 1, 0, 0, 0, 0) + attr


def pack_open_out(fh):
    return struct.pack("<QII", fh, 0, 0)


def pack_write_out(written):
    return struct.pack("<II", written, 0)


def pack_dirent(ino, off, raw, typ, reclen):
    return struct.pack("<QQII", ino, off, len(raw), typ) \
        + raw + b"\x00" * (reclen - 24 - len(raw))


def pack_statfs(st):
    return struct.pack(
        "<QQQQQIIII", st.f_blocks, st.f_bfree, st.f_bavail, st.f_files,
        st.f_ffree, st.f_frsize, 255, st.f_frsize, 0,
    ) + b"\x00" * 24


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

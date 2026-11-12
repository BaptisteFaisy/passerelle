# SPDX-License-Identifier: Apache-2.0
"""The one Python declaration of the FUSE 7.x wire ABI.

Covers VFS-03 (the FUSE wire layout) and ROB-06 (every accessor is bounds
checked, a malformed message raises instead of being trusted).

Both sides of the protocol use this table so the ABI is declared exactly once:

* ``tests/fuse_ref/fuse_ref_server.py`` - the reference server, which builds
  the replies (``pack_*``) and parses the requests (``read_cstr``, ``u32_at``);
* ``host/vhost/fuse_client.py`` - the vhost-user front end, which builds the
  requests (``pack_in_header``, ``pack_*_in``) and parses the replies
  (``parse_out_header``, ``unpack_*``).

Field order and sizes are restated in our own words from the Linux kernel ABI;
no line of the kernel header is copied (see NOTICE section 4). The C twin of
this table is ``protofuse/src/fusep_abi.h``. Standard library only.
"""

from __future__ import annotations

import stat
import struct

# --- protocol version ------------------------------------------------------
FUSE_MAJOR = 7
FUSE_MINOR = 36

# --- header sizes ----------------------------------------------------------
IN_HDR = 40
OUT_HDR = 16
ROOT_ID = 1

# --- opcodes (wire numbers) ------------------------------------------------
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
OP_CREATE = 35
OP_INTERRUPT = 36
OP_DESTROY = 38
OP_BATCH_FORGET = 42
OP_RENAME2 = 45

NO_REPLY_OPS = frozenset((OP_FORGET, OP_BATCH_FORGET, OP_INTERRUPT))

# --- INIT feature flags (wire bits) ----------------------------------------
F_ASYNC_READ = 1 << 0
F_ATOMIC_O_TRUNC = 1 << 3
F_BIG_WRITES = 1 << 5
F_DONT_MASK = 1 << 6
F_PARALLEL_DIROPS = 1 << 18
F_MAX_PAGES = 1 << 22

# --- SETATTR "valid" bits --------------------------------------------------
FATTR_MODE = 1 << 0
FATTR_UID = 1 << 1
FATTR_GID = 1 << 2
FATTR_SIZE = 1 << 3
FATTR_ATIME = 1 << 4
FATTR_MTIME = 1 << 5
FATTR_CTIME = 1 << 10

# --- dirent types ----------------------------------------------------------
DT_UNKNOWN = 0
DT_DIR = 4
DT_REG = 8
DT_LNK = 10


class FrameError(ValueError):
    """Raised when the peer misbehaves or a message is malformed.

    A ValueError subclass on purpose: the reference server of tests/fuse_ref
    answers a malformed request with EINVAL by catching ValueError.
    """


# --- scalar accessors (bounds checked, little endian) ----------------------


def u32_at(msg, off):
    """One little-endian u32 at *off*; FrameError when it is not there."""
    if off + 4 > len(msg):
        raise FrameError("u32 read past the end at %d/%d" % (off, len(msg)))
    return struct.unpack_from("<I", msg, off)[0]


def i32_at(msg, off):
    """One little-endian i32 at *off*; FrameError when it is not there."""
    if off + 4 > len(msg):
        raise FrameError("i32 read past the end at %d/%d" % (off, len(msg)))
    return struct.unpack_from("<i", msg, off)[0]


def u64_at(msg, off):
    """One little-endian u64 at *off*; FrameError when it is not there."""
    if off + 8 > len(msg):
        raise FrameError("u64 read past the end at %d/%d" % (off, len(msg)))
    return struct.unpack_from("<Q", msg, off)[0]


def parse_request(msg):
    """Return (opcode, unique, nodeid) of a request; the body follows at 40."""
    if len(msg) < IN_HDR:
        raise FrameError("short request header: %d bytes" % len(msg))
    if u32_at(msg, 0) != len(msg):
        raise FrameError("length field %d does not match the %d bytes received" % (u32_at(msg, 0), len(msg)))
    return u32_at(msg, 4), u64_at(msg, 8), u64_at(msg, 16)


def read_cstr(data, off):
    """Read one NUL-terminated name at *off*; return (str, next_off)."""
    end = data.find(b"\x00", off)
    if end < 0:
        raise ValueError("unterminated name")
    name = data[off:end]
    if b"/" in name or name in (b"", b".", b".."):
        raise ValueError("invalid name")
    return name.decode("utf-8", "surrogateescape"), end + 1


# --- request builders (client side) ----------------------------------------


def pack_in_header(opcode, unique, nodeid, length):
    """The 40-byte fuse_in_header; *length* covers header plus body."""
    return struct.pack(
        "<IIQQIIII", length, opcode, unique, nodeid, 0, 0, 0, 0
    )


def pack_init_in(max_readahead, flags, minor=FUSE_MINOR):
    """The 64-byte fuse_init_in body (7.28+ layout, flags2 included)."""
    return struct.pack(
        "<IIIIII",
        FUSE_MAJOR,
        minor,
        max_readahead,
        flags & 0xFFFFFFFF,
        flags >> 32,
        0,
    ) + b"\x00" * 40


def pack_lookup_in(name):
    """LOOKUP body: the NUL-terminated name, nothing else."""
    return name.encode("utf-8", "surrogateescape") + b"\x00"


def pack_getattr_in(has_fh=False, fh=0):
    """The 16-byte fuse_getattr_in body."""
    flags = 1 if has_fh else 0
    return struct.pack("<IIQ", flags, 0, fh if has_fh else 0)


def pack_open_in(flags, open_flags=0):
    """The 8-byte fuse_open_in body (flags already in Linux numbering)."""
    return struct.pack("<II", flags, open_flags)


def pack_read_in(fh, offset, size):
    """The 40-byte body shared by READ and READDIR."""
    return struct.pack("<QQIIQII", fh, offset, size, 0, 0, 0, 0)


def pack_write_in(fh, offset, size):
    """The 40-byte fuse_write_in body; the payload follows in its own buffer."""
    return struct.pack("<QQIIQII", fh, offset, size, 0, 0, 0, 0)


def pack_release_in(fh, is_dir=False):
    """The 24-byte body shared by RELEASE and RELEASEDIR."""
    _ = is_dir  # identical layout for both opcodes
    return struct.pack("<QIIQ", fh, 0, 0, 0)


def pack_flush_in(fh):
    """The 24-byte fuse_flush_in body."""
    return struct.pack("<QIIQ", fh, 0, 0, 0)


def pack_forget_in(nlookup):
    """The 8-byte fuse_forget_in body."""
    return struct.pack("<Q", nlookup)


# --- reply parsers (client side) -------------------------------------------


def parse_out_header(msg):
    """Return (len, error, unique) of a 16-byte fuse_out_header."""
    if len(msg) < OUT_HDR:
        raise FrameError("short output header: %d bytes" % len(msg))
    return u32_at(msg, 0), i32_at(msg, 4), u64_at(msg, 8)


_ATTR_FIELDS = (
    "ino", "size", "blocks", "atime", "mtime", "ctime",
    "atimensec", "mtimensec", "ctimensec", "mode", "nlink",
    "uid", "gid", "rdev", "blksize",
)


def unpack_attr(msg, off=0):
    """One fuse_attr; the trailing 7.32 flags word is optional."""
    if off + 84 > len(msg):
        raise FrameError("truncated fuse_attr")
    fields = struct.unpack_from("<QQQQQQIIIIIIIII", msg, off)
    return dict(zip(_ATTR_FIELDS, fields))


def unpack_entry(msg):
    """fuse_entry_out: nodeid, generation, timeouts, then a fuse_attr."""
    if len(msg) < 40 + 84:
        raise FrameError("truncated fuse_entry_out: %d" % len(msg))
    nodeid = u64_at(msg, 0)
    generation = u64_at(msg, 8)
    entry_valid = u64_at(msg, 16)
    attr_valid = u64_at(msg, 24)
    return {
        "nodeid": nodeid,
        "generation": generation,
        "entry_valid": entry_valid,
        "attr_valid": attr_valid,
        "entry_valid_nsec": u32_at(msg, 32),
        "attr_valid_nsec": u32_at(msg, 36),
        "attr": unpack_attr(msg, 40),
    }


def unpack_attr_out(msg):
    """fuse_attr_out: attr_valid, attr_valid_nsec, dummy, then a fuse_attr."""
    if len(msg) < 16 + 84:
        raise FrameError("truncated fuse_attr_out: %d" % len(msg))
    return {
        "attr_valid": u64_at(msg, 0),
        "attr_valid_nsec": u32_at(msg, 8),
        "attr": unpack_attr(msg, 16),
    }


def unpack_init_out(msg):
    """fuse_init_out; accepts the 8-byte, 24-byte and 64-byte variants."""
    if len(msg) < 8:
        raise FrameError("truncated fuse_init_out: %d" % len(msg))
    out = {"major": u32_at(msg, 0), "minor": u32_at(msg, 4)}
    if len(msg) >= 24:
        out["max_readahead"] = u32_at(msg, 8)
        out["flags"] = u32_at(msg, 12)
        out["max_background"] = u32_at(msg, 16) & 0xFFFF
        out["congestion_threshold"] = u32_at(msg, 18) & 0xFFFF
        out["max_write"] = u32_at(msg, 20)
    if len(msg) >= 32:
        out["time_gran"] = u32_at(msg, 24)
        out["max_pages"] = u32_at(msg, 28) & 0xFFFF
        out["map_alignment"] = u32_at(msg, 30) & 0xFFFF
    return out


def unpack_open_out(msg):
    """fuse_open_out: fh, open_flags, padding."""
    if len(msg) < 8:
        raise FrameError("truncated fuse_open_out")
    return u64_at(msg, 0), u32_at(msg, 8) if len(msg) >= 12 else 0


def unpack_write_out(msg):
    """fuse_write_out: the number of bytes written."""
    if len(msg) < 4:
        raise FrameError("truncated fuse_write_out")
    return u32_at(msg, 0)


def unpack_dirents(msg):
    """Every fuse_dirent of a READDIR reply, in order."""
    out = []
    off = 0
    while off + 24 <= len(msg):
        ino = u64_at(msg, off)
        offset = u64_at(msg, off + 8)
        namelen = u32_at(msg, off + 16)
        dtype = u32_at(msg, off + 20)
        if off + 24 + namelen > len(msg):
            raise FrameError("truncated dirent name at %d" % off)
        name = msg[off + 24:off + 24 + namelen]
        out.append({"ino": ino, "off": offset, "name": name, "type": dtype})
        off += 24 + namelen
        off = (off + 7) & ~7  # dirents are 8-byte aligned
    return out


# --- reply builders (server side) ------------------------------------------


def pack_reply(unique, body=b"", error=0):
    """One fuse_out_header plus *body*."""
    return struct.pack("<iiQ", OUT_HDR + len(body), error, unique) + body


def pack_attr(ino, size, mode, nlink, mtime_ns, atime_ns, ctime_ns):
    """One 88-byte fuse_attr (7.32+, trailing flags word present)."""
    blocks = (size + 511) // 512
    atime_s, atime_n = divmod(atime_ns, 1_000_000_000)
    mtime_s, mtime_n = divmod(mtime_ns, 1_000_000_000)
    ctime_s, ctime_n = divmod(ctime_ns, 1_000_000_000)
    return struct.pack(
        "<QQQQQQIIIIIIIIII",
        ino, size, blocks, atime_s, mtime_s, ctime_s,
        atime_n, mtime_n, ctime_n, mode, nlink, 0, 0, 0, 4096, 0,
    )


def pack_init(minor, max_write):
    """The fuse_init_out body the reference server answers with."""
    return struct.pack(
        "<IIIIHHIIHHI", 7, minor, 131072, 0, 16, 8, max_write, 1, 256, 0, 0,
    ) + b"\x00" * 28


def pack_entry(nodeid, attr):
    """fuse_entry_out minus the trailing fuse_attr (which the caller adds)."""
    return struct.pack("<QQQQII", nodeid, 1, 0, 0, 0, 0) + attr


def pack_open_out(fh):
    """fuse_open_out."""
    return struct.pack("<QII", fh, 0, 0)


def pack_write_out(written):
    """fuse_write_out."""
    return struct.pack("<II", written, 0)


def pack_dirent(ino, off, raw, typ, reclen):
    """One fuse_dirent header plus *raw*, padded to *reclen*."""
    return struct.pack("<QQII", ino, off, len(raw), typ) \
        + raw + b"\x00" * (reclen - 24 - len(raw))


def pack_statfs(st):
    """fuse_kstatfs, with blksize forced to the fragment size."""
    return struct.pack(
        "<QQQQQIIII", st.f_blocks, st.f_bfree, st.f_bavail, st.f_files,
        st.f_ffree, st.f_frsize, 255, st.f_frsize, 0,
    ) + b"\x00" * 24


def dirent_type(mode):
    """The DT_* type byte of a fuse_dirent for a stat mode."""
    if stat.S_ISDIR(mode):
        return DT_DIR
    if stat.S_ISREG(mode):
        return DT_REG
    if stat.S_ISLNK(mode):
        return DT_LNK
    return DT_UNKNOWN

# SPDX-License-Identifier: Apache-2.0
"""Wire format of 9P2000.L: constants, message cursor and packing helpers.

The field layout is the normative one of the 9P2000.L description (see the
diod protocol document, https://github.com/chaos/diod/blob/master/protocol.md):
every message is size[4] type[1] tag[2] followed by the documented fields,
integers are little-endian and a string is length[2] followed by that many
bytes, with no terminator.

Only the wire format lives here: the operations are in ref9p_ops.py and the
server itself in ref9p_server.py.
"""

import stat
import struct

# --- message types (9P2000.L) ------------------------------------------------
RLERROR = 7
TSTATFS, RSTATFS = 8, 9
TLOPEN, RLOPEN = 12, 13
TLCREATE, RLCREATE = 14, 15
TSYMLINK, RSYMLINK = 16, 17
TREADLINK, RREADLINK = 22, 23
TGETATTR, RGETATTR = 24, 25
TSETATTR, RSETATTR = 26, 27
TREADDIR, RREADDIR = 40, 41
TFSYNC, RFSYNC = 50, 51
TLINK, RLINK = 70, 71
TMKDIR, RMKDIR = 72, 73
TRENAMEAT, RRENAMEAT = 74, 75
TUNLINKAT, RUNLINKAT = 76, 77
TVERSION, RVERSION = 100, 101
TATTACH, RATTACH = 104, 105
TFLUSH, RFLUSH = 108, 109
TWALK, RWALK = 110, 111
TREAD, RREAD = 116, 117
TWRITE, RWRITE = 118, 119
TCLUNK, RCLUNK = 120, 121

# --- qid type bits (9P2000.L) -----------------------------------------------
QTDIR = 0x80
QTSYMLINK = 0x02

# --- Tsetattr valid bits -----------------------------------------------------
SA_MODE, SA_UID, SA_GID, SA_SIZE = 0x01, 0x02, 0x04, 0x08
SA_ATIME, SA_MTIME, SA_CTIME = 0x10, 0x20, 0x40
SA_ATIME_SET, SA_MTIME_SET = 0x80, 0x100

# --- Tunlinkat flags ---------------------------------------------------------
AT_REMOVEDIR = 0x200

# Linux dirent types, as used by the readdir records.
DT_DIR, DT_REG, DT_LNK = 4, 8, 10

GETATTR_ALL = 0x3FFF
MAXWELEM = 16
UNKNOWN_ERRNO = 4095  # not a Linux errno: the client must treat it as an error
DEFAULT_MSIZE = 8192


class ProtocolError(Exception):
    """A message that cannot be parsed: the answer is Rlerror(EINVAL)."""


class Reader:
    """Cursor over the body of a 9P message (little-endian fields)."""

    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, count):
        """The next `count` bytes, or a ProtocolError when there are none."""
        if self.pos + count > len(self.data):
            raise ProtocolError("message shorter than announced")
        chunk = self.data[self.pos:self.pos + count]
        self.pos += count
        return chunk

    def u8(self):
        """One byte."""
        return struct.unpack("<B", self.take(1))[0]

    def u16(self):
        """Two bytes, little-endian."""
        return struct.unpack("<H", self.take(2))[0]

    def u32(self):
        """Four bytes, little-endian."""
        return struct.unpack("<I", self.take(4))[0]

    def u64(self):
        """Eight bytes, little-endian."""
        return struct.unpack("<Q", self.take(8))[0]

    def string(self):
        """A length-prefixed string, as raw bytes."""
        return self.take(self.u16())


def pack_string(text):
    """A 9P string: length[2] then the bytes, with no terminator."""
    if isinstance(text, str):
        text = text.encode("utf-8", "surrogateescape")
    return struct.pack("<H", len(text)) + text


def pack_message(mtype, tag, body=b""):
    """A 9P message: size[4] type[1] tag[2] then the body."""
    return struct.pack("<IBH", 7 + len(body), mtype, tag) + body


def qid_type(st):
    """The qid type byte of a stat result (symlinks are not followed)."""
    if stat.S_ISLNK(st.st_mode):
        return QTSYMLINK
    if stat.S_ISDIR(st.st_mode):
        return QTDIR
    return 0


def pack_qid(st):
    """A qid: type[1] version[4] path[8]."""
    return struct.pack("<BIQ", qid_type(st), 0, st.st_ino & 0xFFFFFFFFFFFFFFFF)


def dt_type(st):
    """The Linux dirent type of a stat result."""
    if stat.S_ISLNK(st.st_mode):
        return DT_LNK
    if stat.S_ISDIR(st.st_mode):
        return DT_DIR
    return DT_REG


def pack_stat(st):
    """The attribute block of Rgetattr, in the normative field order.

    mode[4] uid[4] gid[4] nlink[8] rdev[8] size[8] blksize[8] blocks[8] then
    the four timestamp pairs and the four reserved fields (btime, gen,
    data_version), eight bytes each.
    """
    atime_sec, atime_nsec = divmod(st.st_atime_ns, 1000000000)
    mtime_sec, mtime_nsec = divmod(st.st_mtime_ns, 1000000000)
    ctime_sec, ctime_nsec = divmod(st.st_ctime_ns, 1000000000)
    head = struct.pack("<IIIQQQQQ", st.st_mode, st.st_uid, st.st_gid,
                       st.st_nlink, st.st_rdev, st.st_size, st.st_blksize,
                       st.st_blocks)
    tail = struct.pack("<QQQQQQQQQQ", atime_sec, atime_nsec, mtime_sec,
                       mtime_nsec, ctime_sec, ctime_nsec, 0, 0, 0, 0)
    return head + tail


class Fid:
    """One fid: the path it points at and the descriptor when it is open."""

    __slots__ = ("path", "fd", "is_dir")

    def __init__(self, path, fd=-1, is_dir=False):
        self.path = path
        self.fd = fd
        self.is_dir = is_dir

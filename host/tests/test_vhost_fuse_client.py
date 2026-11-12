# SPDX-License-Identifier: Apache-2.0
"""FUSE over a virtqueue: the client against the fake backend, plus ABI checks.

Covers VFS-03 (INIT, LOOKUP, GETATTR, OPEN, READ, WRITE, READDIR, FLUSH,
RELEASE encoded and decoded with the single Python FUSE table) and ROB-06 (a
reply with the wrong ``unique``, a positive error field or a truncated body is
refused).

Two of these tests do not talk to any backend: they compare the Python table of
``host/pgwutil/fuse_wire.py`` with the C table the client library uses
(``protofuse/include/pgw/fusep.h`` and ``protofuse/src/fusep_abi.h``), so the
two languages cannot drift apart silently.
"""

from __future__ import annotations

import pathlib
import re

import pytest

from pgwutil import fuse_wire as fw
from vhost.fake_backend import default_responder
from vhost.fuse_client import FuseReplyError, VirtqueueFuseClient
from vhost.testing import FakeSession, temp_socket_path

REPO = pathlib.Path(__file__).resolve().parents[2]
FUSEP_H = REPO / "protofuse" / "include" / "pgw" / "fusep.h"
FUSEP_ABI_H = REPO / "protofuse" / "src" / "fusep_abi.h"


@pytest.fixture()
def fake():
    with FakeSession(temp_socket_path()) as session:
        yield session


def test_init_negotiates_a_seven_x_session(fake):
    out = fake.fuse.init()
    assert out["major"] == fw.FUSE_MAJOR
    assert out["minor"] >= 8
    assert out["max_write"] > 0
    assert fake.fuse.max_write == out["max_write"]


def test_lookup_returns_an_entry_with_its_attributes(fake):
    entry = fake.fuse.lookup(fw.ROOT_ID, "hello.txt")
    assert entry["nodeid"] == 2
    assert entry["attr"]["size"] == 5
    assert entry["attr"]["mode"] & 0o170000 == 0o100000  # a regular file


def test_getattr_returns_the_packed_attributes(fake):
    out = fake.fuse.getattr(2)
    assert out["attr"]["ino"] == 2
    assert out["attr"]["size"] == 5
    assert out["attr_valid"] == 1


def test_open_read_and_write_round_trip(fake):
    fh = fake.fuse.open(2, 0)
    assert fh == 7
    assert fake.fuse.read(2, fh, 0, 5) == b"hello"
    assert fake.fuse.write(2, fh, 0, b"123456789") == 9


def test_readdir_decodes_every_dirent(fake):
    fh = fake.fuse.open(1, 0, is_dir=True)
    entries = fake.fuse.readdir(1, fh)
    assert [e["name"] for e in entries] == [b"file"]
    assert entries[0]["type"] == fw.DT_REG
    assert entries[0]["ino"] == 2


def test_flush_and_release_are_acknowledged(fake):
    fh = fake.fuse.open(2, 0)
    fake.fuse.flush(2, fh)
    fake.fuse.release(2, fh)
    assert fake.backend.errors == []


def test_forget_returns_its_buffer_without_a_reply(fake):
    assert fake.fuse.forget(2) is True
    assert fake.fuse.unexpected == []


def test_an_errno_reply_is_translated(fake):
    def refuse(opcode, nodeid, body):
        return -2, b""  # -ENOENT, whatever was asked

    with FakeSession(temp_socket_path(), responder=refuse) as session:
        with pytest.raises(FuseReplyError):
            session.fuse.init()  # even INIT is refused by this responder
        with pytest.raises(FuseReplyError) as info:
            session.fuse.lookup(fw.ROOT_ID, "nothing")
    assert info.value.error == -2
    assert "ENOENT" in str(info.value)


def test_a_reply_with_the_wrong_unique_is_refused(fake):
    client = VirtqueueFuseClient(fake.vq)
    with pytest.raises(FuseReplyError):
        client._decode(fw.pack_reply(99, b""), 1)


def test_a_positive_error_field_is_refused(fake):
    client = VirtqueueFuseClient(fake.vq)
    with pytest.raises(FuseReplyError):
        client._decode(fw.pack_reply(1, b"", +5), 1)


def test_a_truncated_reply_is_refused(fake):
    with pytest.raises(fw.FrameError):
        fw.parse_out_header(b"\x00" * 8)


def test_every_request_builder_has_the_size_the_c_abi_declares():
    sizes = _c_sizes(FUSEP_ABI_H)
    built = {
        "PGW_FUSE_INIT_IN_SIZE": len(fw.pack_init_in(0, 0)),
        "PGW_FUSE_GETATTR_IN_SIZE": len(fw.pack_getattr_in()),
        "PGW_FUSE_OPEN_IN_SIZE": len(fw.pack_open_in(0)),
        "PGW_FUSE_READ_IN_SIZE": len(fw.pack_read_in(1, 0, 4096)),
        "PGW_FUSE_WRITE_IN_SIZE": len(fw.pack_write_in(1, 0, 10)),
        "PGW_FUSE_RELEASE_IN_SIZE": len(fw.pack_release_in(1)),
        "PGW_FUSE_FORGET_IN_SIZE": len(fw.pack_forget_in(1)),
        "PGW_FUSE_INIT_OUT_SIZE": len(fw.pack_init(36, 131072)),
        "PGW_FUSE_ENTRY_HDR_SIZE": len(fw.pack_entry(1, b"")),
        "PGW_FUSE_OPEN_OUT_SIZE": len(fw.pack_open_out(1)),
        "PGW_FUSE_WRITE_OUT_SIZE": len(fw.pack_write_out(1)),
        "PGW_FUSE_ATTR_SIZE_FLAGS": len(fw.pack_attr(1, 0, 0o100644, 1, 0, 0, 0)),
        "PGW_FUSE_DIRENT_HDR_SIZE": 24,
    }
    for name, length in built.items():
        assert sizes[name] == length, (
            "%s: C says %d, Python builds %d" % (name, sizes[name], length)
        )


def test_the_opcode_and_header_tables_agree_with_the_c_client():
    header = FUSEP_H.read_text(encoding="utf-8")
    body = header.split("enum pgw_fuse_op {", 1)[1].split("}", 1)[0]
    for name, value in re.findall(r"PGW_FUSE_(\w+)\s*=\s*(\d+)", body):
        python = getattr(fw, "OP_" + name, None)
        assert python == int(value), "OP_%s: C says %s, Python says %s" % (name, value, python)
    assert int(re.search(r"#define PGW_FUSE_IN_HDR_SZ\s+(\d+)", header).group(1)) == fw.IN_HDR
    assert int(re.search(r"#define PGW_FUSE_OUT_HDR_SZ\s+(\d+)", header).group(1)) == fw.OUT_HDR
    assert int(re.search(r"#define PGW_FUSE_MAJOR\s+(\d+)", header).group(1)) == fw.FUSE_MAJOR
    assert int(re.search(r"#define PGW_FUSE_ROOT_ID\s+(\d+)", header).group(1)) == fw.ROOT_ID


def test_the_default_responder_covers_the_documented_operations():
    """The double answers what the driver of driver.py asks, and nothing more."""
    for opcode in (fw.OP_INIT, fw.OP_LOOKUP, fw.OP_GETATTR, fw.OP_OPEN, fw.OP_READ,
                   fw.OP_WRITE, fw.OP_READDIR, fw.OP_FLUSH, fw.OP_RELEASE):
        error, _body = default_responder(opcode, 1, b"\x00" * 64)
        assert error == 0, "opcode %d is not implemented by the double" % opcode
    assert default_responder(fw.OP_SETATTR, 1, b"")[0] == -2


def _c_sizes(path: pathlib.Path) -> dict[str, int]:
    """Every ``#define PGW_FUSE_*_SIZE <n>u`` of a C header, as integers."""
    text = path.read_text(encoding="utf-8")
    found = re.findall(r"#define (PGW_FUSE_\w*(?:SIZE|HDR)\w*)\s+(\d+)u?", text)
    return {name: int(value) for name, value in found}

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# fuse_ref_server.py - a reference FUSE server backed by a real directory.
#
# Speaks the FUSE *kernel wire protocol* over a UNIX stream socket, framed with
# a 4-byte big-endian length prefix per message (the framing the Passerelle
# FUSE transport uses, since /dev/fuse is unavailable in most CI sandboxes).
#
# Test infrastructure for protofuse: the Passerelle client is exercised here
# against a second, independent implementation serving a real folder (the
# "truth"), so VFS-01..VFS-05 stop being self-referential. It is NOT the Linux
# kernel - it is one more implementation of the protocol.
#
# Standard library only. Usage:
#   python3 fuse_ref_server.py --root DIR [--socket PATH] [--ready-fd N]
#       [--minor N] [--max-write N] [--fault f1,f2,...] [--idle-timeout S]
from __future__ import annotations

import argparse
import errno
import os
import socket
import stat
import struct
import sys

from fuse_ref_ops import NamespaceOps
from fuse_ref_wire import (
    FATTR_MODE,
    FATTR_SIZE,
    FAULTS,
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
    FrameError,
    apply_fault,
    be_u32,
    dirent_type,
    pack_attr,
    pack_dirent,
    pack_entry,
    pack_init,
    pack_open_out,
    pack_reply,
    pack_write_out,
    read_cstr,
    read_frame,
    parse_request,
    u32_at,
    u64_at,
)


class Server(NamespaceOps):
    """Node and handle tables over a real directory."""

    def __init__(self, root, minor, max_write, faults):
        self.root = os.path.realpath(root)
        self.minor = minor
        self.max_write = max_write
        self.faults = list(faults)
        self.next_nid = 2  # nodeid 1 is the root, reserved forever
        self.next_fh = 1
        self.nodes = {1: (self.root, 1)}  # nodeid -> (abspath, refcount)
        self.path_to_nid = {self.root: 1}
        self.handles = {}  # fh -> {"path", "fd", "is_dir", "listing"}

    # --- node bookkeeping ---------------------------------------------------

    def node_path(self, nodeid):
        rec = self.nodes.get(nodeid)
        return rec[0] if rec else None

    def add_node(self, path, refs):
        # abspath, not realpath: a symlink must keep its own identity, and
        # resolving it would make the node point at its target.
        path = os.path.abspath(path)
        existing = self.path_to_nid.get(path)
        if existing is not None:
            nid, refc = self.nodes[existing]
            self.nodes[existing] = (path, refc + refs)
            return existing
        nid = self.next_nid
        self.next_nid += 1
        self.nodes[nid] = (path, refs)
        self.path_to_nid[path] = nid
        return nid

    def forget(self, nodeid, nlookup):
        rec = self.nodes.get(nodeid)
        if rec is None or nodeid == 1:
            return
        remaining = rec[1] - nlookup
        if remaining > 0:
            self.nodes[nodeid] = (rec[0], remaining)
        else:
            self.nodes.pop(nodeid, None)
            self.path_to_nid.pop(rec[0], None)

    # --- reply builders -----------------------------------------------------

    def errno_reply(self, unique, eno):
        return pack_reply(unique, b"", -eno)

    def stat_attr(self, path, follow):
        st = (os.stat if follow else os.lstat)(path)
        return pack_attr(st.st_ino, st.st_size, st.st_mode, st.st_nlink,
                         st.st_mtime_ns, st.st_atime_ns, st.st_ctime_ns)

    def attr_body(self, path, follow):
        return struct.pack("<QII", 0, 0, 0) + self.stat_attr(path, follow)

    def entry_body(self, nodeid, path):
        return pack_entry(nodeid, self.stat_attr(path, False))

    def child_path(self, nodeid, name):
        parent = self.node_path(nodeid)
        if parent is None:
            return None, errno.ENOENT
        if not stat.S_ISDIR(os.lstat(parent).st_mode):
            return None, errno.ENOTDIR
        return os.path.join(parent, name), 0

    # --- session (no-reply) handlers ----------------------------------------

    def op_forget(self, nodeid, body, batch):
        if batch:
            count = u32_at(body, 0)
            for i in range(count):
                off = 8 + i * 16
                self.forget(u64_at(body, off), u64_at(body, off + 8))
        elif len(body) >= 8:
            self.forget(nodeid, u64_at(body, 0))

    # --- INIT / metadata ----------------------------------------------------

    def op_init(self, unique, _nodeid, body):
        if len(body) < 8:
            return self.errno_reply(unique, errno.EINVAL)
        if u32_at(body, 0) != 7:
            return self.errno_reply(unique, errno.EPROTO)
        return pack_reply(unique, pack_init(self.minor, self.max_write))

    def op_lookup(self, unique, nodeid, body):
        try:
            name, _ = read_cstr(body, 0)
        except ValueError:
            return self.errno_reply(unique, errno.EINVAL)
        path, err = self.child_path(nodeid, name)
        if err:
            return self.errno_reply(unique, err)
        try:
            os.lstat(path)
        except FileNotFoundError:
            return self.errno_reply(unique, errno.ENOENT)
        nid = self.add_node(path, 1)
        return pack_reply(unique, self.entry_body(nid, path))

    def op_getattr(self, unique, nodeid, _body):
        path = self.node_path(nodeid)
        if path is None:
            return self.errno_reply(unique, errno.ENOENT)
        return pack_reply(unique, self.attr_body(path, False))

    def op_setattr(self, unique, nodeid, body):
        if len(body) < 88:
            return self.errno_reply(unique, errno.EINVAL)
        valid = u32_at(body, 0)
        size = struct.unpack_from("<Q", body, 16)[0]
        mode = struct.unpack_from("<I", body, 68)[0]
        path = self.node_path(nodeid)
        if path is None:
            return self.errno_reply(unique, errno.ENOENT)
        try:
            if valid & FATTR_MODE:
                os.chmod(path, stat.S_IMODE(mode))
            if valid & FATTR_SIZE:
                os.truncate(path, size)
        except OSError as e:
            return self.errno_reply(unique, e.errno)
        return pack_reply(unique, self.attr_body(path, False))

    # --- open / read / write / readdir --------------------------------------

    def op_open(self, unique, nodeid, body, is_dir):
        if len(body) < 8:
            return self.errno_reply(unique, errno.EINVAL)
        path = self.node_path(nodeid)
        if path is None:
            return self.errno_reply(unique, errno.ENOENT)
        st = os.lstat(path)
        if is_dir and not stat.S_ISDIR(st.st_mode):
            return self.errno_reply(unique, errno.ENOTDIR)
        if not is_dir and stat.S_ISDIR(st.st_mode):
            return self.errno_reply(unique, errno.EISDIR)
        return self.open_impl(unique, path, u32_at(body, 0), is_dir)

    def open_impl(self, unique, path, flags, is_dir):
        try:
            fd = None if is_dir else os.open(path, flags & 0o777777)
        except OSError as e:
            return self.errno_reply(unique, e.errno)
        fh = self.next_fh
        self.next_fh += 1
        listing = self.snapshot_dir(path) if is_dir else None
        self.handles[fh] = {"path": path, "fd": fd, "is_dir": is_dir,
                            "listing": listing}
        return pack_reply(unique, pack_open_out(fh))

    def snapshot_dir(self, path):
        names = sorted(os.listdir(path))
        listing = [(".", 4, None), ("..", 4, None)]
        for nm in names:
            st = os.lstat(os.path.join(path, nm))
            listing.append((nm, dirent_type(st.st_mode), st.st_ino))
        return listing

    def io_arg(self, nodeid, body):
        if len(body) < 40:
            return None, 0, 0, errno.EINVAL
        rec = self.handles.get(u64_at(body, 0))
        if rec is None or self.node_path(nodeid) is None:
            return None, 0, 0, errno.EBADF
        return rec, u64_at(body, 8), u32_at(body, 16), 0

    def op_read(self, unique, nodeid, body):
        rec, off, size, err = self.io_arg(nodeid, body)
        if err:
            return self.errno_reply(unique, err)
        if rec["is_dir"]:
            return self.errno_reply(unique, errno.EISDIR)
        try:
            data = os.pread(rec["fd"], size, off)
        except OSError as e:
            return self.errno_reply(unique, e.errno)
        return pack_reply(unique, data)

    def op_write(self, unique, nodeid, body):
        rec, off, size, err = self.io_arg(nodeid, body)
        if err:
            return self.errno_reply(unique, err)
        data = body[40:40 + size]
        if len(data) != size:
            return self.errno_reply(unique, errno.EINVAL)
        if rec["is_dir"]:
            return self.errno_reply(unique, errno.EISDIR)
        try:
            written = os.pwrite(rec["fd"], data, off)
        except OSError as e:
            return self.errno_reply(unique, e.errno)
        return pack_reply(unique, pack_write_out(written))

    def op_readdir(self, unique, nodeid, body):
        rec, off, size, err = self.io_arg(nodeid, body)
        if err:
            return self.errno_reply(unique, err)
        if not rec["is_dir"]:
            return self.errno_reply(unique, errno.ENOTDIR)
        return pack_reply(unique, self.readdir_payload(rec, off, size))

    def readdir_payload(self, rec, off, size):
        out = bytearray()
        for idx, (nm, typ, ino) in enumerate(rec["listing"]):
            if idx < off:
                continue
            raw = nm.encode("utf-8", "surrogateescape")
            reclen = (24 + len(raw) + 7) & ~7
            if len(out) + reclen > size:
                break
            out += pack_dirent(ino or 0, idx + 1, raw, typ, reclen)
        return bytes(out)

    # --- dispatch + fault injection -----------------------------------------

    def take_fault(self):
        return self.faults.pop(0) if self.faults else None

    def dispatch(self, msg):
        (opcode, unique, nodeid) = parse_request(msg)
        body = msg[40:]
        if opcode in NO_REPLY_OPS:
            self.no_reply(opcode, nodeid, body)
            return None
        reply = self.route(opcode, unique, nodeid, body)
        # INIT is never faulted: the session must negotiate before the client
        # can be shown a malformed reply (see the safety-valve tests).
        if opcode == OP_INIT:
            return reply
        return apply_fault(self.take_fault(), reply, unique)

    def no_reply(self, opcode, nodeid, body):
        if opcode == OP_FORGET:
            self.op_forget(nodeid, body, False)
        elif opcode == OP_BATCH_FORGET:
            self.op_forget(nodeid, body, True)

    def route(self, opcode, unique, nodeid, body):
        if opcode == OP_INIT:
            return self.op_init(unique, nodeid, body)
        if opcode == OP_LOOKUP:
            return self.op_lookup(unique, nodeid, body)
        if opcode == OP_GETATTR:
            return self.op_getattr(unique, nodeid, body)
        if opcode == OP_SETATTR:
            return self.op_setattr(unique, nodeid, body)
        if opcode == OP_OPEN:
            return self.op_open(unique, nodeid, body, False)
        if opcode == OP_OPENDIR:
            return self.op_open(unique, nodeid, body, True)
        if opcode == OP_CREATE:
            return self.op_create(unique, nodeid, body)
        if opcode == OP_MKDIR:
            return self.op_mkdir(unique, nodeid, body)
        if opcode == OP_MKNOD:
            return self.errno_reply(unique, errno.ENOSYS)
        if opcode == OP_UNLINK:
            return self.op_unlink(unique, nodeid, body, False)
        if opcode == OP_RMDIR:
            return self.op_unlink(unique, nodeid, body, True)
        return self.route_tail(opcode, unique, nodeid, body)

    def route_tail(self, opcode, unique, nodeid, body):
        if opcode in (OP_RENAME, OP_RENAME2):
            return self.op_rename(unique, nodeid, body, opcode == OP_RENAME2)
        if opcode == OP_SYMLINK:
            return self.op_symlink(unique, nodeid, body)
        if opcode == OP_LINK:
            return self.op_link(unique, nodeid, body)
        if opcode == OP_READLINK:
            return self.op_readlink(unique, nodeid, body)
        if opcode == OP_READ:
            return self.op_read(unique, nodeid, body)
        if opcode == OP_WRITE:
            return self.op_write(unique, nodeid, body)
        if opcode == OP_READDIR:
            return self.op_readdir(unique, nodeid, body)
        if opcode == OP_STATFS:
            return self.op_statfs(unique, nodeid, body)
        if opcode in (OP_RELEASE, OP_RELEASEDIR):
            return self.op_release(unique, nodeid, body)
        if opcode in (OP_FLUSH, OP_FSYNC, OP_FSYNCDIR, OP_DESTROY):
            return pack_reply(unique)
        return self.errno_reply(unique, errno.ENOSYS)


def safe_dispatch(server, msg):
    """Run one request, turning an unexpected OS error into an error reply.

    A real file system answers with the errno of the failed syscall; Python
    raises it instead. Translating here means the reference server never drops
    the connection because one path disappeared under it, and the client sees
    the very errno the kernel would have produced (ENOENT, EISDIR, ...).
    """
    try:
        return server.dispatch(msg)
    except OSError as exc:
        try:
            (_opcode, unique, _nodeid) = parse_request(msg)
        except ValueError:
            return None
        return pack_reply(unique, b"", -(exc.errno or errno.EIO))
    except ValueError:
        return None  # undecodable request: nothing to answer to


def serve(server, conn, idle_timeout):
    conn.settimeout(idle_timeout)
    start_ppid = os.getppid()
    while True:
        # Die with our parent: a test binary that crashed must not leave a
        # server behind (the idle timeout is only the second line of defence).
        if os.getppid() != start_ppid:
            return
        msg = read_frame(conn)
        if msg == b"":
            return
        reply = safe_dispatch(server, msg)
        if reply is not None:
            conn.sendall(be_u32(len(reply)) + reply)


def parse_args(argv):
    """Parse and validate the command line; returns (args, fault list)."""
    ap = argparse.ArgumentParser(description="reference FUSE server")
    ap.add_argument("--root", required=True)
    ap.add_argument("--socket", default=None)
    ap.add_argument("--ready-fd", type=int, default=None)
    ap.add_argument("--minor", type=int, default=36)
    ap.add_argument("--max-write", type=int, default=65536)
    ap.add_argument("--fault", default="")
    ap.add_argument("--idle-timeout", type=float, default=60.0)
    args = ap.parse_args(argv)
    faults = [f for f in args.fault.split(",") if f]
    unknown = [f for f in faults if f not in FAULTS]
    if unknown:
        ap.error("unknown --fault value(s): %s" % ",".join(unknown))
    return args, faults


def listen_and_serve(server, sock_path, ready_fd, idle_timeout):
    """Bind, announce the path on `ready_fd`, then serve exactly one client."""
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        os.unlink(sock_path)
    except FileNotFoundError:
        pass
    listener.bind(sock_path)
    listener.listen(1)
    if ready_fd is not None:
        with os.fdopen(ready_fd, "w", closefd=False) as rdy:
            rdy.write(sock_path + "\n")
            rdy.flush()
    conn, _ = listener.accept()
    listener.close()
    try:
        serve(server, conn, idle_timeout)
    except (FrameError, socket.timeout, OSError):
        pass
    finally:
        conn.close()
        try:
            os.unlink(sock_path)
        except FileNotFoundError:
            pass


def main(argv):
    args, faults = parse_args(argv)
    server = Server(args.root, args.minor, args.max_write, faults)
    # The default socket lives *beside* the root, never inside it: a socket file
    # in the served directory would show up in READDIR and pollute the truth.
    sock_path = args.socket or (server.root.rstrip("/") + ".sock")
    listen_and_serve(server, sock_path, args.ready_fd, args.idle_timeout)


if __name__ == "__main__":
    main(sys.argv[1:])

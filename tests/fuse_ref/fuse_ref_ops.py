#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# fuse_ref_ops.py - namespace-mutation operations for the reference FUSE server
# (tests/fuse_ref/fuse_ref_server.py): CREATE, MKDIR, UNLINK/RMDIR, RENAME,
# SYMLINK, LINK, READLINK, STATFS and RELEASE. A mixin mixed into Server so
# that neither file grows past the 500-line rule. Standard library only.
from __future__ import annotations

import errno
import os
import stat
import struct

from fuse_ref_wire import (
    pack_open_out,
    pack_reply,
    pack_statfs,
    read_cstr,
    u32_at,
    u64_at,
)


class NamespaceOps:
    """CREATE / MKDIR / UNLINK / RMDIR / RENAME / SYMLINK / LINK / READLINK /
    STATFS / RELEASE. Expects the Server mixin to provide: node_path,
    add_node, child_path, entry_body, stat_attr, errno_reply."""

    def op_create(self, unique, nodeid, body):
        if len(body) < 16:
            return self.errno_reply(unique, errno.EINVAL)
        flags = u32_at(body, 0)
        mode = u32_at(body, 4)
        try:
            name, _ = read_cstr(body, 16)
        except ValueError:
            return self.errno_reply(unique, errno.EINVAL)
        path, err = self.child_path(nodeid, name)
        if err:
            return self.errno_reply(unique, err)
        try:
            fd = os.open(path, (flags & 0o777777) | os.O_CREAT,
                         stat.S_IMODE(mode))
        except OSError as e:
            return self.errno_reply(unique, e.errno)
        fh = self.next_fh
        self.next_fh += 1
        self.handles[fh] = {"path": path, "fd": fd, "is_dir": False,
                            "listing": None}
        nid = self.add_node(path, 1)
        return pack_reply(unique, self.entry_body(nid, path)
                          + pack_open_out(fh))

    def op_mkdir(self, unique, nodeid, body):
        mode = u32_at(body, 0) if len(body) >= 4 else 0o777
        try:
            name, _ = read_cstr(body, 8)
        except ValueError:
            return self.errno_reply(unique, errno.EINVAL)
        path, err = self.child_path(nodeid, name)
        if err:
            return self.errno_reply(unique, err)
        try:
            os.mkdir(path, stat.S_IMODE(mode))
        except OSError as e:
            return self.errno_reply(unique, e.errno)
        return pack_reply(unique, self.entry_body(self.add_node(path, 1), path))

    def op_unlink(self, unique, nodeid, body, is_dir):
        try:
            name, _ = read_cstr(body, 0)
        except ValueError:
            return self.errno_reply(unique, errno.EINVAL)
        path, err = self.child_path(nodeid, name)
        if err:
            return self.errno_reply(unique, err)
        mode = os.lstat(path).st_mode
        if is_dir and not stat.S_ISDIR(mode):
            return self.errno_reply(unique, errno.ENOTDIR)
        if not is_dir and stat.S_ISDIR(mode):
            return self.errno_reply(unique, errno.EISDIR)
        try:
            (os.rmdir if is_dir else os.unlink)(path)
        except OSError as e:
            return self.errno_reply(unique, e.errno)
        self.path_to_nid.pop(os.path.abspath(path), None)
        return pack_reply(unique)

    def op_rename(self, unique, nodeid, body, has_flags):
        if len(body) < 8:
            return self.errno_reply(unique, errno.EINVAL)
        newdir = struct.unpack_from("<Q", body, 0)[0]
        names_at = 16 if has_flags else 8
        try:
            oldname, off = read_cstr(body, names_at)
            newname, _ = read_cstr(body, off)
        except ValueError:
            return self.errno_reply(unique, errno.EINVAL)
        oldparent, newparent = self.node_path(nodeid), self.node_path(newdir)
        if oldparent is None or newparent is None:
            return self.errno_reply(unique, errno.ENOENT)
        try:
            os.rename(os.path.join(oldparent, oldname),
                      os.path.join(newparent, newname))
        except OSError as e:
            return self.errno_reply(unique, e.errno)
        return pack_reply(unique)

    def op_symlink(self, unique, nodeid, body):
        try:
            name, off = read_cstr(body, 0)
            target, _ = read_cstr(body, off)
        except ValueError:
            return self.errno_reply(unique, errno.EINVAL)
        parent = self.node_path(nodeid)
        if parent is None:
            return self.errno_reply(unique, errno.ENOENT)
        path = os.path.join(parent, name)
        try:
            os.symlink(target, path)
        except OSError as e:
            return self.errno_reply(unique, e.errno)
        return pack_reply(unique, self.entry_body(self.add_node(path, 1), path))

    def op_link(self, unique, nodeid, body):
        if len(body) < 8:
            return self.errno_reply(unique, errno.EINVAL)
        oldpath = self.node_path(u64_at(body, 0))
        try:
            name, _ = read_cstr(body, 8)
        except ValueError:
            return self.errno_reply(unique, errno.EINVAL)
        parent = self.node_path(nodeid)
        if oldpath is None or parent is None:
            return self.errno_reply(unique, errno.ENOENT)
        path = os.path.join(parent, name)
        try:
            os.link(oldpath, path)
        except OSError as e:
            return self.errno_reply(unique, e.errno)
        return pack_reply(unique, self.entry_body(self.add_node(path, 1), path))

    def op_readlink(self, unique, nodeid, _body):
        path = self.node_path(nodeid)
        if path is None:
            return self.errno_reply(unique, errno.ENOENT)
        if not stat.S_ISLNK(os.lstat(path).st_mode):
            return self.errno_reply(unique, errno.EINVAL)
        target = os.readlink(path).encode("utf-8", "surrogateescape")
        return pack_reply(unique, target + b"\x00")

    def op_statfs(self, unique, _nodeid, _body):
        return pack_reply(unique, pack_statfs(os.statvfs(self.root)))

    def op_release(self, unique, _nodeid, body):
        if len(body) >= 8:
            rec = self.handles.pop(u64_at(body, 0), None)
            if rec and rec["fd"] is not None:
                os.close(rec["fd"])
        return pack_reply(unique)

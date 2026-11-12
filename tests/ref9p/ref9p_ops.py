# SPDX-License-Identifier: Apache-2.0
"""The 9P2000.L operations of the reference server.

One function per message, all taking the server state and the request cursor
and returning (reply type, reply body). Every failure is raised as an OSError
carrying a Linux errno: the server turns it into an Rlerror, exactly like
QEMU's 9pfs does, and the client translates that number back into its own
error code (P9-04).

Two 9P rules shape the code and are worth stating:

  * a walk stops at a symbolic link instead of crossing it, and a walk that
    does not consume every name returns the qids it did walk (a "partial
    walk") rather than an error - that is how a missing component is reported;
  * newfid is created only when every name of the walk was consumed.
"""

import errno
import os
import stat
import struct
import time

from ref9p_wire import (AT_REMOVEDIR, GETATTR_ALL, MAXWELEM, RATTACH, RCLUNK,
                        RFLUSH, RFSYNC, RGETATTR, RLINK, RLCREATE, RLOPEN,
                        RMKDIR, RREAD, RREADDIR, RREADLINK, RRENAMEAT,
                        RSETATTR, RSTATFS, RSYMLINK, RUNLINKAT, RVERSION,
                        RWALK, RWRITE, SA_ATIME, SA_ATIME_SET, SA_GID, SA_MTIME,
                        SA_MTIME_SET, SA_MODE, SA_SIZE, SA_UID, TATTACH,
                        TCLUNK, TFLUSH, TFSYNC, TGETATTR, TLINK, TLCREATE,
                        TLOPEN, TMKDIR, TREAD, TREADDIR, TREADLINK, TRENAMEAT,
                        TSETATTR, TSTATFS, TSYMLINK, TUNLINKAT, TVERSION, TWALK,
                        TWRITE, Fid, dt_type, pack_qid, pack_stat, pack_string)


def h_version(server, r):
    """Tversion: the dialect we speak and the msize we can handle."""
    msize = r.u32()
    r.string()  # the version the client proposes
    body = struct.pack("<I", min(msize, server.msize)) + pack_string("9P2000.L")
    return RVERSION, body


def h_attach(server, r):
    """Tattach: bind a fid to the served root."""
    fid = r.u32()
    r.u32()  # afid: we do no authentication
    r.string()  # uname
    r.string()  # aname: the tree is fixed by --root
    r.u32()  # n_uname
    if fid in server.fids:
        raise OSError(errno.EBADF, "fid already in use")
    st = os.stat(server.root)
    server.fids[fid] = Fid(server.root, -1, stat.S_ISDIR(st.st_mode))
    server.attached = True
    return RATTACH, pack_qid(st)


def h_flush(server, r):
    """Tflush: nothing is ever in flight, so the answer is immediate."""
    r.u16()  # oldtag
    return RFLUSH, b""


def h_clunk(server, r):
    """Tclunk: forget a fid and close its descriptor."""
    fid = r.u32()
    found = server.fids.pop(fid, None)
    if found is None:
        raise OSError(errno.EBADF, "unknown fid %d" % fid)
    if found.fd >= 0:
        os.close(found.fd)
    return RCLUNK, b""


def h_walk(server, r):
    """Twalk: consume as many names as the tree allows, report the qids."""
    fid = r.u32()
    newfid = r.u32()
    count = r.u16()
    if count > MAXWELEM:
        raise OSError(errno.EINVAL, "too many names in one walk")
    names = [r.string() for _ in range(count)]
    current = server.fid(fid)
    if not current.is_dir:
        raise OSError(errno.ENOTDIR, "fid is not a directory")
    walked = []
    path = current.path
    for name in names:
        try:
            candidate = server.child(path, name)
            st = os.lstat(candidate)
        except OSError as exc:
            if exc.errno in (errno.ENOENT, errno.ENOTDIR):
                break  # a partial walk: the client turns it into an error
            raise
        walked.append(st)
        path = candidate
        if stat.S_ISLNK(st.st_mode):
            break  # a 9P server does not cross a symlink during a walk
    if len(walked) == count:
        st = walked[-1] if walked else os.lstat(path)
        server.fids[newfid] = Fid(path, -1, stat.S_ISDIR(st.st_mode))
    body = struct.pack("<H", len(walked))
    for st in walked:
        body += pack_qid(st)
    return RWALK, body


def h_lopen(server, r):
    """Tlopen: open the object a fid points at, with Linux open flags."""
    fid = r.u32()
    flags = r.u32()
    found = server.fid(fid)
    if found.fd >= 0:
        os.close(found.fd)
        found.fd = -1
    found.fd = os.open(found.path, flags, 0o644)
    return RLOPEN, pack_qid(os.fstat(found.fd)) + struct.pack("<I", 0)


def h_lcreate(server, r):
    """Tlcreate: create a regular file and rebind the fid to it."""
    fid = r.u32()
    name = r.string()
    flags = r.u32()
    mode = r.u32()
    r.u32()  # gid
    parent = server.fid(fid)
    path = server.child(parent.path, name)
    fd = os.open(path, flags | os.O_CREAT, mode & 0o7777)
    server.fids[fid] = Fid(path, fd, False)
    return RLCREATE, pack_qid(os.fstat(fd)) + struct.pack("<I", 0)


def h_read(server, r):
    """Tread: at most `count` bytes from the offset of an open fid."""
    fid = r.u32()
    offset = r.u64()
    count = r.u32()
    found = server.fid(fid)
    if found.fd < 0:
        raise OSError(errno.EBADF, "fid is not open")
    data = os.pread(found.fd, min(count, server.msize), offset)
    return RREAD, struct.pack("<I", len(data)) + data


def h_write(server, r):
    """Twrite: write the payload at the offset, until every byte is in."""
    fid = r.u32()
    offset = r.u64()
    count = r.u32()
    data = r.take(count)
    found = server.fid(fid)
    if found.fd < 0:
        raise OSError(errno.EBADF, "fid is not open")
    done = 0
    while done < len(data):
        done += os.pwrite(found.fd, data[done:], offset + done)
    return RWRITE, struct.pack("<I", done)


def h_fsync(server, r):
    """Tfsync: push the open descriptor to the real file system."""
    fid = r.u32()
    r.u32()  # datasync
    found = server.fid(fid)
    if found.fd >= 0:
        os.fsync(found.fd)
    return RFSYNC, b""


def h_getattr(server, r):
    """Tgetattr: the whole attribute block, symlinks not followed."""
    fid = r.u32()
    request = r.u64()
    found = server.fid(fid)
    st = os.lstat(found.path)
    valid = request & GETATTR_ALL
    return RGETATTR, struct.pack("<Q", valid) + pack_qid(st) + pack_stat(st)


def apply_setattr(path, valid, mode, uid, gid, size, atime, mtime):
    """Apply the fields the client asked to change."""
    if valid & SA_MODE:
        os.chmod(path, mode & 0o7777)
    if valid & SA_UID:
        os.chown(path, uid, -1)
    if valid & SA_GID:
        os.chown(path, -1, gid)
    if valid & SA_SIZE:
        os.truncate(path, size)
    if not valid & (SA_ATIME | SA_MTIME):
        return
    now = time.time_ns()
    st = os.stat(path)
    if valid & SA_ATIME:
        atime_ns = atime[0] * 1000000000 + atime[1] if valid & SA_ATIME_SET \
            else now
    else:
        atime_ns = st.st_atime_ns
    if valid & SA_MTIME:
        mtime_ns = mtime[0] * 1000000000 + mtime[1] if valid & SA_MTIME_SET \
            else now
    else:
        mtime_ns = st.st_mtime_ns
    os.utime(path, ns=(atime_ns, mtime_ns))


def h_setattr(server, r):
    """Tsetattr: chmod, chown, truncate and utimes behind one valid mask."""
    fid = r.u32()
    valid = r.u32()
    mode = r.u32()
    uid = r.u32()
    gid = r.u32()
    size = r.u64()
    atime = (r.u64(), r.u64())
    mtime = (r.u64(), r.u64())
    found = server.fid(fid)
    apply_setattr(found.path, valid, mode, uid, gid, size, atime, mtime)
    return RSETATTR, b""


def h_readdir(server, r):
    """Treaddir: records qid[13] offset[8] type[1] name[s], at most count bytes.

    The cookie is the index of the next entry: a stable, restart-proof
    position that does not depend on the order the kernel reports names in.
    """
    fid = r.u32()
    offset = r.u64()
    count = r.u32()
    found = server.fid(fid)
    if not found.is_dir:
        raise OSError(errno.ENOTDIR, "fid is not a directory")
    names = sorted(os.listdir(found.path))
    index = int(offset)
    out = b""
    while index < len(names):
        st = os.lstat(os.path.join(found.path, names[index]))
        record = pack_qid(st) + struct.pack("<Q", index + 1)
        record += struct.pack("<B", dt_type(st)) + pack_string(names[index])
        if len(out) + len(record) > count:
            break
        out += record
        index += 1
    return RREADDIR, struct.pack("<I", len(out)) + out


def h_mkdir(server, r):
    """Tmkdir: a new directory in the directory a fid points at."""
    dfid = r.u32()
    name = r.string()
    mode = r.u32()
    r.u32()  # gid
    path = server.child(server.fid(dfid).path, name)
    os.mkdir(path, mode & 0o7777)
    return RMKDIR, pack_qid(os.lstat(path))


def h_unlinkat(server, r):
    """Tunlinkat: remove a name, or a directory when AT_REMOVEDIR is set."""
    dfid = r.u32()
    name = r.string()
    flags = r.u32()
    path = server.child(server.fid(dfid).path, name)
    if flags & AT_REMOVEDIR:
        os.rmdir(path)
    else:
        os.unlink(path)
    return RUNLINKAT, b""


def h_renameat(server, r):
    """Trenameat: move a name from one directory to another."""
    olddfid = r.u32()
    oldname = r.string()
    newdfid = r.u32()
    newname = r.string()
    old = server.child(server.fid(olddfid).path, oldname)
    new = server.child(server.fid(newdfid).path, newname)
    os.rename(old, new)
    return RRENAMEAT, b""


def h_symlink(server, r):
    """Tsymlink: a new symbolic link pointing at symtgt."""
    dfid = r.u32()
    name = r.string()
    target = r.string()
    r.u32()  # gid
    path = server.child(server.fid(dfid).path, name)
    os.symlink(target.decode("utf-8", "surrogateescape"), path)
    return RSYMLINK, pack_qid(os.lstat(path))


def h_readlink(server, r):
    """Treadlink: the target of the link a fid points at."""
    fid = r.u32()
    found = server.fid(fid)
    return RREADLINK, pack_string(os.readlink(found.path))


def h_link(server, r):
    """Tlink: a second name for the object a fid points at."""
    dfid = r.u32()
    fid = r.u32()
    name = r.string()
    target = server.fid(fid)
    path = server.child(server.fid(dfid).path, name)
    os.link(target.path, path)
    return RLINK, b""


def h_statfs(server, r):
    """Tstatfs: the statistics of the real file system under the fid."""
    fid = r.u32()
    found = server.fid(fid)
    st = os.statvfs(found.path)
    body = struct.pack("<IIQQQQQQI", 0, st.f_bsize, st.f_blocks, st.f_bfree,
                       st.f_bavail, st.f_files, st.f_ffree, 0, st.f_namemax)
    return RSTATFS, body


# The dispatch table of the reference server: one entry per message we serve.
# Anything else answers Rlerror(ENOSYS), which is what a real server does for
# an operation it does not implement.
HANDLERS = {
    TVERSION: h_version,
    TATTACH: h_attach,
    TFLUSH: h_flush,
    TCLUNK: h_clunk,
    TWALK: h_walk,
    TLOPEN: h_lopen,
    TLCREATE: h_lcreate,
    TREAD: h_read,
    TWRITE: h_write,
    TFSYNC: h_fsync,
    TGETATTR: h_getattr,
    TSETATTR: h_setattr,
    TREADDIR: h_readdir,
    TMKDIR: h_mkdir,
    TUNLINKAT: h_unlinkat,
    TRENAMEAT: h_renameat,
    TSYMLINK: h_symlink,
    TREADLINK: h_readlink,
    TLINK: h_link,
    TSTATFS: h_statfs,
}

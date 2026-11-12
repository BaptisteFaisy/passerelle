#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Reference 9P2000.L server used to validate the Passerelle 9P client.

The client of proto9p/ used to be tested only against an in-memory fake
server, which made the conformance claims of P9-01..P9-10 self-referential:
the same author wrote both sides of the conversation. This server is the
second, independent side: a real implementation of 9P2000.L that speaks the
protocol on a TCP socket, over a *real* directory, and answers with the Linux
errno numbers a real server would answer with (as QEMU's 9pfs does).

Wire format
-----------
Each 9P message is sent as a 4-byte big-endian length followed by the message
itself. The message is the normative 9P2000.L one: size[4] type[1] tag[2]
followed by the documented fields, integers little-endian, strings sent as
length[2] plus the bytes (see the diod protocol description,
https://github.com/chaos/diod/blob/master/protocol.md).

Usage
-----
    python3 ref9p_server.py --root /tmp/share --port 0 --ready-fd 3

--port 0 asks the kernel for a free port; the port that was chosen is written
to --ready-fd as an ASCII decimal number followed by a newline, so a test
harness never has to guess or to poll. The server is silent on stdout.

--fault injects a deliberate protocol violation once the session is attached,
to exercise the client's validation path (P9-09, ROB-06):

    unknown-errno  every reply becomes an Rlerror carrying errno 4095, a
                   number no Linux errno table knows;
    truncate       every reply loses its last body byte, so the message is
                   well framed but too short for its own type.

Only the Python standard library is used. The protocol operations live in
ref9p_ops.py and the wire format in ref9p_wire.py.
"""

import argparse
import errno
import os
import socket
import struct
import sys
import threading

from ref9p_ops import HANDLERS
from ref9p_wire import (DEFAULT_MSIZE, ProtocolError, RLERROR, Reader,
                        UNKNOWN_ERRNO, pack_message)


class Ref9pServer:
    """The 9P2000.L state machine, serving one real directory."""

    def __init__(self, root, msize=DEFAULT_MSIZE, fault="none"):
        self.root = os.path.realpath(root)
        self.msize = msize
        self.fault = fault
        self.fids = {}
        self.attached = False

    # --- state ---------------------------------------------------------------
    def fid(self, num):
        """The fid `num`, or EBADF: an unknown fid is never a silent success."""
        found = self.fids.get(num)
        if found is None:
            raise OSError(errno.EBADF, "unknown fid %d" % num)
        return found

    def child(self, parent_path, name):
        """The path of `name` inside `parent_path`, refusing anything unusual."""
        text = name.decode("utf-8", "surrogateescape")
        if text in ("", ".", "..") or "/" in text or "\0" in text:
            raise OSError(errno.EINVAL, "bad name")
        return os.path.join(parent_path, text)

    # --- dispatch ------------------------------------------------------------
    def dispatch(self, payload):
        """Turn one request into the bytes of its reply."""
        tag = 0
        fault_zone = self.attached  # a fault never breaks the handshake itself
        try:
            reader = Reader(payload)
            size = reader.u32()
            mtype = reader.u8()
            tag = reader.u16()
            if size != len(payload):
                raise ProtocolError("size field does not match the frame")
            handler = HANDLERS.get(mtype)
            if handler is None:
                raise OSError(errno.ENOSYS, "unsupported message %d" % mtype)
            reply_type, body = handler(self, reader)
        except ProtocolError:
            return pack_message(RLERROR, tag, struct.pack("<I", errno.EINVAL))
        except OSError as exc:
            code = exc.errno if exc.errno else errno.EIO
            return pack_message(RLERROR, tag, struct.pack("<I", code))
        reply = pack_message(reply_type, tag, body)
        return self.apply_fault(reply) if fault_zone else reply

    def apply_fault(self, reply):
        """Inject the deliberate violation asked for on the command line."""
        if self.fault == "none":
            return reply
        mtype = struct.unpack("<B", reply[4:5])[0]
        tag = struct.unpack("<H", reply[5:7])[0]
        if self.fault == "unknown-errno":
            return pack_message(RLERROR, tag, struct.pack("<I", UNKNOWN_ERRNO))
        if self.fault == "truncate":
            body = reply[7:]
            if not body:
                return reply
            return pack_message(mtype, tag, body[:-1])
        return reply


def recv_exact(sock, count):
    """Read exactly `count` bytes; None when the peer went away."""
    chunks = []
    got = 0
    while got < count:
        chunk = sock.recv(count - got)
        if not chunk:
            return None
        chunks.append(chunk)
        got += len(chunk)
    return b"".join(chunks)


def serve_connection(sock, server):
    """Serve one client until it closes the connection."""
    with sock:
        while True:
            header = recv_exact(sock, 4)
            if header is None:
                return
            length = struct.unpack(">I", header)[0]
            payload = recv_exact(sock, length)
            if payload is None:
                return
            reply = server.dispatch(payload)
            sock.sendall(struct.pack(">I", len(reply)) + reply)


def serve(server, listener):
    """Accept connections forever, one thread each."""
    while True:
        try:
            conn, _ = listener.accept()
        except OSError:
            return
        thread = threading.Thread(target=serve_connection, args=(conn, server))
        thread.daemon = True
        thread.start()


def parse_args(argv):
    """Command line: --root, --port, --ready-fd, --msize, --fault."""
    parser = argparse.ArgumentParser(description="reference 9P2000.L server")
    parser.add_argument("--root", required=True, help="directory to serve")
    parser.add_argument("--port", type=int, default=0,
                        help="TCP port to listen on (0: any free port)")
    parser.add_argument("--ready-fd", type=int, default=-1,
                        help="descriptor the chosen port is written to")
    parser.add_argument("--msize", type=int, default=DEFAULT_MSIZE,
                        help="maximum message size the server accepts")
    parser.add_argument("--fault", default="none",
                        choices=("none", "unknown-errno", "truncate"),
                        help="deliberate protocol violation to inject")
    return parser.parse_args(argv)


def listen(args):
    """Bind, listen and announce the port through --ready-fd."""
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", args.port))
    listener.listen(8)
    port = listener.getsockname()[1]
    if args.ready_fd >= 0:
        os.write(args.ready_fd, ("%d\n" % port).encode("ascii"))
        os.close(args.ready_fd)
    return listener, port


def main(argv=None):
    """Start the server and serve until it is killed."""
    args = parse_args(argv if argv is not None else sys.argv[1:])
    if not os.path.isdir(args.root):
        sys.stderr.write("ref9p_server: no such directory: %s\n" % args.root)
        return 2
    server = Ref9pServer(args.root, args.msize, args.fault)
    listener, _port = listen(args)
    serve(server, listener)
    return 0


if __name__ == "__main__":
    sys.exit(main())

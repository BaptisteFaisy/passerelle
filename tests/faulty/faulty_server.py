#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""faulty_server.py - a 9P2000.L server that misbehaves on purpose (ROB-07).

Gap 20 of docs/gap-analysis.md: "a deliberately faulty server must not crash
the client" was unproven because no faulty server existed. This is it. It is a
real 9P2000.L server - the reference implementation of tests/ref9p/ answers
every request correctly - wrapped in a layer that breaks one rule of the
protocol per run, chosen by --fault:

Message-level faults (the frame is well formed, the 9P message is not):
    truncate         the body loses its last byte, size[4] stays consistent;
    short-frame      the whole reply loses its last byte, so size[4] lies;
    oversize         size[4] claims 2 GiB for a short message;
    bad-tag          the reply answers a tag the client never used;
    out-of-order     the reply carries the tag of an earlier request;
    garbage          bytes that are not a message at all;
    read-count-lie   Rread claims more data bytes than the message carries;
    walk-overflow    Rwalk claims more qids than the message carries;
    version-mismatch Rversion answers "9P2000", a version the client refuses.

Transport-level faults (the message would be fine, the framing is not):
    unsolicited      a reply for a request that was never sent comes first;
    huge-frame       the length prefix claims 4 GiB;
    never-ends       the length prefix promises bytes that never arrive;
    late-reply       the reply arrives after the client gave up;
    close-mid-frame  the connection dies in the middle of a frame.

Message faults apply once the session is attached, so the handshake always
succeeds and the client reaches its data path; version-mismatch applies to the
handshake itself. Every fault is injected on *every* reply of the run, which
keeps the test deterministic.

Only the Python standard library is used, and the protocol itself is imported
from tests/ref9p/ rather than written a second time.

Usage (the same command line as ref9p_server.py, so the C harness of
tests/ref9p/ starts it unchanged):
    python3 faulty_server.py --root DIR --port 0 --ready-fd 3 --fault MODE
"""

import argparse
import errno
import os
import struct
import sys
import threading
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, os.pardir, "ref9p"))

from ref9p_server import Ref9pServer, listen, recv_exact  # noqa: E402
from ref9p_wire import (  # noqa: E402
    DEFAULT_MSIZE,
    RLERROR,
    RREAD,
    RVERSION,
    RWALK,
    TREAD,
    TVERSION,
    TWALK,
    pack_message,
    pack_string,
)

MSG_FAULTS = ("truncate", "short-frame", "oversize", "bad-tag", "out-of-order",
              "garbage", "read-count-lie", "walk-overflow", "version-mismatch")
TRANSPORT_FAULTS = ("unsolicited", "huge-frame", "never-ends", "late-reply",
                    "close-mid-frame")
FAULTS = MSG_FAULTS + TRANSPORT_FAULTS

# size[4] == 0x03020100: below the 7-byte header minimum and not a message.
GARBAGE = bytes(range(64))
OVERSIZE = 0x7FFFFFFF
HUGE_FRAME = 0xFFFFFFFF
PROMISED_EXTRA = 4096       # never-ends: bytes promised and never sent
LATE_REPLY_S = 1.2          # late-reply: longer than the client's timeout
NOTAG = 0xFFFF


class FaultyServer(Ref9pServer):
    """The reference server, with one protocol rule broken per run."""

    def __init__(self, root, msize=DEFAULT_MSIZE, fault="none"):
        # "none" for the reference layer: the faults are ours, applied on top.
        Ref9pServer.__init__(self, root, msize, "none")
        self.fault = fault
        self.prev_tag = None

    # --- message level ------------------------------------------------------
    def dispatch(self, payload):
        """Answer one request, then break the reply when a fault is armed."""
        zone = self.attached or self.fault == "version-mismatch"
        mtype = payload[4] if len(payload) >= 5 else 0
        tag = struct.unpack_from("<H", payload, 5)[0] if len(payload) >= 7 else 0
        reply = Ref9pServer.dispatch(self, payload)
        prev, self.prev_tag = self.prev_tag, tag
        if not zone:
            return reply
        return self.break_reply(reply, mtype, tag, prev)

    def break_reply(self, reply, mtype, tag, prev_tag):
        """Return `reply` with the armed violation applied to it."""
        fault = self.fault
        if fault == "truncate":
            body = reply[7:]
            return reply if not body else self.repack(reply, body[:-1])
        if fault == "short-frame":
            return reply[:-1] if len(reply) > 1 else reply
        if fault == "oversize":
            return struct.pack("<I", OVERSIZE) + reply[4:]
        if fault == "bad-tag":
            return self.retag(reply, (tag + 1) & 0xFFFF)
        if fault == "out-of-order":
            stale = NOTAG if prev_tag is None else (prev_tag + 7) & 0xFFFF
            return self.retag(reply, stale)
        if fault == "garbage":
            return GARBAGE
        if fault == "version-mismatch" and mtype == TVERSION:
            body = struct.pack("<I", self.msize) + pack_string("9P2000")
            return pack_message(RVERSION, tag, body)
        if fault == "read-count-lie" and mtype == TREAD:
            return self.lie_read_count(reply)
        if fault == "walk-overflow" and mtype == TWALK:
            return self.overflow_walk(reply)
        return reply

    @staticmethod
    def repack(reply, body):
        """Rebuild a message with the same type and tag but a new body."""
        tag = struct.unpack_from("<H", reply, 5)[0]
        return pack_message(reply[4], tag, body)

    @staticmethod
    def retag(reply, tag):
        """The same message, answering a different tag."""
        return reply[:5] + struct.pack("<H", tag) + reply[7:]

    @staticmethod
    def lie_read_count(reply):
        """Rread: count[4] claims 100 more bytes than the message carries."""
        if reply[4] != RREAD or len(reply) < 11:
            return reply
        count = struct.unpack_from("<I", reply, 7)[0]
        return reply[:7] + struct.pack("<I", count + 100) + reply[11:]

    @staticmethod
    def overflow_walk(reply):
        """Rwalk: nwname[2] claims three more qids than the message carries."""
        if reply[4] != RWALK or len(reply) < 9:
            return reply
        count = struct.unpack_from("<H", reply, 7)[0]
        return reply[:7] + struct.pack("<H", count + 3) + reply[9:]


def frame(payload):
    """The transport frame: a 4-byte big-endian length, then the message."""
    return struct.pack(">I", len(payload)) + payload


def send_faulty(sock, server, reply, zone):
    """Send one reply, breaking the framing when a transport fault is armed.

    `zone` is server.attached as it was *before* the reply was built, so a
    transport fault never breaks the handshake itself. Returns False when the
    connection must be dropped afterwards.
    """
    fault = server.fault
    if not zone or fault not in TRANSPORT_FAULTS:
        sock.sendall(frame(reply))
        return True
    if fault == "unsolicited":
        stray = pack_message(RLERROR, NOTAG, struct.pack("<I", errno.EIO))
        sock.sendall(frame(stray))
    if fault == "huge-frame":
        sock.sendall(struct.pack(">I", HUGE_FRAME) + reply)
        return True
    if fault == "never-ends":
        sock.sendall(struct.pack(">I", len(reply) + PROMISED_EXTRA) + reply)
        return True
    if fault == "late-reply":
        time.sleep(LATE_REPLY_S)
    if fault == "close-mid-frame":
        half = max(1, len(reply) // 2)
        sock.sendall(struct.pack(">I", len(reply)) + reply[:half])
        return False
    sock.sendall(frame(reply))
    return True


def serve_connection(sock, server):
    """Serve one client until it goes away or a fault kills the connection."""
    with sock:
        while True:
            header = recv_exact(sock, 4)
            if header is None:
                return
            payload = recv_exact(sock, struct.unpack(">I", header)[0])
            if payload is None:
                return
            zone = server.attached
            reply = server.dispatch(payload)
            if not send_faulty(sock, server, reply, zone):
                return


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
    """The command line of ref9p_server.py, with our own fault list."""
    parser = argparse.ArgumentParser(description="faulty 9P2000.L server")
    parser.add_argument("--root", required=True, help="directory to serve")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--ready-fd", type=int, default=-1)
    parser.add_argument("--msize", type=int, default=DEFAULT_MSIZE)
    parser.add_argument("--fault", default="none", choices=("none",) + FAULTS)
    return parser.parse_args(argv)


def main(argv=None):
    """Start the faulty server and serve until it is killed."""
    args = parse_args(argv if argv is not None else sys.argv[1:])
    if not os.path.isdir(args.root):
        sys.stderr.write("faulty_server: no such directory: %s\n" % args.root)
        return 2
    server = FaultyServer(args.root, args.msize, args.fault)
    listener, _port = listen(args)
    serve(server, listener)
    return 0


if __name__ == "__main__":
    sys.exit(main())

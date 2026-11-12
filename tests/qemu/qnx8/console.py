#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive the serial console of a QEMU guest through a unix socket.

The guest's console is a QEMU ``-serial unix:...`` socket. This helper
connects to it, sends the commands given on the command line, records
everything the guest prints in a log file and exits.

Usage:
    console.py <socket> <logfile> <command> [command ...]

Each command is sent as a line, terminated by a newline; the helper waits
for the guest to go quiet before sending the next one.
"""

from __future__ import annotations

import socket
import sys
import time

QUIET = 1.5  # seconds of silence that mean "the guest answered"
TIMEOUT = 30.0  # seconds before a command is given up on


def connect(path: str) -> socket.socket:
    """Connect to the QEMU serial socket, waiting for QEMU to create it."""
    deadline = time.time() + 20.0
    while True:
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(path)
            return sock
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.2)


def read_until_quiet(sock: socket.socket, log: object, quiet: float) -> str:
    """Read from *sock* until it stays silent for *quiet* seconds."""
    sock.settimeout(0.4)
    text = ""
    last = time.time()
    while time.time() - last < quiet:
        try:
            data = sock.recv(4096)
        except socket.timeout:
            continue
        if not data:
            break
        chunk = data.decode("utf-8", "replace")
        text += chunk
        log.write(chunk)  # type: ignore[union-attr]
        log.flush()  # type: ignore[union-attr]
        last = time.time()
    return text


def main(argv: list[str]) -> int:
    """Run the helper: connect, drain the banner, run the commands."""
    if len(argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    sock = connect(argv[1])
    with open(argv[2], "w", encoding="utf-8") as log:
        read_until_quiet(sock, log, QUIET)
        for command in argv[3:]:
            sock.sendall((command + "\n").encode("utf-8"))
            read_until_quiet(sock, log, QUIET)
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

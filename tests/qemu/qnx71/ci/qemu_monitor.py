#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Ask the QEMU monitor of the QNX guest what the host side of the bus looks like.

The guest reports what QNX made of the device; the monitor reports what QEMU and
the firmware gave it. `info pci` is the one that matters here: it prints the IRQ
line and the interrupt pin assigned to our virtio-9p device ("IRQ 10, pin A"),
i.e. exactly the two values image/pci_hw.cfg records - so the CI job can tell
when the fragment has drifted from the machine that boots it.

The monitor connection is the one tests/qemu/qnx71/qnx_console.py already
implements (same socket, same handshake): it is imported, not copied.

Standard library only. Usage:

    python3 qemu_monitor.py "info pci" [--timeout 20] [--out FILE]

Exit status: 0 answered, 2 monitor unreachable (the caller warns, it does not
fail the job: the guest-side evidence stands on its own), 1 usage error.
"""
from __future__ import annotations

import argparse
import os
import re
import socket
import sys
import time
from pathlib import Path

EXIT_OK = 0
EXIT_USAGE = 1
EXIT_UNREACHABLE = 2

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import qnx_console  # noqa: E402  (reuse its monitor connection, not a copy)

# The HMP echoes every keystroke and redraws its prompt. Once the escape
# sequences are gone, what is left of the echo is one line made of the growing
# prefixes of the command ("iininfinfo... info pci"), always the first one, and
# prompt lines. Both are dropped; the answer starts on the next line. The echo
# of a command of n characters is at most 1+2+...+n = n(n+1)/2 characters long,
# which is the bound used to recognise it.
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[=>]|\r")
NOISE = ("", "(qemu)", "(qemu", "qemu)", "(q")


def echo_bound(command: str) -> int:
    """Longest line the keystroke echo of `command` can produce."""
    return len(command) * (len(command) + 1) // 2


def clean(raw: str, command: str) -> str:
    """Return the monitor answer without the terminal noise of the HMP."""
    text = ANSI.sub("", raw)
    echoed = command.strip()
    bound = echo_bound(echoed)
    kept = []
    for line in text.split("\n"):
        stripped = line.strip()
        if stripped in NOISE:
            continue
        if echoed and stripped.endswith(echoed) and len(stripped) <= bound:
            continue  # the keystroke echo of the command itself
        kept.append(line.rstrip())
    return "\n".join(kept).strip() + "\n"


def query(sock: socket.socket, command: str, settle: float) -> str:
    """Send one HMP command and return everything the monitor prints back."""
    sock.sendall((command + "\n").encode("ascii"))
    time.sleep(settle)
    chunks: list[bytes] = []
    sock.settimeout(2.0)
    while True:
        try:
            data = sock.recv(65536)
        except socket.timeout:
            break
        except OSError:
            break
        if not data:
            break
        chunks.append(data)
    return b"".join(chunks).decode("utf-8", errors="replace")


def main(argv: list[str] | None = None) -> int:
    """Run one monitor command and print (or save) its cleaned answer."""
    default_vm = os.path.expanduser(os.environ.get("QNX_VM_DIR", "~/qnx71-vm"))
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", nargs="?", default="info pci")
    parser.add_argument("--settle", type=float, default=1.5,
                        help="seconds given to the monitor to answer")
    parser.add_argument("--timeout", type=float, default=20.0,
                        help="ceiling of the whole exchange, seconds")
    parser.add_argument("--vm-dir", default=default_vm)
    parser.add_argument("--out", default="", help="also write the answer here")
    args = parser.parse_args(argv)

    # qnx_console computes its socket path at import time from QNX_VM_DIR; the
    # CI passes the directory explicitly, so the constant is set here.
    qnx_console.MON = str(Path(args.vm_dir).expanduser() / "mon.sock")
    deadline = time.monotonic() + args.timeout
    try:
        sock = qnx_console.monitor()
    except OSError as exc:
        print("qemu_monitor: cannot reach %s (%s)"
              % (qnx_console.MON, exc), file=sys.stderr)
        return EXIT_UNREACHABLE
    settle = min(args.settle, max(0.2, deadline - time.monotonic()))
    try:
        answer = clean(query(sock, args.command, settle), args.command)
    finally:
        sock.close()
    if not answer.strip():
        print("qemu_monitor: no answer to %r" % args.command, file=sys.stderr)
        return EXIT_UNREACHABLE
    sys.stdout.write(answer)
    if args.out:
        Path(args.out).write_text(answer, encoding="utf-8")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())

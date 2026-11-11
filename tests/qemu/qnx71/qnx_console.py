#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive the VGA console of the QNX guest through the QEMU HMP monitor.

The QNX 7.1 image has no serial console, so the only text channel is the VGA
framebuffer. This helper types a command with `sendkey`, waits, then dumps the
screen to a PNG that a human (or a vision model) can read.

Usage: qnx_console.py "command to type" [out.png] [wait_seconds]
"""
import socket
import sys
import time

import os

VM = os.path.expanduser(os.environ.get("QNX_VM_DIR", "~/qnx71-vm"))
MON = os.path.join(VM, "mon.sock")

# HMP sendkey names for the characters we need.
KEYS = {
    " ": "spc", "\n": "ret", "\t": "tab", "-": "minus", "_": "shift-minus",
    "/": "slash", ".": "dot", ":": "shift-dot", "|": "shift-backslash",
    ">": "shift-dot", "<": "shift-comma", ",": "comma", ";": "semicolon",
    "=": "equal", "+": "shift-equal", "*": "shift-8", "'": "apostrophe",
    '"': "shift-apostrophe", "(": "shift-9", ")": "shift-0", "[": "bracket_left",
    "]": "bracket_right", "$": "shift-4", "&": "shift-7", "#": "shift-3",
    "!": "shift-1", "?": "shift-slash", "@": "shift-2", "%": "shift-5",
    "^": "shift-6", "~": "shift-grave_accent", "`": "grave_accent",
    "\\": "backslash", "{": "shift-bracket_left", "}": "shift-bracket_right",
}


def key_for(ch):
    if ch in KEYS:
        return KEYS[ch]
    if ch.isupper():
        return "shift-" + ch.lower()
    if ch.isdigit():
        return ch
    return ch


def monitor():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(MON)
    s.settimeout(5)
    time.sleep(0.4)
    try:
        s.recv(65536)
    except socket.timeout:
        pass
    return s


def send(s, cmd):
    s.sendall((cmd + "\n").encode())
    time.sleep(0.3)
    try:
        s.recv(65536)
    except socket.timeout:
        pass


def type_text(s, text):
    for ch in text:
        send(s, "sendkey " + key_for(ch))
        time.sleep(0.02)


def main():
    text = sys.argv[1] if len(sys.argv) > 1 else ""
    out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/qnx-console.png"
    wait = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
    s = monitor()
    if text:
        type_text(s, text)
        send(s, "sendkey ret")
    time.sleep(wait)
    raw = os.path.join(os.path.dirname(out) or ".", ".qnx-raw.ppm")
    send(s, "screendump " + raw)
    time.sleep(1.5)
    s.close()
    from PIL import Image
    Image.open(raw).convert("RGB").save(out)
    os.unlink(raw)
    print("screen saved to", out)


if __name__ == "__main__":
    main()

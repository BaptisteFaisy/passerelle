#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Wait for the SSH banner of the QNX 7.1 guest, under a hard ceiling.

The community image brings up `io-pkt` and then `sshd` only ~40 s after the
kernel starts, and it has been seen on four different DHCP addresses, which is
why tests/qemu/qnx71/qnx-vm.sh forwards the ports 2222-2225: exactly one of them
answers with a banner. Waiting for that is what this script does, and it is the
CI replacement for the `sleep 50` of the manual procedure - a bounded poll
instead of a fixed sleep (rule "pas d'attente en dur > 30 s" of
~/Bureau/CI-BUDGET-5MIN.md), with a ceiling on the whole wait and an early exit
when QEMU itself is gone.

Standard library only. Usage:

    python3 wait_ssh.py --timeout 600 [--ports 2222-2225] [--port-file p.txt]

Exit status:
    0  a port answered with an SSH banner (the port is printed as SSH_PORT=<n>)
    1  the ceiling was reached without a banner
    3  the QEMU process of the guest is gone (no point in waiting any longer)
"""
from __future__ import annotations

import argparse
import os
import socket
import sys
import time
from pathlib import Path

EXIT_UP = 0
EXIT_TIMEOUT = 1
EXIT_GUEST_DEAD = 3

# One progress line every PROGRESS_EVERY rounds (~30 s at the default interval):
# enough to show the wait is alive in a CI log, not enough to flood it.
PROGRESS_EVERY = 6


class GuestDead(Exception):
    """The QEMU process of the guest disappeared while we were waiting."""


def parse_ports(spec: str) -> list[int]:
    """Turn '2222-2225' or '2222,2225' into a list of ports."""
    ports: list[int] = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            low, high = part.split("-", 1)
            ports.extend(range(int(low), int(high) + 1))
        else:
            ports.append(int(part))
    if not ports:
        raise ValueError("no port in %r" % spec)
    return ports


def read_banner(host: str, port: int, timeout: float) -> str:
    """Return what the port answers with, or '' when it does not answer."""
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            data = sock.recv(128)
    except OSError:
        return ""
    return data.decode("ascii", errors="replace").strip()


def qemu_running(vm_dir: Path) -> bool:
    """True while the QEMU process recorded by qnx-vm.sh is alive."""
    pid_file = vm_dir / "qemu.pid"
    try:
        pid = int(pid_file.read_text(encoding="ascii").strip())
    except (OSError, ValueError):
        return True  # no pid file: do not guess, keep waiting for the banner
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def probe_once(host: str, ports: list[int], timeout: float) -> tuple[int, str] | None:
    """Return (port, banner) of the first port answering with an SSH banner."""
    for port in ports:
        banner = read_banner(host, port, timeout)
        if banner.startswith("SSH-"):
            return port, banner
        if banner:
            print("wait_ssh: port %d answered %r (not SSH), trying the next one"
                  % (port, banner[:60]), flush=True)
    return None


def wait(host: str, ports: list[int], ceiling: float, interval: float,
         probe_timeout: float, vm_dir: Path) -> tuple[int, str] | None:
    """Poll the ports until one answers with a banner, or the ceiling expires.

    Raises GuestDead when QEMU disappears, which is worth knowing immediately:
    waiting the whole ceiling for a guest that will never answer is wasted CI
    time, and the caller then points at qemu.out instead of at the network.
    """
    started = time.monotonic()
    deadline = started + ceiling
    rounds = 0
    while True:
        rounds += 1
        found = probe_once(host, ports, probe_timeout)
        if found is not None:
            return found
        if rounds % PROGRESS_EVERY == 1:
            print("wait_ssh: no banner yet on %s (%ds elapsed, ceiling %ds)"
                  % (",".join(str(p) for p in ports),
                     int(time.monotonic() - started), int(ceiling)), flush=True)
        if time.monotonic() >= deadline:
            return None
        if not qemu_running(vm_dir):
            raise GuestDead(str(vm_dir))
        time.sleep(interval)


def main(argv: list[str] | None = None) -> int:
    """Parse the arguments, wait, and report the port that answered."""
    default_vm = os.path.expanduser(os.environ.get("QNX_VM_DIR", "~/qnx71-vm"))
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--ports", default="2222-2225")
    parser.add_argument("--timeout", type=float, default=600.0,
                        help="ceiling of the whole wait, seconds")
    parser.add_argument("--interval", type=float, default=5.0,
                        help="delay between two rounds, seconds (<= 30)")
    parser.add_argument("--probe-timeout", type=float, default=3.0)
    parser.add_argument("--vm-dir", default=default_vm)
    parser.add_argument("--port-file", default="",
                        help="write the answering port to this file")
    args = parser.parse_args(argv)

    if args.interval > 30:
        print("wait_ssh: --interval must stay <= 30 s (CI budget rule)",
              file=sys.stderr)
        return 2
    vm_dir = Path(args.vm_dir).expanduser()
    started = time.monotonic()
    try:
        found = wait(args.host, parse_ports(args.ports), args.timeout,
                     args.interval, args.probe_timeout, vm_dir)
    except GuestDead:
        print("wait_ssh: the QEMU process of %s is gone after %ds - read %s"
              % (vm_dir, int(time.monotonic() - started), vm_dir / "qemu.out"),
              file=sys.stderr)
        return EXIT_GUEST_DEAD
    if found is None:
        print("wait_ssh: no SSH banner after %ds (ceiling reached)"
              % int(time.monotonic() - started), file=sys.stderr)
        return EXIT_TIMEOUT
    port, banner = found
    waited = int(time.monotonic() - started)
    print("wait_ssh: %s:%d answered %r after %ds"
          % (args.host, port, banner, waited), flush=True)
    if args.port_file:
        Path(args.port_file).write_text("%d\n" % port, encoding="ascii")
    print("SSH_PORT=%d" % port)
    return EXIT_UP


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Command line entry point of the vhost-user front end.

Implements requirements:
  - VFS-06: ``python3 -m vhost --share DIR`` launches virtiofsd on DIR and runs
    the whole FUSE sequence of ``host/vhost/driver.py`` through it, then prints
    a pass/fail report - the virtiofs proof without QEMU, QNX or a guest.

Standard library only.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

from vhost.driver import format_report, run_scenario
from qshare.runner import find_virtiofsd


def main(argv: list[str] | None = None) -> int:
    """Run the virtiofsd scenario and report; 0 when every check passed."""
    parser = argparse.ArgumentParser(prog="python3 -m vhost", description=__doc__)
    parser.add_argument("--share", required=True, type=pathlib.Path, help="directory to share")
    parser.add_argument("--socket", default=None, help="vhost-user socket path (default: /tmp)")
    parser.add_argument("--timeout", type=float, default=10.0, help="per-operation timeout")
    args = parser.parse_args(argv)

    if find_virtiofsd() is None:
        print("virtiofsd not found (looked in /usr/libexec, /usr/lib/qemu, PATH)")
        return 2
    report = run_scenario(args.share, socket_path=args.socket, timeout=args.timeout)
    print(format_report(report))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())

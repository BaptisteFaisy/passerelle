#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""E5: does the share work with a real guest, using the options qshare emits?

Feasibility test E5 of the specification asks whether ``virtiofsd`` and the
shared guest memory work on this PC with a Linux guest. This harness answers it,
and answers the 9P half of the same question on the way, by booting a minimal
Linux guest on the **exact options qshare generates** - not on a hand-written
QEMU command line. If the emitted options were wrong, this test fails.

What it proves
--------------
* the QEMU option strings of ``qshare --emit`` are accepted by QEMU 8.2 for both
  protocols (9P via ``-fsdev local`` + ``-device virtio-9p-pci``, virtiofs via
  ``memory-backend-memfd`` + ``vhost-user-fs-pci``);
* ``qshare``'s own virtiofsd wrapper starts the daemon and the socket appears
  (VFS-06, host side);
* a guest kernel mounts the share with tag ``pgw.tag`` and can read a file the
  PC wrote before the boot, then write one that the PC sees afterwards.

What it does **not** prove: anything about the QNX driver. The guest here is
Linux, so the client is the Linux one; only the host side of Passerelle is under
test. E1/E3/E4 still need the QNX SDP.

Exit status: 0 = every marker PASS, 1 = a check failed, 2 = prerequisites
missing (explicit skip, never a silent green).
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
HOST_DIR = REPO_ROOT / "host"
HERE = pathlib.Path(__file__).resolve().parent
CONSOLE_TIMEOUT = 240.0


class Skipped(Exception):
    """A prerequisite is missing."""


def require(cmd: str, why: str) -> None:
    if shutil.which(cmd) is None:
        raise Skipped(f"{cmd} not found ({why})")


def find_kernel() -> pathlib.Path:
    """Newest host kernel that is readable and has a matching /lib/modules tree.

    Recent Ubuntu kernels are installed 0600 (root only); QEMU runs unprivileged
    here, so readability is part of the choice, not just the version.
    """
    for kernel in sorted(pathlib.Path("/boot").glob("vmlinuz-*"), reverse=True):
        if not os.access(kernel, os.R_OK):
            continue
        version = kernel.name[len("vmlinuz-"):]
        if pathlib.Path(f"/lib/modules/{version}").is_dir():
            return kernel
    raise Skipped("no readable host kernel with a matching /lib/modules tree")


def build_initramfs(work: pathlib.Path, cache: pathlib.Path,
                    kernel: pathlib.Path) -> pathlib.Path:
    image = work / "initramfs.cpio.gz"
    proc = subprocess.run(
        ["sh", str(HERE / "mkinitramfs.sh"), str(image),
         kernel.name[len("vmlinuz-"):], str(cache)],
        capture_output=True, text=True, check=False,
    )
    sys.stdout.write(proc.stdout)
    if proc.returncode != 0 or not image.exists():
        sys.stderr.write(proc.stderr)
        raise Skipped(f"mkinitramfs.sh failed (rc={proc.returncode})")
    return image


def qemu_command(share: pathlib.Path, proto: str, tag: str,
                 kernel: pathlib.Path, initrd: pathlib.Path,
                 work: pathlib.Path) -> tuple[list, object]:
    """Ask qshare for the command line, so the options tested are the emitted ones.

    ``build_qemu_cmd`` decides between ``-kernel`` and ``-drive`` from the image
    name, and resolves symlinks, so a host kernel cannot be smuggled in as the
    image. Instead the harness passes a throwaway disk image (never booted: with
    ``-kernel`` present QEMU uses the kernel) and appends the direct-boot
    arguments. Everything that matters here - the share options - still comes
    from qshare untouched.
    """
    sys.path.insert(0, str(HOST_DIR))
    from qshare.options import validate_share
    from qshare.runner import build_qemu_cmd

    cfg = validate_share(str(share), tag=tag, proto=proto)
    append = f"console=ttyS0 pgw.share={proto} pgw.tag={tag}"
    placeholder = work / "unused-disk.img"
    if not placeholder.exists():
        with open(placeholder, "wb") as fh:
            fh.truncate(1024 * 1024)
    cmd = build_qemu_cmd(
        cfg=cfg,
        image=placeholder,
        arch="x86_64",
        extra_args=("-kernel", str(kernel), "-initrd", str(initrd),
                    "-append", append, "-no-reboot"),
    )
    return cmd, cfg


def run_guest(cmd: list, cfg, console: pathlib.Path) -> str:
    sys.path.insert(0, str(HOST_DIR))
    from qshare.runner import QemuInstance, share_backend

    print("+ " + " ".join(str(c) for c in cmd))
    with share_backend(cfg):
        instance = QemuInstance(cmd, console_log=console)
        instance.start()
        try:
            if not instance.wait_for_console("PGW-E5 DONE", timeout=CONSOLE_TIMEOUT):
                print("   (no DONE marker; collecting what the console shows)")
        finally:
            instance.stop()
    return console.read_text(encoding="utf-8", errors="replace")


def check_proto(proto: str, work: pathlib.Path, cache: pathlib.Path,
                kernel: pathlib.Path, initrd: pathlib.Path) -> bool:
    tag = f"e5{proto}"[:31]
    share = work / f"share-{proto}"
    share.mkdir(parents=True, exist_ok=True)
    (share / "from-host.txt").write_text(f"hello-from-pc-{proto}\n")
    console = work / f"console-{proto}.log"

    cmd, cfg = qemu_command(share, proto, tag, kernel, initrd, work)
    started = time.monotonic()
    text = run_guest(cmd, cfg, console)
    elapsed = time.monotonic() - started

    passes = [l for l in text.splitlines() if "PGW-E5 PASS" in l]
    fails = [l for l in text.splitlines() if "PGW-E5 FAIL" in l]
    for line in text.splitlines():
        if "PGW-E5 INFO" in line:
            print("   guest:", line.split("PGW-E5 INFO", 1)[-1].strip())
    for line in passes:
        print("   PASS:", line.split("PGW-E5 PASS", 1)[-1].strip())
    for line in fails:
        print("   FAIL:", line.split("PGW-E5 FAIL", 1)[-1].strip())

    written = share / "from-guest.txt"
    host_ok = written.exists() and "written-from-guest" in written.read_text()
    print(f"   host side: {written.name} {'written by the guest' if host_ok else 'MISSING'}")
    ok = bool(passes) and not fails and host_ok and "RESULT PASS" in text
    print(f"[{proto}] {'PASS' if ok else 'FAIL'} in {elapsed:.1f}s "
          f"({len(passes)} pass, {len(fails)} fail) - console: {console}")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--protos", default="virtiofs,9p")
    parser.add_argument("--cache", default=os.path.expanduser("~/.cache/passerelle"))
    parser.add_argument("--workdir", default="", help="keep the artefacts here")
    args = parser.parse_args()

    try:
        require("qemu-system-x86_64", "the hypervisor")
        require("curl", "fetching the guest rootfs")
        require("cpio", "packing the initramfs")
        require("gzip", "packing the initramfs")
        require("unzstd", "decompressing the kernel modules")
        kernel = find_kernel()
        protos = [p.strip() for p in args.protos.split(",") if p.strip()]
        if "virtiofs" in protos:
            sys.path.insert(0, str(HOST_DIR))
            from qshare.runner import find_virtiofsd
            if find_virtiofsd() is None:
                raise Skipped("virtiofsd not found (qshare looks in /usr/lib/qemu, "
                              "/usr/libexec, PATH)")
    except Skipped as exc:
        print(f"::notice::E5 skipped, prerequisite missing: {exc}")
        return 2

    work = pathlib.Path(args.workdir) if args.workdir else pathlib.Path(
        tempfile.mkdtemp(prefix="pgw-e5-"))
    work.mkdir(parents=True, exist_ok=True)
    print(f"kernel: {kernel}")
    print(f"workdir: {work}")
    results: dict[str, bool] = {}
    try:
        initrd = build_initramfs(work, pathlib.Path(args.cache), kernel)
        for proto in protos:
            results[proto] = check_proto(proto, work, pathlib.Path(args.cache),
                                         kernel, initrd)
    finally:
        if not args.workdir:
            shutil.rmtree(work, ignore_errors=True)

    failed = [p for p, ok in results.items() if not ok]
    if failed:
        print(f"E5 FAIL: {', '.join(failed)}", file=sys.stderr)
        return 1
    print("E5 PASS: " + "; ".join(
        f"{p} share mounted and exchanged files with a Linux guest" for p in protos))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

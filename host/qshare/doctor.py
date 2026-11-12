# SPDX-License-Identifier: Apache-2.0
"""`qshare doctor`: what is missing on this machine (gap 92, DX-01).

One command that answers the question a newcomer asks first: *can this
machine run the demonstration?* It probes the hypervisor (QEMU >= 8.2),
the virtiofs daemon (PATH, then /usr/libexec and /usr/lib/qemu), the KVM
device, the guest tooling, the kernel the guest can boot, the free disk
space, the build chain (CMake and a C11 compiler), the Python version and
the QNX SDP - then prints one line per probe: the verdict and the concrete
fix to apply.

The exit status answers the question the table asks: 0 when the E5
demonstration can run here, 3 when something it needs is missing. A probe
that cannot answer never raises: a missing tool is a verdict, not a crash.

Nothing here re-reads an option by hand: the version comes from the one
shared lookup of `pgwutil.package_version`, the guest architecture from
the one configuration file of the four tools (`pgwutil.config`).
"""

from __future__ import annotations

import argparse
import os
import pathlib
import platform
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass

from pgwutil import dump_json, package_version, run_cmd, which
from pgwutil import config as pgwconfig
from pgwutil.hostenv import find_host_kernel
from qshare.runner import find_virtiofsd

TOOL = "qshare"
MIN_QEMU = (8, 2)
MIN_CMAKE = (3, 16)
MIN_PYTHON = (3, 10)
MIN_DISK_GIB = 1.0
TIGHT_DISK_GIB = 5.0
GUEST_TOOLS = ("curl", "cpio", "gzip", "unzstd")
COMPILERS = ("cc", "gcc", "clang")
KVM_DEVICE = "/dev/kvm"

OK = "ok"
WARN = "warn"
MISSING = "missing"
BLOCKS_E5 = "e5"
BLOCKS_BUILD = "build"
BLOCKS_NONE = "none"

DESCRIPTION = """Say what is missing on this machine before the first share.

qshare doctor probes QEMU, virtiofsd, /dev/kvm, the guest tooling, the
bootable kernel, the free disk space, CMake, a C11 compiler, Python and
the QNX SDP, then prints one line per probe: the verdict and the fix.

Exit status: 0 when the E5 demonstration can run here, 3 when something
it needs is missing.
"""

C11_PROGRAM = """#include <stddef.h>
_Static_assert(sizeof(int) >= 4, "C11 static assertions are required");
int main(void) { return 0; }
"""


@dataclass(frozen=True)
class Probe:
    """One probe: its verdict, what was found and the fix to apply."""

    name: str
    verdict: str
    detail: str
    fix: str = ""
    blocks: str = BLOCKS_NONE
    version: str = ""
    path: str = ""

    def as_dict(self) -> dict:
        """Machine-readable view of the probe."""
        return {
            "name": self.name,
            "verdict": self.verdict,
            "detail": self.detail,
            "fix": self.fix,
            "blocks": self.blocks,
            "version": self.version,
            "path": self.path,
        }


def _version_of(cmd: list[str]) -> str:
    """Version a tool reports, empty when it cannot be asked."""
    try:
        proc = run_cmd(cmd, timeout=15)
    except (OSError, subprocess.SubprocessError):
        return ""
    text = f"{proc.stdout or ''} {proc.stderr or ''}"
    match = re.search(r"(\d+\.\d+(?:\.\d+)?)", text)
    return match.group(1) if match else ""


def _at_least(version: str, minimum: tuple[int, ...]) -> bool:
    """True when *version* is greater than or equal to *minimum*."""
    parts = [int(part) for part in version.split(".") if part.isdigit()]
    if not parts:
        return False
    width = max(len(parts), len(minimum))
    parts += [0] * (width - len(parts))
    return tuple(parts[:width]) >= minimum


def _in_path(binary: pathlib.Path) -> bool:
    """True when *binary* is reachable through PATH."""
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        if not directory:
            continue
        candidate = pathlib.Path(directory) / binary.name
        try:
            if candidate.is_file() and candidate.samefile(binary):
                return True
        except OSError:
            continue
    return False


def probe_qemu(arch: str) -> Probe:
    """The hypervisor, and the version it must reach (QEMU >= 8.2)."""
    binary = which(f"qemu-system-{arch}")
    if binary is None:
        return Probe(
            "qemu", MISSING, f"qemu-system-{arch} not found",
            f"install QEMU >= 8.2 (apt install qemu-system-{arch})", BLOCKS_E5,
        )
    version = _version_of([str(binary), "--version"])
    if version and not _at_least(version, MIN_QEMU):
        return Probe(
            "qemu", MISSING, f"QEMU {version} at {binary} is older than 8.2",
            "install QEMU >= 8.2 (the E5 bench needs the vhost-user-fs device)",
            BLOCKS_E5, version, str(binary),
        )
    return Probe(
        "qemu", OK, f"QEMU {version or 'unknown version'} at {binary}",
        blocks=BLOCKS_E5, version=version, path=str(binary),
    )


def probe_virtiofsd() -> Probe:
    """The virtiofs daemon: PATH first, then the distribution directories."""
    binary = find_virtiofsd()
    if binary is None:
        return Probe(
            "virtiofsd", MISSING,
            "virtiofsd not found in PATH, /usr/libexec or /usr/lib/qemu",
            "install it (apt install qemu-virtiofsd) or put it in PATH",
            BLOCKS_E5,
        )
    version = _version_of([str(binary), "--version"])
    where = "in PATH" if _in_path(binary) else "not in PATH"
    return Probe(
        "virtiofsd", OK,
        f"virtiofsd {version or 'unknown version'} at {binary} ({where})",
        blocks=BLOCKS_E5, version=version, path=str(binary),
    )


def probe_kvm() -> Probe:
    """The KVM device: without it QEMU emulates, which is slow but works."""
    device = pathlib.Path(KVM_DEVICE)
    if not device.exists():
        return Probe(
            "kvm", WARN, "/dev/kvm is absent: QEMU will emulate (TCG), the guest is slow",
            "load the module (sudo modprobe kvm) or accept the slow path",
        )
    if not os.access(device, os.R_OK | os.W_OK):
        return Probe(
            "kvm", WARN, f"{device} is not usable by this user: QEMU will emulate (TCG)",
            "add this user to the 'kvm' group and log in again",
        )
    return Probe("kvm", OK, f"{device} is usable by this user")


def probe_guest_tools() -> Probe:
    """The tools the guest harness needs to build its initramfs."""
    missing = [tool for tool in GUEST_TOOLS if which(tool) is None]
    if missing:
        return Probe(
            "guest-tools", MISSING, f"not in PATH: {', '.join(missing)}",
            f"install them (apt install {' '.join(missing)})", BLOCKS_E5,
        )
    return Probe("guest-tools", OK, f"{', '.join(GUEST_TOOLS)} are in PATH", blocks=BLOCKS_E5)


def probe_kernel() -> Probe:
    """The host kernel a guest can boot with its filesystem modules."""
    kernel = find_host_kernel()
    if kernel is None:
        return Probe(
            "kernel", MISSING,
            "no readable host kernel with a matching /lib/modules tree",
            "install a kernel with its modules (sudo apt install linux-image-generic)",
            BLOCKS_E5,
        )
    return Probe(
        "kernel", OK, f"host kernel {kernel.name} is readable with its modules tree",
        blocks=BLOCKS_E5, path=str(kernel),
    )


def probe_disk() -> Probe:
    """The free space the build trees and the guest image need."""
    target = pathlib.Path.cwd()
    try:
        stat = os.statvfs(target)
    except OSError as err:
        return Probe("disk", WARN, f"cannot read the free space of {target}: {err}",
                     blocks=BLOCKS_E5)
    free = stat.f_bavail * stat.f_frsize / (1024 ** 3)
    if free < MIN_DISK_GIB:
        return Probe(
            "disk", MISSING, f"only {free:.1f} GiB free on {target}",
            f"free at least {MIN_DISK_GIB:.0f} GiB (the guest image and the rootfs need room)",
            BLOCKS_E5,
        )
    if free < TIGHT_DISK_GIB:
        return Probe(
            "disk", WARN, f"only {free:.1f} GiB free on {target}",
            f"free more space: under {TIGHT_DISK_GIB:.0f} GiB the build trees get tight",
            blocks=BLOCKS_E5,
        )
    return Probe("disk", OK, f"{free:.1f} GiB free on {target}", blocks=BLOCKS_E5)


def probe_cmake() -> Probe:
    """CMake, which drives every build of the tree."""
    binary = which("cmake")
    if binary is None:
        return Probe(
            "cmake", MISSING, "cmake not found",
            "install CMake >= 3.16 (apt install cmake)", BLOCKS_BUILD,
        )
    version = _version_of([str(binary), "--version"])
    if version and not _at_least(version, MIN_CMAKE):
        return Probe(
            "cmake", MISSING, f"CMake {version} at {binary} is older than 3.16",
            "install CMake >= 3.16", BLOCKS_BUILD, version, str(binary),
        )
    return Probe(
        "cmake", OK, f"CMake {version or 'unknown version'} at {binary}",
        blocks=BLOCKS_BUILD, version=version, path=str(binary),
    )


def _compiles_c11(binary: pathlib.Path) -> tuple[bool, str]:
    """Compile a tiny C11 program; never raises."""
    with tempfile.TemporaryDirectory(prefix="pgw-doctor-") as work:
        source = pathlib.Path(work) / "c11.c"
        source.write_text(C11_PROGRAM, encoding="utf-8")
        output = pathlib.Path(work) / "c11"
        try:
            proc = run_cmd(
                [str(binary), "-std=c11", str(source), "-o", str(output)], timeout=120
            )
        except (OSError, subprocess.SubprocessError) as err:
            return False, str(err)
    if proc.returncode != 0:
        text = (proc.stderr or proc.stdout or "compilation failed").strip()
        return False, text.splitlines()[-1][:160] if text else "compilation failed"
    return True, ""


def probe_compiler() -> Probe:
    """A C compiler, checked on a real C11 program."""
    binary = None
    for name in COMPILERS:
        binary = which(name)
        if binary is not None:
            break
    if binary is None:
        return Probe(
            "compiler", MISSING, "no C compiler found (cc, gcc, clang)",
            "install one (apt install build-essential)", BLOCKS_BUILD,
        )
    version = _version_of([str(binary), "--version"])
    compiles, reason = _compiles_c11(binary)
    if not compiles:
        return Probe(
            "compiler", WARN, f"{binary.name} does not compile a C11 program: {reason}",
            "install a C11 compiler (gcc >= 4.9 or clang >= 3.1)",
            BLOCKS_BUILD, version, str(binary),
        )
    return Probe(
        "compiler", OK, f"{binary.name} {version or 'unknown version'} compiles C11",
        blocks=BLOCKS_BUILD, version=version, path=str(binary),
    )


def probe_python() -> Probe:
    """The Python the tools run on."""
    version = platform.python_version()
    if not _at_least(version, MIN_PYTHON):
        return Probe(
            "python", MISSING, f"Python {version} is older than 3.10",
            "install Python >= 3.10", BLOCKS_BUILD, version, sys.executable,
        )
    return Probe(
        "python", OK, f"Python {version} at {sys.executable}",
        blocks=BLOCKS_BUILD, version=version, path=sys.executable,
    )


def probe_sdp() -> Probe:
    """The QNX SDP: needed to build the driver, not to run the E5 bench."""
    compiler = which("qcc")
    if compiler is not None:
        return Probe("sdp", OK, f"the QNX SDP compiler is at {compiler}", path=str(compiler))
    target = os.environ.get("QNX_TARGET", "").strip()
    if target:
        return Probe("sdp", OK, f"QNX_TARGET={target} is set")
    return Probe(
        "sdp", WARN, "no QNX SDP found (no qcc, no QNX_TARGET)",
        "needed only to build the QNX driver, not for the E5 bench: install the SDP 8.0",
    )


def probe_rootfs_cache() -> Probe:
    """The guest rootfs cache: the first guest run downloads it."""
    cache = pathlib.Path.home() / ".cache" / "passerelle"
    if (cache / "alpine-minirootfs.tar.gz").is_file():
        return Probe("guest-rootfs", OK, f"the guest rootfs is cached in {cache}")
    return Probe(
        "guest-rootfs", WARN, f"no guest rootfs cached in {cache}: the first guest run fetches it",
        "a network access is needed once (or copy an alpine-minirootfs tarball there)",
    )


def collect(arch: str) -> list[Probe]:
    """Every probe, in the order the report prints them."""
    return [
        probe_qemu(arch),
        probe_virtiofsd(),
        probe_kvm(),
        probe_guest_tools(),
        probe_kernel(),
        probe_disk(),
        probe_cmake(),
        probe_compiler(),
        probe_python(),
        probe_sdp(),
        probe_rootfs_cache(),
    ]


def _summary(probes: list[Probe]) -> str:
    """One sentence: what this machine can do, and what it cannot."""
    blocking = [probe for probe in probes
                if probe.blocks == BLOCKS_E5 and probe.verdict == MISSING]
    building = [probe for probe in probes
                if probe.blocks == BLOCKS_BUILD and probe.verdict == MISSING]
    warnings = [probe.name for probe in probes if probe.verdict == WARN]
    if blocking:
        reasons = "; ".join(f"{probe.name}: {probe.fix or probe.detail}" for probe in blocking)
        return f"the E5 demonstration cannot run here - {reasons}"
    chain = "the portable build chain is ready"
    if building:
        chain = "the portable build chain is missing " + ", ".join(
            probe.name for probe in building
        )
    note = f" ({len(warnings)} warning(s): {', '.join(warnings)})" if warnings else ""
    return f"the E5 demonstration can run here{note}; {chain}"


def build_report(probes: list[Probe]) -> dict:
    """The report, as data: one entry per probe plus the verdicts."""
    return {
        "command": "qshare doctor",
        "version": package_version(),
        "host": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
        },
        "e5_ready": not any(
            probe.blocks == BLOCKS_E5 and probe.verdict == MISSING for probe in probes
        ),
        "build_ready": not any(
            probe.blocks == BLOCKS_BUILD and probe.verdict == MISSING for probe in probes
        ),
        "probes": [probe.as_dict() for probe in probes],
        "missing": [probe.name for probe in probes if probe.verdict == MISSING],
        "summary": _summary(probes),
    }


def render(report: dict) -> str:
    """The human-readable report: one line per probe, then the summary."""
    lines = [
        f"qshare doctor - {report['host']['system']} {report['host']['machine']}, "
        f"host tools {report['version']}",
        "",
    ]
    lines += [_probe_line(probe) for probe in report["probes"]]
    lines += ["", report["summary"]]
    return "\n".join(lines)


def _probe_line(probe: dict) -> str:
    """One line of the table: the verdict, the detail and the fix."""
    text = f"{probe['name']:<12} {probe['verdict'].upper():<8} {probe['detail']}"
    return f"{text} -> fix: {probe['fix']}" if probe["fix"] else text


def register(parser: argparse.ArgumentParser) -> None:
    """Register the options of `qshare doctor`."""
    parser.add_argument(
        "--arch", default=None, help=pgwconfig.help_text("arch", "guest architecture to probe")
    )
    parser.add_argument(
        "--json", action="store_true", help="print the report as JSON (for machines)"
    )
    pgwconfig.add_show_config(parser)


def run(args: argparse.Namespace) -> int:
    """Probe the machine, print the report and return the exit status."""
    config = pgwconfig.apply(args, pgwconfig.load(TOOL))
    if getattr(args, "show_config", None):
        print(pgwconfig.render(config, args.show_config))
        return 0
    arch = config.get("arch", "x86_64")
    report = build_report(collect(arch))
    if getattr(args, "json", False):
        print(dump_json(report))
    else:
        print(render(report))
    return 0 if report["e5_ready"] else 3

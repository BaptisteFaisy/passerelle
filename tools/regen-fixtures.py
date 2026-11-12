#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regenerate the 9P trace fixtures of host/tests/fixtures/ (gap 55).

The fixtures were written by hand once and were never regenerated, so a
change of the protocol library or of the trace format could keep passing on
data that no longer describes a real run. This tool produces them from a real
run instead:

  * the production client of proto9p/ talks to the independent reference
    server of tests/ref9p/ over a socket (the harness of tests/ref9p/ is
    reused as it is);
  * every frame is captured and rendered in the QEMU trace format, the one
    the parser of host/bench/trace.py reads;
  * the run is deterministic: the tree has fixed names and fixed bytes, the
    clock is the injectable monotonic clock of common/ (pgw/time.h) and only
    the protocol facts that do not depend on the machine are rendered.

Usage:
    tools/regen-fixtures.py --write     # rewrite the fixtures
    tools/regen-fixtures.py --check     # fail when they drifted (exit 1)
    tools/regen-fixtures.py --verify    # two captures, byte for byte

Exit status: 0 when everything is fine, 1 when a fixture drifted, 2 when the
capture could not run at all (no build tool, no compiler, ...).

Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURE_DIR = ROOT / "host" / "tests" / "fixtures"

# The fixtures this tool owns. Every other file of the directory is captured
# by hand from a real system and cannot be reproduced by a script.
REGENERABLE = ("qemu-9p-trace.log",)

# The fixtures that are deliberately left out, and why. Reproducing them would
# mean faking determinism, which is worse than saying so.
NOT_REGENERABLE = {
    "qemu-9p-real-trace.log": (
        "a real QEMU 8.2 trace, captured from a Linux guest on this host: it "
        "carries the wall-clock timestamps, the pids and the inode numbers of "
        "the machine that produced it, and reproducing it would need the same "
        "QEMU, the same guest image and the same moment. It is kept as the "
        "ground truth of the trace *format*, not as a regenerated artefact."
    ),
}


def capture_binary(build_dir: pathlib.Path) -> pathlib.Path | None:
    """Path of the capture program, building it when it is not there yet."""
    env = os.environ.get("PGW_TRACECAP_BIN")
    if env:
        candidate = pathlib.Path(env)
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
        return None
    built = build_dir / "tests" / "tracecap" / "tracecap"
    if built.is_file() and os.access(built, os.X_OK):
        return built
    if shutil.which("cmake") is None:
        return None
    configure = subprocess.run(
        ["cmake", "-B", str(build_dir), str(ROOT)],
        capture_output=True,
        text=True,
        check=False,
    )
    if configure.returncode != 0:
        sys.stderr.write(configure.stdout + configure.stderr)
        return None
    build = subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", "tracecap", "-j4"],
        capture_output=True,
        text=True,
        check=False,
    )
    if build.returncode != 0:
        sys.stderr.write(build.stdout + build.stderr)
        return None
    return built if built.is_file() else None


def render(binary: pathlib.Path, out: pathlib.Path) -> int:
    """Render the capture into `out`. Returns the exit status of the tool."""
    result = subprocess.run(
        [str(binary), "--out", str(out)], capture_output=True, text=True, check=False
    )
    if result.stderr:
        sys.stderr.write(result.stderr)
    return result.returncode


def first_difference(new: bytes, old: bytes) -> int:
    """Index of the first differing byte, or -1 when the two are equal."""
    limit = min(len(new), len(old))
    for i in range(limit):
        if new[i] != old[i]:
            return i
    return -1 if len(new) == len(old) else limit


def describe(data: bytes, index: int) -> str:
    """The line of `data` that holds the byte at `index`."""
    start = data.rfind(b"\n", 0, index) + 1
    end = data.find(b"\n", index)
    if end < 0:
        end = len(data)
    return data[start:end].decode("utf-8", "replace")


def process(binary: pathlib.Path, write: bool) -> int:
    """Regenerate every regenerable fixture. Returns the exit status."""
    status = 0
    with tempfile.TemporaryDirectory(prefix="pgw-fixtures-") as tmp:
        rendered = pathlib.Path(tmp) / "trace.log"
        for name in REGENERABLE:
            target = FIXTURE_DIR / name
            if render(binary, rendered) != 0:
                return 2
            new = rendered.read_bytes()
            old = target.read_bytes() if target.is_file() else b""
            if new == old:
                print(f"fixtures: {name} is up to date")
                continue
            if write:
                target.write_bytes(new)
                print(f"fixtures: {name} rewritten ({len(new)} bytes)")
                continue
            index = first_difference(new, old)
            sys.stderr.write(f"fixtures: {name} drifted\n")
            sys.stderr.write(f"  committed:   {describe(old, index)}\n")
            sys.stderr.write(f"  regenerated: {describe(new, index)}\n")
            sys.stderr.write("  run: tools/regen-fixtures.py --write\n")
            status = 1
    return status


def verify(binary: pathlib.Path) -> int:
    """Capture twice and compare the two renderings byte for byte."""
    result = subprocess.run(
        [str(binary), "--verify-deterministic"], capture_output=True, text=True,
        check=False,
    )
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    return result.returncode


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--write", action="store_true",
                       help="rewrite the fixtures (the default)")
    group.add_argument("--check", action="store_true",
                       help="fail when a fixture no longer matches the code")
    group.add_argument("--verify", action="store_true",
                       help="capture twice and compare the two renderings")
    parser.add_argument("--build-dir", default=os.environ.get("BUILD_DIR", "build"),
                        help="the build tree that holds the capture program")
    args = parser.parse_args(argv)

    build_dir = pathlib.Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = ROOT / build_dir
    binary = capture_binary(build_dir)
    if binary is None:
        sys.stderr.write(
            "fixtures: the capture program could not be built; no fixture was "
            "checked. Install cmake and a C compiler, or set PGW_TRACECAP_BIN.\n"
        )
        return 2
    if args.verify:
        return verify(binary)
    return process(binary, write=not args.check)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

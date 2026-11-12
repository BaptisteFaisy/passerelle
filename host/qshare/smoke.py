# SPDX-License-Identifier: Apache-2.0
"""`qshare smoke`: prove the whole chain works on this machine (gap 94, DX-01).

One command that runs everything the project claims, in order: configure
the build tree, build it, run the CTest suite, run the pytest suite, run
the 9P and FUSE reference benches, and finally boot a guest on the exact
options `qshare` emits (the E5 bench).

Two rules make the report honest:

* a step that cannot run prints SKIPPED with the reason, never PASS - a
  missing CMake, a missing kernel or a machine without KVM is a fact, not
  a success;
* the build tree is reused, never rebuilt from scratch: an existing CMake
  cache is configured again (which is cheap) and only what changed is
  compiled. The report says what was reused.

`--fast` skips the slow parts (the guest boots a virtual machine), and
`--dry-run` prints the commands without running them - a dry run is
never reported as a pass.
"""

from __future__ import annotations

import argparse
import datetime
import os
import pathlib
import re
import subprocess
from dataclasses import dataclass, field

from pgwutil import dump_json, package_version, run_cmd, which
from pgwutil import config as pgwconfig

TOOL = "qshare"
REPO = pathlib.Path(__file__).resolve().parents[2]
BUILD_TIMEOUT = 900.0
GUEST_TIMEOUT = 900.0
PYTHON = "python3"
REFERENCE_BENCH = "ref9p|fuse_ref"

PASS = "pass"
FAIL = "fail"
SKIPPED = "skipped"
DRY = "dry"

DESCRIPTION = """Prove the whole chain works on this machine.

qshare smoke configures and builds the tree, runs ctest and pytest, runs
the 9P and FUSE reference benches, and boots a guest on the options
qshare emits. Anything that cannot run prints SKIPPED with its reason.

Exit status: 0 when every step that could run passed, 1 when one failed,
3 when nothing could run at all.
"""


@dataclass
class Step:
    """One step of the smoke: its verdict, what it says and the command."""

    name: str
    verdict: str
    detail: str
    command: list[str] = field(default_factory=list)

    def as_dict(self) -> dict:
        """Machine-readable view of the step."""
        return {
            "name": self.name,
            "verdict": self.verdict,
            "detail": self.detail,
            "command": list(self.command),
        }


@dataclass
class Context:
    """What the steps need to know: where, how much, and which mode."""

    repo: pathlib.Path
    build_dir: pathlib.Path
    jobs: int
    timeout: float
    fast: bool = False
    dry_run: bool = False


def _tail(proc: subprocess.CompletedProcess[str], lines: int = 4) -> str:
    """The last meaningful lines of a command output."""
    text = (proc.stdout or "") + (proc.stderr or "")
    rows = [row for row in text.splitlines() if row.strip()]
    return " | ".join(rows[-lines:])[:300]


def _cache_note(build_dir: pathlib.Path) -> str:
    """What the existing build tree is, when there is one."""
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return ""
    stamp = datetime.datetime.fromtimestamp(cache.stat().st_mtime).strftime("%Y-%m-%d %H:%M")
    return f"reused the existing build tree {build_dir} (CMake cache from {stamp})"


def _counts(output: str, pattern: str) -> str:
    """The 'N tests, M failed' summary of a test run."""
    match = re.search(pattern, output)
    return match.group(0) if match else ""


def _step_configure(ctx: Context) -> Step:
    """Configure the build tree, reusing an existing one."""
    cmake = which("cmake")
    if cmake is None:
        return Step("configure", SKIPPED, "cmake not found (install CMake >= 3.16)", ["cmake"])
    cmd = [str(cmake), "-B", str(ctx.build_dir), "-S", str(ctx.repo)]
    if ctx.dry_run:
        return Step("configure", DRY, " ".join(cmd), cmd)
    reused = _cache_note(ctx.build_dir)
    proc = run_cmd(cmd, cwd=ctx.repo, timeout=BUILD_TIMEOUT)
    if proc.returncode != 0:
        return Step("configure", FAIL, _tail(proc), cmd)
    return Step("configure", PASS, reused or "configured a new build tree", cmd)


def _step_build(ctx: Context) -> Step:
    """Build the tree: only what changed is compiled."""
    cmake = which("cmake")
    if cmake is None:
        return Step("build", SKIPPED, "cmake not found (install CMake >= 3.16)", ["cmake"])
    cmd = [str(cmake), "--build", str(ctx.build_dir), "-j", str(ctx.jobs)]
    if ctx.dry_run:
        return Step("build", DRY, " ".join(cmd), cmd)
    if not ctx.build_dir.is_dir():
        return Step("build", SKIPPED, "no build tree to build (the configure step did not run)")
    proc = run_cmd(cmd, cwd=ctx.repo, timeout=BUILD_TIMEOUT)
    if proc.returncode != 0:
        return Step("build", FAIL, _tail(proc), cmd)
    return Step("build", PASS, "built the tree", cmd)


def _step_ctest(ctx: Context) -> Step:
    """Run the CTest suite of the tree."""
    ctest = which("ctest")
    if ctest is None:
        return Step("ctest", SKIPPED, "ctest not found (it comes with CMake)", ["ctest"])
    cmd = [str(ctest), "--test-dir", str(ctx.build_dir), "--output-on-failure",
           "-j", str(ctx.jobs)]
    if ctx.dry_run:
        return Step("ctest", DRY, " ".join(cmd), cmd)
    if not ctx.build_dir.is_dir():
        return Step("ctest", SKIPPED, "no build tree to test (the configure step did not run)")
    proc = run_cmd(cmd, cwd=ctx.repo, timeout=BUILD_TIMEOUT)
    output = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0:
        return Step("ctest", FAIL, _tail(proc), cmd)
    summary = _counts(output, r"\d+% tests passed[^|]*")
    return Step("ctest", PASS, summary or "every CTest test passed", cmd)


def _has_pytest(python: pathlib.Path) -> bool:
    """True when pytest is installed for that interpreter."""
    try:
        proc = run_cmd([str(python), "-c", "import pytest"], timeout=60)
    except (OSError, subprocess.SubprocessError):
        return False
    return proc.returncode == 0


def _step_pytest(ctx: Context) -> Step:
    """Run the pytest suite of the host tools."""
    python = which(PYTHON)
    if python is None:
        return Step("pytest", SKIPPED, "python3 not found (the host tools are Python)",
                    [PYTHON])
    cmd = [str(python), "-m", "pytest", "host/tests", "-q"]
    if ctx.dry_run:
        return Step("pytest", DRY, " ".join(cmd), cmd)
    if not _has_pytest(python):
        return Step("pytest", SKIPPED,
                    "pytest is not installed for python3 (python3 -m pip install pytest)", cmd)
    proc = run_cmd(cmd, cwd=ctx.repo, timeout=BUILD_TIMEOUT)
    output = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0:
        return Step("pytest", FAIL, _tail(proc), cmd)
    summary = _counts(output, r"\d+ passed[^\n]*")
    return Step("pytest", PASS, summary or "every pytest test passed", cmd)


def _step_reference_bench(ctx: Context) -> Step:
    """Run the 9P and FUSE reference benches on their own."""
    ctest = which("ctest")
    if ctest is None:
        return Step("bench-ref", SKIPPED, "ctest not found (it comes with CMake)", ["ctest"])
    cmd = [str(ctest), "--test-dir", str(ctx.build_dir), "-R", REFERENCE_BENCH,
           "--output-on-failure"]
    if ctx.dry_run:
        return Step("bench-ref", DRY, " ".join(cmd), cmd)
    if not ctx.build_dir.is_dir():
        return Step("bench-ref", SKIPPED, "no build tree to test")
    proc = run_cmd(cmd, cwd=ctx.repo, timeout=BUILD_TIMEOUT)
    output = (proc.stdout or "") + (proc.stderr or "")
    if "No tests were found" in output:
        return Step("bench-ref", SKIPPED,
                    "the build tree registers no 9P/FUSE reference bench", cmd)
    if proc.returncode != 0:
        return Step("bench-ref", FAIL, _tail(proc), cmd)
    summary = _counts(output, r"\d+% tests passed[^|]*")
    return Step("bench-ref", PASS, summary or "the 9P and FUSE reference benches passed", cmd)


def _step_guest(ctx: Context) -> Step:
    """Boot a guest on the options qshare emits (the E5 bench)."""
    if ctx.fast:
        return Step("guest", SKIPPED, "--fast: the guest check boots a virtual machine")
    python = which(PYTHON)
    if python is None:
        return Step("guest", SKIPPED, "python3 not found (the host tools are Python)", [PYTHON])
    harness = ctx.repo / "tests" / "qemu" / "run_share_check.py"
    if not harness.is_file():
        return Step("guest", SKIPPED, "the E5 harness is not in this tree")
    cmd = [str(python), str(harness)]
    if ctx.dry_run:
        return Step("guest", DRY, " ".join(cmd), cmd)
    proc = run_cmd(cmd, cwd=ctx.repo, timeout=ctx.timeout)
    output = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode == 0:
        return Step("guest", PASS, "the guest mounted the share and wrote back to it", cmd)
    if proc.returncode == 2:
        return Step("guest", SKIPPED, _tail(proc, 2) or "a prerequisite is missing", cmd)
    return Step("guest", FAIL, _tail(proc), cmd)


def _steps(ctx: Context) -> list[Step]:
    """Every step of the smoke, in order."""
    return [
        _step_configure(ctx),
        _step_build(ctx),
        _step_ctest(ctx),
        _step_pytest(ctx),
        _step_reference_bench(ctx),
        _step_guest(ctx),
    ]


def _summary(steps: list[Step]) -> str:
    """How many steps passed, failed and were skipped."""
    passed = sum(1 for step in steps if step.verdict == PASS)
    failed = sum(1 for step in steps if step.verdict == FAIL)
    skipped = sum(1 for step in steps if step.verdict == SKIPPED)
    dry = sum(1 for step in steps if step.verdict == DRY)
    text = f"{passed} passed, {failed} failed, {skipped} skipped"
    return f"{text}, {dry} dry" if dry else text


def register(parser: argparse.ArgumentParser) -> None:
    """Register the options of `qshare smoke`."""
    parser.add_argument(
        "--build-dir", default="build", help="build tree to reuse (default: build)"
    )
    parser.add_argument(
        "--jobs", "-j", type=int, default=0, help="parallel jobs (default: the CPU count)"
    )
    parser.add_argument(
        "--fast", action="store_true", help="skip the slow parts (the guest check)"
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="print the commands without running them"
    )
    parser.add_argument(
        "--json", action="store_true", help="print the report as JSON (for machines)"
    )
    parser.add_argument(
        "--timeout", type=float, default=None,
        help=(
            "seconds to wait for the guest check (default: at least "
            f"{GUEST_TIMEOUT:.0f}, or the configured timeout when it is longer)"
        ),
    )
    pgwconfig.add_show_config(parser)


def _guest_timeout(args: argparse.Namespace, config) -> float:
    """How long the guest check may take: the option, else the configuration."""
    if args.timeout is not None:
        return float(args.timeout)
    return max(GUEST_TIMEOUT, float(config.typed("timeout")))


def run(args: argparse.Namespace) -> int:
    """Run every step and report what happened."""
    config = pgwconfig.apply(args, pgwconfig.load(TOOL))
    if getattr(args, "show_config", None):
        print(pgwconfig.render(config, args.show_config))
        return 0
    ctx = Context(
        repo=REPO,
        build_dir=pathlib.Path(args.build_dir).expanduser().resolve(),
        jobs=args.jobs or (os.cpu_count() or 1),
        timeout=_guest_timeout(args, config),
        fast=bool(args.fast),
        dry_run=bool(args.dry_run),
    )
    if not (ctx.repo / "CMakeLists.txt").is_file():
        print("qshare smoke: SKIPPED - the source tree is not here "
              "(the host tools are installed as a package); run it from a checkout")
        return 3
    steps = _steps(ctx)
    report = {
        "command": "qshare smoke",
        "version": package_version(),
        "build_dir": str(ctx.build_dir),
        "fast": ctx.fast,
        "dry_run": ctx.dry_run,
        "steps": [step.as_dict() for step in steps],
        "summary": _summary(steps),
    }
    if getattr(args, "json", False):
        print(dump_json(report))
    else:
        print(_render(report))
    if any(step.verdict == FAIL for step in steps):
        return 1
    if all(step.verdict == SKIPPED for step in steps):
        return 3
    return 0


def _render(report: dict) -> str:
    """The human-readable report: one line per step, then the summary."""
    lines = [
        f"qshare smoke - host tools {report['version']}, build tree {report['build_dir']}",
        "",
    ]
    lines += [_step_line(step) for step in report["steps"]]
    lines += ["", report["summary"]]
    return "\n".join(lines)


def _step_line(step: dict) -> str:
    """One line of the report: the verdict, the step and what it says."""
    return f"{step['verdict'].upper():<8} {step['name']:<12} {step['detail']}"

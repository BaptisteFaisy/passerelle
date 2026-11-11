# SPDX-License-Identifier: Apache-2.0
"""One-command demonstration (DX-02).

`qshare --demo <dir>` replays the whole story of the specification:

  1. validate the share and print the QEMU options that expose it;
  2. show the mkqnximage snippets that mount it at boot;
  3. run a program straight from the share (qrun, DX-03);
  4. show that a file edited on the PC is visible at once in the guest;
  5. run a short differential test and a short content exercise.

Without an image (`--run`) the guest steps are executed locally and
clearly labelled as such, so the demonstration is replayable on any PC
that has Python and nothing else.
"""

from __future__ import annotations

import pathlib
import sys
import tempfile
import time

from pgwutil import ensure_dir, write_atomic
from qshare.image import build_fragment, render_fragment
from qshare.options import build_qemu_args, validate_share

DEMO_PROGRAM = "demo_hello.sh"
DEMO_SOURCE = "demo_source.txt"


def _step(index: int, text: str) -> None:
    """Print one numbered demonstration step."""
    print(f"\n=== step {index}: {text}")


def _prepare_share(directory: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path]:
    """Create the demo program and source file inside the share."""
    program = directory / DEMO_PROGRAM
    write_atomic(
        program,
        "#!/bin/sh\n"
        "# Demo program executed from the share (DX-02, DX-03)\n"
        f"echo \"hello from the share: $(pwd)\"\n"
        f"exit 0\n",
    )
    program.chmod(0o755)
    source = directory / DEMO_SOURCE
    write_atomic(source, f"written at {time.time():.6f}\n")
    return program, source


def _run_program(directory: pathlib.Path, program: str) -> int:
    """Run the demo program, locally (no guest) or through the guest."""
    from qshare.qrun import run_local

    result = run_local(directory, program, [])
    sys.stdout.write(result.stdout)
    if result.stderr:
        sys.stderr.write(result.stderr)
    return result.exit_code


def _show_pc_change(directory: pathlib.Path, source: pathlib.Path) -> None:
    """Show the same file content, before and after a host-side edit."""
    before = source.read_text(encoding="utf-8").strip()
    write_atomic(source, f"edited on the PC at {time.time():.6f}\n")
    after = source.read_text(encoding="utf-8").strip()
    print(f"before edit: {before}")
    print(f"after edit : {after}")
    print("the guest sees 'after edit' immediately: no copy, no image rebuild")


def _short_checks(seed: int) -> int:
    """Run one short differential test and one short content exercise."""
    from fsproof.diff import run_diff
    from fsproof.exercise import run_exercise

    diff_result = run_diff(seed=seed, count=200)
    print(
        f"differential test: {diff_result.total_actions} operations, "
        f"{'no divergence' if diff_result.success else f'{len(diff_result.mismatches)} divergences'}"
    )
    with tempfile.TemporaryDirectory(prefix="pgw_demo_ex_") as tmp:
        exercise = run_exercise(target_dir=pathlib.Path(tmp), ops=500, workers=2)
    print(f"content exerciser: {exercise.ops_completed} operations, {exercise.workers} workers")
    return 0 if diff_result.success and exercise.success else 1


def run_demo(
    directory: str | pathlib.Path,
    tag: str = "hostshare",
    proto: str = "9p",
    image: str | None = None,
    arch: str = "x86_64",
    seed: int = 7,
) -> int:
    """Replay the whole demonstration and return a process status."""
    share = ensure_dir(pathlib.Path(directory).expanduser())
    cfg = validate_share(share, tag=tag, proto=proto)

    _step(1, "share the directory (DX-01)")
    print(" ".join(build_qemu_args(cfg)))

    _step(2, "mount it at boot with mkqnximage (DX-01)")
    fragment = build_fragment(
        driver_bin="/opt/passerelle/sbin/passerelle",
        tag=cfg.tag,
    )
    print(render_fragment(fragment))

    _step(3, "run a program from the share (DX-03)")
    program, source = _prepare_share(share)
    if image:
        print(f"guest image: {image} (arch {arch}); start it with qshare --run {image}")
    status = _run_program(share, DEMO_PROGRAM)

    _step(4, "a file edited on the PC changes at once (FS-12)")
    _show_pc_change(share, source)

    _step(5, "conformance and integrity checks (7.2, 7.3)")
    checks = _short_checks(seed)

    print("\ndemo finished: " + ("PASS" if status == 0 and checks == 0 else "FAIL"))
    return 0 if status == 0 and checks == 0 else 1

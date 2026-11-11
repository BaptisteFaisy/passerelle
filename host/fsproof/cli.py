# SPDX-License-Identifier: Apache-2.0
"""Command line interface of fsproof (7.2, 7.3, 7.4).

Subcommands:
  diff      seeded differential test, automatic reduction, regressions
  exercise  content exerciser, alone or with N workers, private or shared
  cases     the directed table of section 7.4
  clients   list the client profiles the same bench can drive

Every subcommand can target another client with --client, which is what
makes the comparison objective (PERF-07).
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile

from fsproof.cases import all_passed, run_cases
from fsproof.clients import describe_profiles, get_profile, profile_names
from fsproof.diff import load_regression, run_diff
from fsproof.exercise import exercise_plan, run_exercise
from pgwutil import HostError, dump_json, write_atomic


def _parser() -> argparse.ArgumentParser:
    """Argument parser for fsproof."""
    parser = argparse.ArgumentParser(
        prog="fsproof",
        description="Filesystem conformance bench: differential, content, directed (7.2-7.4)",
    )
    parser.add_argument(
        "--client", default="passerelle", help=f"client under test ({', '.join(profile_names())})"
    )
    subs = parser.add_subparsers(dest="command", required=True)

    diff = subs.add_parser("diff", help="seeded differential test against the PC truth")
    diff.add_argument("--seed", type=int, default=1, help="seed of the operation sequence")
    diff.add_argument("--count", type=int, default=200, help="number of operations")
    diff.add_argument("--local", action="store_true", help="replay on the PC, no guest")
    diff.add_argument(
        "--guest-cmd", help="guest agent command, e.g. 'qrun --console /tmp/ser.sock'"
    )
    diff.add_argument("--regressions", default="regressions", help="regression directory")
    diff.add_argument("--replay", metavar="FILE", help="replay a stored regression case")
    diff.add_argument("--mtime-slack", type=float, default=0.0, help="tolerated mtime drift in seconds")
    diff.add_argument("--json", metavar="FILE", help="also write the result as JSON")

    exercise = subs.add_parser("exercise", help="content exerciser (7.3)")
    exercise.add_argument("--dir", help="directory to exercise (default: temporary)")
    exercise.add_argument("--ops", type=int, default=10000, help="operations per run")
    exercise.add_argument("--workers", type=int, default=1, help="concurrent workers")
    exercise.add_argument("--shared", action="store_true", help="all workers on one file")
    exercise.add_argument("--plan", action="store_true", help="run the three shapes of 7.3")
    exercise.add_argument("--seed", type=int, default=2026, help="seed")

    cases = subs.add_parser("cases", help="directed cases of section 7.4")
    cases.add_argument("--dir", help="working directory (default: temporary)")
    cases.add_argument("--slow", action="store_true", help="include the 100 000-entry directory")
    cases.add_argument("--only", action="append", help="run only this case (repeatable)")

    subs.add_parser("clients", help="list the client profiles")
    return parser


def _diff_command(args: argparse.Namespace) -> int:
    """Run the differential test, or replay a stored regression."""
    profile = get_profile(args.client)
    result = _run_requested_diff(args, profile.name)
    _print_diff_result(result, args.json)
    return 0 if result.success else 1


def _run_requested_diff(args: argparse.Namespace, client: str):
    """Build the run parameters and execute the differential test."""
    mode = "local" if args.local or not args.guest_cmd else "guest"
    command = None if not args.guest_cmd else args.guest_cmd.split()
    slack_ns = int(args.mtime_slack * 1e9)
    if args.replay:
        actions = load_regression(pathlib.Path(args.replay))
        print(f"replaying {len(actions)} stored operations from {args.replay}")
        return run_diff(
            seed=-1,
            count=len(actions),
            mode=mode,
            guest_command=command,
            client=client,
            reduce_divergence=False,
            mtime_slack_ns=slack_ns,
            actions=actions,
        )
    return run_diff(
        seed=args.seed,
        count=args.count,
        mode=mode,
        guest_command=command,
        client=client,
        reg_dir=args.regressions,
        mtime_slack_ns=slack_ns,
    )


def _print_diff_result(result, json_path: str | None) -> None:
    """Print the verdict, the divergences, the reduction and the regression."""
    verdict = "PASS" if result.success else "FAIL"
    print(
        f"diff [{result.client}] mode={result.mode} seed={result.seed} "
        f"ops={result.total_actions} divergences={len(result.divergences)} "
        f"time={result.duration_s:.2f}s -> {verdict}"
    )
    for line in result.mismatches[:20]:
        print(f"  {line}")
    if result.errors:
        print(f"  {len(result.errors)} replay errors (first: {result.errors[0]})")
    if result.reduced is not None:
        lines = [action.line() for action in result.reduced]
        print(f"  reduced to {len(result.reduced)} operations: {lines[:8]}")
    if result.regression is not None:
        print(f"  regression stored in {result.regression}")
    if json_path:
        write_atomic(
            pathlib.Path(json_path),
            dump_json(
                {
                    "client": result.client,
                    "mode": result.mode,
                    "seed": result.seed,
                    "actions": result.total_actions,
                    "divergences": result.mismatches,
                    "reduced": None
                    if result.reduced is None
                    else [action.line() for action in result.reduced],
                }
            ),
        )


def _exercise_command(args: argparse.Namespace) -> int:
    """Run the content exerciser."""
    directory = pathlib.Path(args.dir) if args.dir else pathlib.Path(
        tempfile.mkdtemp(prefix="fsproof_ex_")
    )
    if args.plan:
        results = exercise_plan(directory, ops=args.ops, workers=max(2, args.workers))
    else:
        results = [
            run_exercise(
                directory,
                ops=args.ops,
                workers=args.workers,
                shared_file=args.shared,
                seed=args.seed,
            )
        ]
    status = 0
    for result in results:
        label = "shared" if result.shared_file else "private"
        verdict = "PASS" if result.success else "FAIL"
        print(
            f"exercise {label} workers={result.workers} ops={result.ops_completed} "
            f"time={result.duration_s:.2f}s rate={result.ops_per_second:.0f}op/s -> {verdict}"
        )
        if not result.success:
            print(f"  {result.mismatch}")
            status = 1
    return status


def _cases_command(args: argparse.Namespace) -> int:
    """Run the directed table."""
    directory = pathlib.Path(args.dir) if args.dir else pathlib.Path(
        tempfile.mkdtemp(prefix="fsproof_cases_")
    )
    profile = get_profile(args.client)
    results = run_cases(directory, include_slow=args.slow, select=args.only)
    for result in results:
        verdict = "PASS" if result.passed else "FAIL"
        detail = f" ({result.detail})" if result.detail else ""
        print(f"[{verdict}] {result.name}{detail}")
    print(
        f"cases [{profile.name}]: {sum(1 for r in results if r.passed)}/{len(results)} passed"
    )
    return 0 if all_passed(results) else 1


def main(argv: list[str] | None = None) -> int:
    """Entry point of the fsproof command."""
    args = _parser().parse_args(argv)
    try:
        if args.command == "diff":
            return _diff_command(args)
        if args.command == "exercise":
            return _exercise_command(args)
        if args.command == "cases":
            return _cases_command(args)
        print(describe_profiles())
        return 0
    except (HostError, KeyError) as err:
        sys.stderr.write(f"fsproof: {err}\n")
        return 2

# SPDX-License-Identifier: Apache-2.0
"""Command line interface of pbench (7.5, 7.6, PERF-01..PERF-07).

  pbench roundtrips  count roundtrips per POSIX call from a QEMU 9P trace
  pbench throughput  sequential read/write
  pbench metadata    10 000 files created, stat'ed, removed
  pbench tree        50 000-entry tree walk
  pbench mixed       source-like tree, then a text search
  pbench exec        1 000 program launches from the share
  pbench priority    4 KiB read during a bulk copy (PERF-05)
  pbench all         every workload in one go, written to JSON
  pbench report      aggregate runs into Markdown + JSON + SVG, check 7.6
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile

from bench.loads import WORKLOADS, LoadResult, run_workload
from bench.report import LEVELS, TargetRun, build_report, write_report
from bench.roundtrips import build_table, render_markdown
from bench.trace import parse_trace
from pgwutil import HostError, dump_json, load_json, write_atomic


def _add_workload_parsers(subs: object) -> None:
    """Register the roundtrips and per-workload subcommands."""
    rounds = subs.add_parser("roundtrips", help="roundtrips per POSIX call from a trace")
    rounds.add_argument("trace", help="QEMU trace file (-trace 'v9fs_*')")
    rounds.add_argument("--client", default="passerelle", help="client name for the report")
    rounds.add_argument("--markdown", metavar="FILE", help="write the table as Markdown")
    rounds.add_argument("--json", metavar="FILE", help="write the table as JSON")
    for name in WORKLOADS:
        sub = subs.add_parser(name, help=f"run the {name} workload")
        sub.add_argument("--dir", help="working directory (default: temporary)")
        sub.add_argument("--scale", type=float, default=1.0, help="1.0 = specification size")
        sub.add_argument("--json", metavar="FILE", help="write the result as JSON")


def _add_all_parser(subs: object) -> None:
    """Register the 'all' subcommand and its conformance options."""
    every = subs.add_parser("all", help="run every workload and write one JSON file")
    every.add_argument("--dir", help="working directory (default: temporary)")
    every.add_argument("--scale", type=float, default=1.0, help="1.0 = specification size")
    every.add_argument("--client", default="passerelle", help="client name")
    every.add_argument("--json", metavar="FILE", required=True, help="output JSON file")
    every.add_argument("--mount-ms", type=float, default=0.0, help="measured mount time (PERF-06)")
    every.add_argument("--diff-ops", type=int, default=0, help="differential operations run")
    every.add_argument("--diff-divergences", type=int, default=0, help="divergences seen")
    every.add_argument("--exercise-ops", type=int, default=0, help="exerciser operations run")
    every.add_argument("--exercise-workers", type=int, default=1, help="exerciser workers used")
    every.add_argument("--cases-passed", type=int, default=0, help="directed cases passed")
    every.add_argument("--cases-total", type=int, default=0, help="directed cases run")
    every.add_argument("--leaks", type=int, default=0, help="leaks after a million opens")
    every.add_argument("--fuzz-seconds", type=float, default=0.0, help="random-input run time")
    every.add_argument("--virtiofs", action="store_true", help="also ran the virtiofs path")
    every.add_argument(
        "--conformance",
        action="store_true",
        help="also run fsproof (diff, exerciser, cases) and fill those figures",
    )
    every.add_argument(
        "--conformance-scale", type=float, default=0.01,
        help="scale of the conformance run (1.0 = socle sizes, slow)",
    )
    every.add_argument("--workers", type=int, default=16, help="exerciser workers")


def _add_report_parsers(subs: object) -> None:
    """Register the mount, report and workloads subcommands."""
    mount = subs.add_parser("mount", help="measure the guest mount time (PERF-06)")
    mount.add_argument("--console", required=True, help="guest console unix socket")
    mount.add_argument("--driver", default="passerelle", help="guest driver command")
    mount.add_argument("--tag", default="hostshare", help="mount tag")
    mount.add_argument("--mount-point", default="/host", help="guest mount point")
    mount.add_argument("--json", metavar="FILE", help="write the measurement as JSON")
    report = subs.add_parser("report", help="aggregate runs and check section 7.6")
    report.add_argument(
        "--run", action="append", default=[], metavar="JSON",
        help="JSON file produced by 'pbench all' (repeatable)",
    )
    report.add_argument("--level", choices=tuple(LEVELS), default="socle", help="threshold level")
    report.add_argument("--output", "-o", default="bench_report", help="output prefix")
    subs.add_parser("workloads", help="list the workload names")


def _parser() -> argparse.ArgumentParser:
    """Argument parser for pbench."""
    parser = argparse.ArgumentParser(
        prog="pbench",
        description="Passerelle measurements: roundtrips, workloads, comparative report",
    )
    subs = parser.add_subparsers(dest="command", required=True)
    _add_workload_parsers(subs)
    _add_all_parser(subs)
    _add_report_parsers(subs)
    return parser


def _write_result(result: LoadResult, path: str | None) -> None:
    """Print one workload result, optionally writing it as JSON."""
    print(
        f"{result.workload}: {result.operations} ops in {result.duration_s:.3f}s "
        f"({result.ops_per_second:.0f} op/s, {result.throughput_mib_s:.1f} MiB/s) -- {result.detail}"
    )
    if path:
        write_atomic(pathlib.Path(path), dump_json({
            "workload": result.workload,
            "operations": result.operations,
            "duration_s": result.duration_s,
            "bytes_moved": result.bytes_moved,
            "throughput_mib_s": result.throughput_mib_s,
            "ops_per_second": result.ops_per_second,
            "detail": result.detail,
            "extra": result.extra,
        }))


def _roundtrips(args: argparse.Namespace) -> int:
    """Analyse a trace and publish the roundtrip table."""
    trace_path = pathlib.Path(args.trace).expanduser()
    if not trace_path.exists():
        raise HostError(f"trace file not found: {trace_path}")
    log = parse_trace(trace_path.read_text(encoding="utf-8", errors="replace"))
    if not log.events:
        raise HostError(
            f"no 9P event in {trace_path}; start QEMU with -trace 'enable=v9fs_*' -trace file=..."
        )
    table = build_table(log)
    markdown = render_markdown(table, args.client)
    print(markdown)
    if log.bad_lines:
        print(f"\n{len(log.bad_lines)} unparsable 9P line(s), first: {log.bad_lines[0][:120]}")
    if args.markdown:
        write_atomic(pathlib.Path(args.markdown), markdown + "\n")
    if args.json:
        write_atomic(pathlib.Path(args.json), dump_json(table.to_json()))
    return 0


def _workload(args: argparse.Namespace) -> int:
    """Run one workload."""
    directory = pathlib.Path(args.dir) if args.dir else pathlib.Path(
        tempfile.mkdtemp(prefix=f"pbench_{args.command}_")
    )
    _write_result(run_workload(args.command, directory, scale=args.scale), args.json)
    return 0


def _run_payload(args: argparse.Namespace, results: dict[str, LoadResult]) -> dict[str, object]:
    """Build the JSON payload of a 'pbench all' run file."""
    return {
        "client": args.client,
        "label": args.client,
        "scale": args.scale,
        "mount_time_ms": args.mount_ms,
        "diff_ops": args.diff_ops,
        "diff_divergences": args.diff_divergences,
        "exercise_ops": args.exercise_ops,
        "exercise_workers": args.exercise_workers,
        "cases_passed": args.cases_passed,
        "cases_total": args.cases_total,
        "leaks": args.leaks,
        "fuzz_seconds": args.fuzz_seconds,
        "virtiofs": args.virtiofs,
        "workloads": {
            name: {
                "operations": result.operations,
                "duration_s": result.duration_s,
                "bytes_moved": result.bytes_moved,
                "throughput_mib_s": result.throughput_mib_s,
                "ops_per_second": result.ops_per_second,
                "detail": result.detail,
                "extra": result.extra,
            }
            for name, result in results.items()
        },
    }


def _all(args: argparse.Namespace) -> int:
    """Run every workload and write one JSON file per client."""
    directory = pathlib.Path(args.dir) if args.dir else pathlib.Path(
        tempfile.mkdtemp(prefix="pbench_all_")
    )
    results = {name: run_workload(name, directory, scale=args.scale) for name in WORKLOADS}
    for result in results.values():
        _write_result(result, None)
    figures: dict[str, int] = {}
    if args.conformance:
        from bench.conformance import measure, summarize

        measured = measure(
            scale=args.conformance_scale, workers=args.workers, work=directory / "conformance"
        )
        print(summarize(measured))
        figures = measured.as_dict()
    payload = _run_payload(args, results)
    payload.update(figures)
    write_atomic(pathlib.Path(args.json), dump_json(payload))
    print(f"written {args.json}")
    return 0


def _mount(args: argparse.Namespace) -> int:
    """Measure the guest mount time (PERF-06)."""
    from bench.mount import describe, measure_mount_ms

    mount_ms = measure_mount_ms(
        console=pathlib.Path(args.console),
        driver=args.driver,
        tag=args.tag,
        mount_point=args.mount_point,
    )
    print(describe(mount_ms))
    if args.json:
        write_atomic(pathlib.Path(args.json), dump_json({"mount_time_ms": mount_ms}))
    return 0 if mount_ms < 50.0 else 1


def _load_run(path: pathlib.Path) -> TargetRun:
    """Rebuild a TargetRun from a JSON file produced by 'pbench all'."""
    payload = load_json(path)
    workloads: dict[str, LoadResult] = {}
    for name, item in payload.get("workloads", {}).items():
        workloads[name] = LoadResult(
            workload=name,
            operations=int(item.get("operations", 0)),
            duration_s=float(item.get("duration_s", 0.0)),
            bytes_moved=int(item.get("bytes_moved", 0)),
            detail=str(item.get("detail", "")),
            extra={key: float(value) for key, value in item.get("extra", {}).items()},
        )
    return TargetRun(
        client=str(payload.get("client", path.stem)),
        label=str(payload.get("label", payload.get("client", path.stem))),
        workloads=workloads,
        mount_time_ms=float(payload.get("mount_time_ms", 0.0)),
        diff_ops=int(payload.get("diff_ops", 0)),
        diff_divergences=int(payload.get("diff_divergences", 0)),
        exercise_ops=int(payload.get("exercise_ops", 0)),
        exercise_workers=int(payload.get("exercise_workers", 1)),
        exercise_mismatches=int(payload.get("exercise_mismatches", 0)),
        cases_passed=int(payload.get("cases_passed", 0)),
        cases_total=int(payload.get("cases_total", 0)),
        leaks=int(payload.get("leaks", 0)),
        fuzz_seconds=float(payload.get("fuzz_seconds", 0.0)),
        virtiofs=bool(payload.get("virtiofs", False)),
    )


def _report(args: argparse.Namespace) -> int:
    """Aggregate runs and check the thresholds."""
    if not args.run:
        raise HostError("report needs at least one --run JSON file (see 'pbench all')")
    runs = [_load_run(pathlib.Path(item).expanduser()) for item in args.run]
    report = build_report(runs, level=args.level)
    written = write_report(report, args.output)
    for check in report.checks:
        verdict = "PASS" if check.passed else "FAIL"
        print(f"[{verdict}] {check.requirement}: {check.measured} (needs {check.threshold})")
    for kind, path in written.items():
        print(f"{kind}: {path}")
    print("verdict: " + ("PASS" if report.passed else "FAIL"))
    return 0 if report.passed else 1


def main(argv: list[str] | None = None) -> int:
    """Entry point of the pbench command."""
    args = _parser().parse_args(argv)
    try:
        if args.command == "roundtrips":
            return _roundtrips(args)
        if args.command == "all":
            return _all(args)
        if args.command == "report":
            return _report(args)
        if args.command == "mount":
            return _mount(args)
        if args.command == "workloads":
            print(" ".join(WORKLOADS))
            return 0
        return _workload(args)
    except (HostError, OSError, ValueError) as err:
        sys.stderr.write(f"pbench: {err}\n")
        return 2

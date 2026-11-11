# SPDX-License-Identifier: Apache-2.0
"""pbench -- measurements and comparative report (7.5, 7.6, PERF-01..07).

Exposes the trace parser, the roundtrip table, the six workloads and the
report aggregation, all with the standard library only.
"""

from __future__ import annotations

from bench.loads import (
    WORKLOADS,
    LoadResult,
    run_exec,
    run_metadata,
    run_mixed,
    run_priority,
    run_throughput,
    run_tree,
    run_workload,
)
from bench.report import (
    LEVELS,
    Check,
    Report,
    TargetRun,
    build_report,
    evaluate,
    render_markdown,
    write_report,
)
from bench.roundtrips import (
    OperationStat,
    RoundtripTable,
    build_table,
    compare_tables,
    render_markdown as render_roundtrips,
)
from bench.trace import Exchange, TraceEvent, TraceLog, parse_line, parse_trace, posix_operation

__all__ = [
    "LEVELS",
    "WORKLOADS",
    "Check",
    "Exchange",
    "LoadResult",
    "OperationStat",
    "Report",
    "RoundtripTable",
    "TargetRun",
    "TraceEvent",
    "TraceLog",
    "build_report",
    "build_table",
    "compare_tables",
    "evaluate",
    "parse_line",
    "parse_trace",
    "posix_operation",
    "render_markdown",
    "render_roundtrips",
    "run_exec",
    "run_metadata",
    "run_mixed",
    "run_priority",
    "run_throughput",
    "run_tree",
    "run_workload",
    "write_report",
]

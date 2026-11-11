# SPDX-License-Identifier: Apache-2.0
"""Roundtrip table per POSIX operation (PERF-01, PERF-02, PERF-07).

A roundtrip is one request sent to the server and its reply: the number
of roundtrips per POSIX call is a property of the client's design, not of
the machine, which is why the specification expresses its performance
requirements that way. This module aggregates a parsed QEMU trace into
that table and renders it as Markdown and JSON.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass

from bench.trace import TraceLog, posix_operation


@dataclass(frozen=True)
class OperationStat:
    """Roundtrip statistics for one POSIX operation.

    Latencies are None when the trace carries no timestamps: QEMU only
    writes them when the trace backend is asked to, and inventing a 0.000 ms
    latency would be worse than publishing nothing.
    """

    operation: str
    requests: int
    replies: int
    errors: int
    roundtrips_per_call: float
    mean_latency_ms: float | None
    max_latency_ms: float | None


@dataclass(frozen=True)
class RoundtripTable:
    """The published table: one row per operation plus a total."""

    rows: list[OperationStat]
    total_requests: int
    total_replies: int
    total_errors: int
    unpaired: int
    timed: bool = True

    def to_json(self) -> dict[str, object]:
        """JSON-serialisable form (published by the report)."""
        return {
            "rows": [asdict(row) for row in self.rows],
            "total_requests": self.total_requests,
            "total_replies": self.total_replies,
            "total_errors": self.total_errors,
            "unpaired": self.unpaired,
            "timed": self.timed,
        }


def build_table(log: TraceLog) -> RoundtripTable:
    """Aggregate a trace into one row per POSIX operation."""
    requests: dict[str, int] = {}
    replies: dict[str, int] = {}
    errors: dict[str, int] = {}
    latencies: dict[str, list[float]] = {}
    unpaired = 0
    timed = any(event.timestamp > 0 for event in log.events)

    for exchange in log.exchanges():
        operation = posix_operation(exchange.request.event)
        requests[operation] = requests.get(operation, 0) + 1
        if not exchange.answered:
            unpaired += 1
            continue
        replies[operation] = replies.get(operation, 0) + 1
        if exchange.error:
            errors[operation] = errors.get(operation, 0) + 1
        if timed and exchange.reply is not None:
            latencies.setdefault(operation, []).append(exchange.latency_s * 1000.0)

    rows: list[OperationStat] = []
    for operation in sorted(requests):
        count = requests[operation]
        answered = replies.get(operation, 0)
        samples = latencies.get(operation, [])
        rows.append(
            OperationStat(
                operation=operation,
                requests=count,
                replies=answered,
                errors=errors.get(operation, 0),
                roundtrips_per_call=round(answered / count, 3) if count else 0.0,
                mean_latency_ms=_round(sum(samples) / len(samples)) if samples else None,
                max_latency_ms=_round(max(samples)) if samples else None,
            )
        )
    return RoundtripTable(
        rows=rows,
        total_requests=sum(requests.values()),
        total_replies=sum(replies.values()),
        total_errors=sum(errors.values()),
        unpaired=unpaired,
        timed=timed,
    )


def _round(value: float) -> float:
    """Round a latency to microseconds."""
    return round(value, 3)


def _cell(value: float | None) -> str:
    """Render a latency cell, or 'n/a' when the trace had no timestamps."""
    return "n/a" if value is None else f"{value:.3f}"


def render_markdown(table: RoundtripTable, client: str = "passerelle") -> str:
    """Render the roundtrip table as Markdown (PERF-01 publication)."""
    lines = [
        f"# Roundtrips per POSIX operation -- {client}",
        "",
        "| POSIX operation | 9P requests | replies | errors | roundtrips/call | mean ms | max ms |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for row in table.rows:
        lines.append(
            f"| {row.operation} | {row.requests} | {row.replies} | {row.errors} | "
            f"{row.roundtrips_per_call:.2f} | {_cell(row.mean_latency_ms)} | "
            f"{_cell(row.max_latency_ms)} |"
        )
    lines.append(
        f"| **total** | **{table.total_requests}** | **{table.total_replies}** | "
        f"**{table.total_errors}** | | | |"
    )
    if table.unpaired:
        lines.append("")
        lines.append(f"{table.unpaired} request(s) had no reply in the trace.")
    if not table.timed:
        lines.append("")
        lines.append(
            "The trace carries no timestamps, so latencies are not reported; "
            "roundtrips per call do not depend on them."
        )
    return "\n".join(lines)


def compare_tables(
    mine: RoundtripTable, others: dict[str, RoundtripTable]
) -> dict[str, list[str]]:
    """Report operations where *mine* uses more roundtrips (PERF-02)."""
    worse: dict[str, list[str]] = {}
    mine_by_op = {row.operation: row.roundtrips_per_call for row in mine.rows}
    for name, table in others.items():
        findings: list[str] = []
        for row in table.rows:
            if row.operation not in mine_by_op:
                continue
            ours = mine_by_op[row.operation]
            if ours > row.roundtrips_per_call:
                findings.append(
                    f"{row.operation}: {ours:.2f} roundtrips/call vs {row.roundtrips_per_call:.2f} for {name}"
                )
        if findings:
            worse[name] = findings
    return worse

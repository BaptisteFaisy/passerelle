# SPDX-License-Identifier: Apache-2.0
"""Hand-written SVG bar chart, the only chart code of the project.

Covers PERF-07: the comparative report publishes a chart without depending
on matplotlib or any other third-party package. If matplotlib happens to be
installed the report also writes a PNG, but this SVG is always produced, so
a plain Python installation can always publish its numbers.
"""

from __future__ import annotations

from typing import Sequence

BAR_HEIGHT = 22
ROW_HEIGHT = 34
LABEL_WIDTH = 210
CHART_WIDTH = 640
VALUE_GAP = 8


def _escape(text: str) -> str:
    """Escape the XML special characters."""
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
        .replace("'", "&apos;")
    )


def _row(index: int, label: str, value: float, ratio: float, unit: str) -> list[str]:
    """One labelled horizontal bar."""
    y = 40 + index * ROW_HEIGHT
    width = max(1, int((CHART_WIDTH - LABEL_WIDTH - 80) * ratio))
    return [
        f'<text x="8" y="{y + 15}" font-size="12">{_escape(label)}</text>',
        f'<rect x="{LABEL_WIDTH}" y="{y}" width="{width}" height="{BAR_HEIGHT}" fill="#3b7dd8"/>',
        f'<text x="{LABEL_WIDTH + width + VALUE_GAP}" y="{y + 15}" font-size="12">'
        f"{value:.1f} {_escape(unit)}</text>",
    ]


def bar_chart(
    title: str, labels: Sequence[str], values: Sequence[float], unit: str = "MiB/s"
) -> str:
    """Render a horizontal bar chart: one bar per label, scaled to the peak."""
    peak = max(list(values) + [0.0]) or 1.0
    height = 60 + ROW_HEIGHT * max(1, len(labels))
    rows: list[str] = []
    for index, (label, value) in enumerate(zip(labels, values)):
        rows.extend(_row(index, label, value, value / peak, unit))
    header = (
        f'<text x="8" y="24" font-size="14" font-weight="bold">{_escape(title)}</text>'
    )
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{CHART_WIDTH}" height="{height}">'
        '<rect width="100%" height="100%" fill="#ffffff"/>'
        f"{header}{''.join(rows)}</svg>"
    )

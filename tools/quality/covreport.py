#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Coverage reports written with the standard library only.

Four artefacts are produced from the aggregated gcov data of covcollect.py:

  summary.txt    the human readable report (global score, per module table,
                 number of uncovered lines, tool versions);
  badge.svg      a shields-style badge drawn by hand, so the README can show
                 the score without any network call or third-party service;
  coverage.xml   a Cobertura 1.9 document, the format read by GitHub Actions
                 coverage viewers and by most dashboards;
  index.html     a one-page report that opens straight from the artefact.

Requirements covered: 7.6 (measurements are published as artefacts, not only
logged) and the hackathon brief (standard library only, English comments).
"""

from __future__ import annotations

import datetime
import pathlib
import xml.sax.saxutils as sax

import qcommon

BADGE_COLORS = (
    (90.0, "#4c1"),
    (75.0, "#97ca00"),
    (60.0, "#a4a61d"),
    (50.0, "#dfb317"),
    (25.0, "#fe7d37"),
    (0.0, "#e05d44"),
)


def percent(covered: int, total: int) -> float:
    """Return the percentage of covered items (0.0 when nothing to measure)."""
    return 100.0 * covered / total if total else 0.0


def file_stats(agg: dict) -> dict:
    """Turn the aggregated line counts into per-file statistics."""
    stats = {}
    for name, data in agg.items():
        lines = data["lines"]
        covered = sum(1 for count in lines.values() if count > 0)
        total = len(lines)
        funcs = data.get("functions", {})
        fcovered = sum(1 for hits in funcs.values() if hits > 0)
        stats[name] = {
            "lines": total,
            "covered": covered,
            "uncovered": sorted(n for n, c in lines.items() if c == 0),
            "functions": len(funcs),
            "functions_covered": fcovered,
            "pct": percent(covered, total),
            "rel": pathlib.PurePosixPath(name),
        }
    return stats


def module_table(stats: dict) -> list[dict]:
    """Aggregate the per-file statistics by module (production code first)."""
    groups: dict[str, dict] = {}
    for name, item in stats.items():
        rel = item["rel"]
        module = qcommon.module_of(rel)
        scope = "test" if qcommon.is_test_path(rel) else "prod"
        key = "%s/%s" % (module, scope)
        slot = groups.setdefault(key, {"name": module, "scope": scope,
                                       "files": 0, "lines": 0, "covered": 0,
                                       "functions": 0, "functions_covered": 0})
        slot["files"] += 1
        slot["lines"] += item["lines"]
        slot["covered"] += item["covered"]
        slot["functions"] += item["functions"]
        slot["functions_covered"] += item["functions_covered"]
    for slot in groups.values():
        slot["uncovered"] = slot["lines"] - slot["covered"]
        slot["pct"] = percent(slot["covered"], slot["lines"])
    order = {"prod": 0, "test": 1}
    return sorted(groups.values(),
                  key=lambda s: (order[s["scope"]], -s["pct"], s["name"]))


def totals(stats: dict, scope: str | None = None) -> dict:
    """Sum the statistics of the whole tree (or of one scope: prod/test)."""
    lines = covered = functions = fcovered = files = 0
    for name, item in stats.items():
        if scope == "prod" and qcommon.is_test_path(item["rel"]):
            continue
        if scope == "test" and not qcommon.is_test_path(item["rel"]):
            continue
        files += 1
        lines += item["lines"]
        covered += item["covered"]
        functions += item["functions"]
        fcovered += item["functions_covered"]
    return {"files": files, "lines": lines, "covered": covered,
            "uncovered": lines - covered, "functions": functions,
            "functions_covered": fcovered, "pct": percent(covered, lines),
            "fn_pct": percent(fcovered, functions)}


def _ranges(numbers: list[int], limit: int = 12) -> str:
    """Compress a sorted line list into ranges: 3-7, 12, 40-44."""
    out: list[str] = []
    start = prev = None
    for number in numbers:
        if start is None:
            start = prev = number
        elif number == prev + 1:
            prev = number
        else:
            out.append(str(start) if start == prev else "%d-%d" % (start, prev))
            start = prev = number
    if start is not None:
        out.append(str(start) if start == prev else "%d-%d" % (start, prev))
    if len(out) > limit:
        return ", ".join(out[:limit]) + ", ... (%d ranges)" % len(out)
    return ", ".join(out) if out else "-"


def summary_text(stats: dict, counts: dict, versions: dict) -> str:
    """Build the text report: global score, per module, worst files."""
    prod = totals(stats, "prod")
    everything = totals(stats)
    lines = [
        "passerelle coverage summary",
        "generated: %s" % datetime.datetime.now(datetime.timezone.utc)
                        .isoformat(timespec="seconds"),
        "tools: %s" % ", ".join("%s %s" % kv for kv in versions.items()),
        "gcov data files: %(gcda)d (failed: %(failed)d) -> %(sources)d source(s)"
        % counts,
        "",
        "PRODUCTION CODE : %6.2f %% lines (%d/%d, %d uncovered)  "
        "functions %.2f %% (%d/%d)"
        % (prod["pct"], prod["covered"], prod["lines"], prod["uncovered"],
           prod["fn_pct"], prod["functions_covered"], prod["functions"]),
        "ALL MEASURED    : %6.2f %% lines (%d/%d, %d uncovered)"
        % (everything["pct"], everything["covered"], everything["lines"],
           everything["uncovered"]),
        "",
        "%-24s %5s %8s %8s %9s %8s" % ("module", "scope", "lines", "covered",
                                       "uncov.", "pct"),
    ]
    for slot in module_table(stats):
        lines.append("%-24s %5s %8d %8d %9d %7.2f%%"
                     % (slot["name"], slot["scope"], slot["lines"],
                        slot["covered"], slot["uncovered"], slot["pct"]))
    lines += ["", "files with the most uncovered lines:"]
    worst = sorted(stats.items(), key=lambda kv: -len(kv[1]["uncovered"]))[:10]
    for name, item in worst:
        lines.append("  %-46s %5d uncovered (%.1f%%)  lines %s"
                     % (name, len(item["uncovered"]), item["pct"],
                        _ranges(item["uncovered"])))
    return "\n".join(lines) + "\n"


def badge_svg(pct: float, label: str = "coverage") -> str:
    """Return a shields-style badge drawn by hand (no dependency)."""
    color = next(c for threshold, c in BADGE_COLORS if pct >= threshold)
    value = "%.1f%%" % pct
    left, right = 62, 12 + 7 * len(value)
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="20" '
        'role="img" aria-label="%s: %s">'
        '<rect width="%d" height="20" fill="#555"/>'
        '<rect x="%d" width="%d" height="20" fill="%s"/>'
        '<g fill="#fff" text-anchor="middle" '
        'font-family="DejaVu Sans,Verdana,sans-serif" font-size="11">'
        '<text x="%d" y="14">%s</text>'
        '<text x="%d" y="14">%s</text></g></svg>\n'
        % (left + right, sax.escape(label), sax.escape(value), left + right,
           left, right, color, left // 2, sax.escape(label),
           left + right // 2, sax.escape(value))
    )


def _xml_lines(handle, stats: dict, name: str) -> None:
    """Write the <lines> block of one Cobertura <class> element."""
    handle.write('        <lines>\n')
    for number in sorted(stats["lines_by_number"]):
        handle.write('          <line number="%d" hits="%d" branch="false"/>\n'
                     % (number, stats["lines_by_number"][number]))
    handle.write('        </lines>\n')


def write_cobertura(path: pathlib.Path, agg: dict, stats: dict) -> None:
    """Write a Cobertura 1.9 XML report (line level, one class per file)."""
    everything = totals(stats)
    stamp = int(datetime.datetime.now(datetime.timezone.utc).timestamp())
    with path.open("w", encoding="utf-8") as handle:
        handle.write('<?xml version="1.0" ?>\n')
        handle.write('<!DOCTYPE coverage SYSTEM "http://cobertura.sourceforge.net'
                     '/xml/coverage-04.dtd">\n')
        handle.write('<coverage line-rate="%.4f" lines-covered="%d" '
                     'lines-valid="%d" branch-rate="0" branches-covered="0" '
                     'branches-valid="0" complexity="0" version="1.9" '
                     'timestamp="%d">\n'
                     % (everything["pct"] / 100.0, everything["covered"],
                        everything["lines"], stamp))
        handle.write('  <sources><source>.</source></sources>\n  <packages>\n')
        for module in sorted({qcommon.module_of(s["rel"]) for s in stats.values()}):
            files = {n: s for n, s in stats.items()
                     if qcommon.module_of(s["rel"]) == module}
            tot = totals(files)
            handle.write('    <package name="%s" line-rate="%.4f" '
                         'branch-rate="0" complexity="0">\n      <classes>\n'
                         % (sax.escape(module), tot["pct"] / 100.0))
            for name in sorted(files):
                item = files[name]
                handle.write('        <class name="%s" filename="%s" '
                             'line-rate="%.4f" branch-rate="0" complexity="0">\n'
                             % (sax.escape(name), sax.escape(name),
                                item["pct"] / 100.0))
                item = dict(item, lines_by_number=agg[name]["lines"])
                _xml_lines(handle, item, name)
                handle.write('        </class>\n')
            handle.write('      </classes>\n    </package>\n')
        handle.write('  </packages>\n</coverage>\n')


def write_html(path: pathlib.Path, stats: dict, counts: dict) -> None:
    """Write a one-page HTML report (opens straight from the artefact)."""
    prod = totals(stats, "prod")
    with path.open("w", encoding="utf-8") as handle:
        handle.write('<!doctype html><html><head><meta charset="utf-8">'
                     '<title>passerelle coverage</title><style>'
                     'body{font:14px/1.5 system-ui,sans-serif;margin:2em}'
                     'table{border-collapse:collapse}td,th{border-bottom:1px '
                     'solid #ddd;padding:.3em .8em;text-align:right}'
                     'th:first-child,td:first-child{text-align:left}'
                     '</style></head><body>\n')
        handle.write('<h1>passerelle coverage</h1>')
        handle.write('<p><b>%.2f%%</b> of production lines covered '
                     '(%d/%d, %d uncovered) &mdash; %d source files, '
                     '%d gcov data files.</p>\n'
                     % (prod["pct"], prod["covered"], prod["lines"],
                        prod["uncovered"], prod["files"], counts["gcda"]))
        handle.write('<table><tr><th>file</th><th>lines</th><th>covered</th>'
                     '<th>uncovered</th><th>%</th></tr>\n')
        for name in sorted(stats, key=lambda n: stats[n]["pct"]):
            item = stats[name]
            handle.write('<tr><td>%s</td><td>%d</td><td>%d</td><td>%d</td>'
                         '<td>%.1f</td></tr>\n'
                         % (sax.escape(name), item["lines"], item["covered"],
                            len(item["uncovered"]), item["pct"]))
        handle.write('</table>\n</body></html>\n')


def write_summary(path: pathlib.Path, stats: dict, counts: dict,
                  versions: dict) -> dict:
    """Write summary.txt + badge.svg and return the production totals."""
    prod = totals(stats, "prod")
    path.write_text(summary_text(stats, counts, versions), encoding="utf-8")
    badge = path.parent / "badge.svg"
    badge.write_text(badge_svg(prod["pct"]), encoding="utf-8")
    return prod

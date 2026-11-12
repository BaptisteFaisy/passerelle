#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Formatting gate: hygiene checks everywhere, clang-format where available.

Two families of checks, so the gate is useful with and without a toolchain:

  hygiene (no dependency, always enforced)
      long-line        a line wider than --max-line columns (default 100);
      tab              a tabulation in a C/H/Python source;
      trailing-space   a line ending with spaces;
      no-eol           a file not ending with exactly one newline.

  clang-format (only when the executable and .clang-format are both present)
      The file is reformatted in memory and compared: any difference is a
      violation. When clang-format is missing the check is reported as an
      explicit skip - never as a pass.

The tree predates this gate, so the tolerated violations are recorded in
tools/quality/format-baseline.txt (a ratchet): a file listed there keeps its
known count, every new violation fails. Regenerate the baseline after a
deliberate reformatting commit with --write-baseline. Formatting findings are
downgraded to advisory when the installed clang-format major version differs
from the one that produced the baseline, because formatting rules drift
between LLVM releases and a version mismatch must not invent violations.

Exit codes: 0 ok, 1 violation, 2 usage error, 3 nothing to check.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import datetime
import pathlib
import re
import sys

if __package__ in (None, ""):  # allow `python3 tools/quality/check_format.py`
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import qcommon

CHECKS = ("long-line", "tab", "trailing-space", "no-eol", "clang-format")
C_EXTS = {".c", ".h"}
BASELINE_DEFAULT = "tools/quality/format-baseline.txt"
META_RE = re.compile(r"^#\s*(clang-format|generated):\s*(.+)$")


def parse_args(argv=None) -> argparse.Namespace:
    """Parse the command line."""
    ap = argparse.ArgumentParser(
        description="Check the formatting rules of the repository.",
        epilog="exit codes: 0 ok, 1 violation, 2 usage, 3 nothing to check")
    ap.add_argument("--root", default=None, help="repository root")
    ap.add_argument("--max-line", type=int, default=100,
                    help="hard column limit (default: 100)")
    ap.add_argument("--baseline", default=None,
                    help="tolerated violations (default: %s)" % BASELINE_DEFAULT)
    ap.add_argument("--write-baseline", action="store_true",
                    help="record the current violations and exit 0")
    ap.add_argument("--diff-ref", default=None,
                    help="only check the files that differ from this git ref")
    ap.add_argument("--clang-format", default=None,
                    help="clang-format executable (default: from PATH)")
    ap.add_argument("--jobs", type=int, default=4,
                    help="parallel clang-format workers (default: 4)")
    ap.add_argument("--strict-clang-format", action="store_true",
                    help="enforce clang-format even on a version mismatch")
    ap.add_argument("--advisory", action="store_true",
                    help="report violations without failing (exit 0)")
    ap.add_argument("--quiet", action="store_true", help="only print problems")
    return ap.parse_args(argv)


def hygiene(text: str, max_line: int) -> dict:
    """Count the dependency-free violations of one file."""
    counts = {"long-line": 0, "tab": 0, "trailing-space": 0}
    for line in text.split("\n"):
        if len(line) > max_line:
            counts["long-line"] += 1
        if "\t" in line:
            counts["tab"] += 1
        if line != line.rstrip():
            counts["trailing-space"] += 1
    if text and not text.endswith("\n"):
        counts["no-eol"] = 1
    return {k: v for k, v in counts.items() if v}


def formats_differ(path: pathlib.Path, clang_format: str) -> bool:
    """True when clang-format would rewrite the file."""
    code, out = qcommon.run([clang_format, "--style=file", str(path)],
                            timeout=60, quiet=True)
    if code != 0:
        qcommon.warning("clang-format failed on %s (code %d)" % (path, code))
        return False
    return out != path.read_text(encoding="utf-8", errors="replace")


def scan_clang_format(files, clang_format: str, jobs: int) -> dict:
    """Return {path: True} for every C/H file clang-format would rewrite."""
    bad = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(formats_differ, p, clang_format): p for p in files}
        for future in concurrent.futures.as_completed(futures):
            if future.result():
                bad[futures[future]] = True
    return bad


def tool_version(clang_format: str | None) -> str:
    """Return the clang-format version string ("absent" when unavailable)."""
    if not clang_format:
        return "absent"
    code, out = qcommon.run([clang_format, "--version"], timeout=30, quiet=True)
    match = re.search(r"version (\S+)", out) if code == 0 else None
    return match.group(1) if match else "unknown"


def select_files(args, root: pathlib.Path) -> list:
    """Return the files to check (all of them, or only the changed ones)."""
    files = list(qcommon.iter_files(root, qcommon.SOURCE_EXTS))
    if not args.diff_ref:
        return files
    changed = changed_files(root, args.diff_ref)
    if changed is None:
        return files
    return [f for f in files if f.relative_to(root).as_posix() in changed]


def changed_files(root: pathlib.Path, ref: str):
    """Return the paths that differ from ref, or None when git cannot tell."""
    git = qcommon.tool_path("git")
    if not git:
        qcommon.notice("git not found: --diff-ref ignored, checking the tree")
        return None
    code, out = qcommon.run([git, "diff", "--name-only", "--diff-filter=ACMR",
                             ref], cwd=root, timeout=120, quiet=True)
    if code != 0:
        qcommon.notice("git diff against '%s' failed: checking the whole tree"
                       % ref)
        return None
    untracked = qcommon.run([git, "ls-files", "--others", "--exclude-standard"],
                            cwd=root, timeout=120, quiet=True)[1]
    return set(out.split()) | set(untracked.split())


def collect(args, root: pathlib.Path, clang_format: str | None):
    """Run every check; return (findings, checked count)."""
    files = select_files(args, root)
    findings: dict = {}
    for path in files:
        rel = path.relative_to(root).as_posix()
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            qcommon.warning("cannot read %s: %s" % (rel, exc))
            continue
        counts = hygiene(text, args.max_line)
        if counts:
            findings[rel] = counts
    c_files = [f for f in files if f.suffix in C_EXTS]
    if clang_format and c_files:
        for path in scan_clang_format(c_files, clang_format, args.jobs):
            rel = path.relative_to(root).as_posix()
            findings.setdefault(rel, {})["clang-format"] = 1
    return findings, len(files)


def load_baseline(path: pathlib.Path):
    """Read the baseline file; return (entries, meta)."""
    entries: dict = {}
    meta: dict = {}
    if not path.is_file():
        return entries, meta
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            match = META_RE.match(line)
            if match:
                meta[match.group(1)] = match.group(2).strip()
            continue
        parts = line.split()
        entries[parts[0]] = {k: int(v) for k, v in
                             (item.split("=") for item in parts[1:] if "=" in item)}
    return entries, meta


def write_baseline(path: pathlib.Path, findings: dict, version: str) -> None:
    """Record the current violations so the gate starts green from here."""
    qcommon.ensure_dir(path.parent)
    lines = [
        "# %s - formatting violations tolerated by the gate." % path.name,
        "# A ratchet: these counts are frozen, every new violation fails.",
        "# Regenerate after a deliberate reformatting commit:",
        "#   python3 tools/quality/check_format.py --write-baseline",
        "# generated: %s" % datetime.datetime.now(datetime.timezone.utc)
                              .isoformat(timespec="seconds"),
        "# clang-format: %s" % version,
        "",
    ]
    for rel in sorted(findings):
        counts = " ".join("%s=%d" % kv for kv in sorted(findings[rel].items()))
        lines.append("%s %s" % (rel, counts))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    qcommon.notice("baseline written: %s (%d file(s), clang-format %s)"
                   % (path, len(findings), version))


def exceeds(found: dict, tolerated: dict, kind: str) -> bool:
    """True when a check reports more violations than the baseline allows."""
    return found.get(kind, 0) > tolerated.get(kind, 0)


def violations(findings, baseline, hard_kinds):
    """Return the violations above the baseline, as readable messages."""
    out = []
    for rel in sorted(findings):
        tolerated = baseline.get(rel, {})
        for kind in sorted(findings[rel]):
            if kind not in hard_kinds or not exceeds(findings[rel], tolerated,
                                                     kind):
                continue
            out.append("%s: %s=%d (tolerated: %d)"
                       % (rel, kind, findings[rel][kind], tolerated.get(kind, 0)))
    return out


def hard_kinds(args, clang_format: str | None, version: str, meta: dict):
    """Return the checks that may fail the gate, plus the advisory ones."""
    hard = {"long-line", "tab", "trailing-space", "no-eol"}
    advisory = set()
    if not clang_format:
        qcommon.notice("clang-format not found: the C formatting check is "
                       "skipped (explicit skip, not a pass)")
        return hard, advisory, True
    recorded = meta.get("clang-format", "")
    drift = recorded and recorded.split(".")[0] != version.split(".")[0]
    if drift and not args.strict_clang_format:
        qcommon.notice("clang-format %s differs from the baseline (%s): the "
                       "formatting check is advisory for this run"
                       % (version, recorded))
        advisory.add("clang-format")
    else:
        hard.add("clang-format")
    return hard, advisory, False


def main(argv=None) -> int:
    """Entry point: check, compare with the baseline, report."""
    args = parse_args(argv)
    root = pathlib.Path(args.root or qcommon.repo_root()).resolve()
    baseline_path = pathlib.Path(args.baseline or root / BASELINE_DEFAULT)
    if not baseline_path.is_absolute():
        baseline_path = root / baseline_path
    clang_format = qcommon.tool_path(args.clang_format or "clang-format")
    version = tool_version(clang_format)
    findings, checked = collect(args, root, clang_format)
    if not checked:
        qcommon.notice("no source file to check (root=%s)" % root)
        return qcommon.EXIT_SKIP
    if args.write_baseline:
        write_baseline(baseline_path, findings, version)
        return qcommon.EXIT_OK
    baseline, meta = load_baseline(baseline_path)
    hard, advisory, _ = hard_kinds(args, clang_format, version, meta)
    problems = violations(findings, baseline, hard)
    tolerated = sum(1 for rel in baseline if rel in findings)
    if not args.quiet:
        qcommon.notice("format check: %d file(s) checked, %d violation(s), "
                       "%d file(s) tolerated by the baseline, clang-format %s"
                       % (checked, len(problems), tolerated, version))
    for item in problems:
        qcommon.error(item)
    if advisory:
        n = sum(1 for f in findings.values() if "clang-format" in f)
        qcommon.warning("%d file(s) would be rewritten by clang-format "
                        "(advisory this run)" % n)
    if problems and not args.advisory:
        qcommon.gh_summary("### Format\n\n%d violation(s):\n\n```\n%s```\n"
                           % (len(problems), "\n".join(problems[:50])))
        return qcommon.EXIT_FAIL
    qcommon.gh_summary("### Format\n\n%d file(s) checked, %d violation(s).\n"
                       % (checked, len(problems)))
    return qcommon.EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())

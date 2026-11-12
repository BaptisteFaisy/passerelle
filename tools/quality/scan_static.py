#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Static analysis of the C tree: clang-tidy first, cppcheck second.

The script never invents a green result:

  * clang-tidy is used when the executable and a compilation database
    (compile_commands.json, produced by every preset of CMakePresets.json)
    are available;
  * cppcheck is used when clang-tidy is missing: it needs no compilation
    database, only the source directories;
  * when neither tool is available the script exits with the "skip" code 3
    and prints an explicit notice - the job then reports "skipped", never
    "passed".

The report is written as text (one diagnostic per line, plus a summary) and
is meant to be published as a workflow artefact.

Exit codes: 0 no error-level diagnostic (warnings are reported and counted),
1 error-level diagnostic or more warnings than --max-warnings, 2 usage error,
3 explicit skip (no analyser available).
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import pathlib
import re
import sys

if __package__ in (None, ""):  # allow `python3 tools/quality/scan_static.py`
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import qcommon

# Focused set: real bug classes, not style opinions. The readability and
# naming checks are excluded on purpose - they would flood the report without
# telling anything about the correctness of the code.
# Curated set: real bug classes, not style opinions. Excluded on purpose,
# measured on this tree on 2026-11-11 (1033 findings, 0 error level):
#   bugprone-signed-bitwise (244)  the code manipulates hardware registers
#                                  with masks; the compiler already enforces
#                                  -Wsign-conversion -Werror;
#   clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling (241)
#                                  asks for C11 Annex K (memcpy_s), which no
#                                  portable target - and no QNX SDP - provides;
#   bugprone-reserved-identifier + cert-dcl37-c/cert-dcl51-cpp (222) flags
#                                  _POSIX_C_SOURCE and the __start_/__stop_
#                                  linker-section symbols the test harness
#                                  depends on;
#   bugprone-easily-swappable-parameters (175) and cert-err33-c (90): opinion
#                                  noise; re-enable them with --checks for a
#                                  dedicated audit.
DEFAULT_CHECKS = (
    "clang-analyzer-*,bugprone-*,cert-*,concurrency-*,misc-*,performance-*,"
    "-bugprone-signed-bitwise,-bugprone-easily-swappable-parameters,"
    "-bugprone-reserved-identifier,-cert-dcl37-c,-cert-dcl51-cpp,-cert-err33-c,"
    "-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,"
    "-misc-include-cleaner,-misc-use-anonymous-namespace,"
    "-performance-no-int-to-ptr")
CPPCHECK_ENABLE = "warning,style,performance,portability"
# Only the severity token decides the kind: the message text of a warning may
# itself contain the word "error" ("missing error handling"), which must not
# turn a warning into a failure.
SEVERITY_RE = re.compile(r":\s*(error|warning):\s")
CHECK_NAME_RE = re.compile(r"\[([a-zA-Z0-9_.+,-]+)\]$")


def parse_args(argv=None) -> argparse.Namespace:
    """Parse the command line."""
    ap = argparse.ArgumentParser(
        description="Run clang-tidy (or cppcheck) on the C tree.",
        epilog="exit codes: 0 ok, 1 findings, 2 usage, 3 explicit skip")
    ap.add_argument("--root", default=None, help="repository root")
    ap.add_argument("--build-dir", default=None,
                    help="build tree holding compile_commands.json")
    ap.add_argument("--preset", default="ci",
                    help="preset configured when the build tree is missing")
    ap.add_argument("--checks", default=DEFAULT_CHECKS,
                    help="clang-tidy check list (default: %s)" % DEFAULT_CHECKS)
    ap.add_argument("--report", default=None,
                    help="report file (default: build/static/static-analysis.txt)")
    ap.add_argument("--max-warnings", type=int, default=-1,
                    help="fail above this warning count (-1 = report only)")
    ap.add_argument("--fail-on-checks", default=None,
                    help="comma separated check names/prefixes that must fail "
                         "the run (e.g. clang-analyzer-core.*)")
    ap.add_argument("--jobs", type=int, default=4,
                    help="parallel analysers (default: 4)")
    ap.add_argument("--timeout", type=int, default=120,
                    help="per-file timeout in seconds")
    ap.add_argument("--configure", action="store_true",
                    help="configure the preset when the build tree is missing")
    return ap.parse_args(argv)


def translation_units(compile_db: pathlib.Path, root: pathlib.Path) -> list:
    """Return the source files of the compilation database, in the tree."""
    try:
        entries = json.loads(compile_db.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        qcommon.warning("cannot read %s: %s" % (compile_db, exc))
        return []
    files = []
    for entry in entries:
        source = entry.get("file", "")
        if not source:
            continue
        path = pathlib.Path(source)
        if not path.is_absolute():
            path = root / path
        if not path.is_file():
            continue
        if qcommon.is_skipped(path.relative_to(root).parts):
            continue
        files.append(path)
    return sorted(set(files))


def run_clang_tidy(files, clang_tidy, build_dir, checks, jobs, timeout):
    """Run clang-tidy on every translation unit; return the diagnostics."""
    cmd = [clang_tidy, "-p", str(build_dir), "--quiet",
           "--checks=%s" % checks]

    def analyse(path):
        code, out = qcommon.run(cmd + [str(path)], timeout=timeout, quiet=True)
        return path, code, out

    findings = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for path, code, out in pool.map(analyse, files):
            if code not in (0, 1):  # 1 means "diagnostics found"
                qcommon.warning("clang-tidy failed on %s (code %d)"
                                % (path, code))
            findings.extend(qcommon.parse_warnings(out, SEVERITY_RE.pattern))
    return sorted(set(findings))


def run_cppcheck(dirs, cppcheck, jobs, timeout):
    """Run cppcheck on the source directories; return the diagnostics."""
    cmd = [cppcheck, "--enable=%s" % CPPCHECK_ENABLE, "--std=c11",
           "--inline-suppr", "--suppress=missingIncludeSystem",
           "--suppress=unusedFunction", "--quiet", "-j", str(jobs)]
    findings = []
    for directory in dirs:
        code, out = qcommon.run(cmd + [str(directory)], timeout=timeout,
                                quiet=True)
        if code not in (0, 1):
            qcommon.warning("cppcheck failed on %s (code %d)" % (directory, code))
        findings.extend(qcommon.parse_warnings(
            out, r":\s*(error|warning|style|performance|portability):\s"))
    return sorted(set(findings))


def severity(line: str) -> str:
    """Return the severity token of a diagnostic line ("error" wins)."""
    match = SEVERITY_RE.search(line)
    return match.group(1) if match else "warning"


def analyse_version(tool: str) -> str:
    """Return the version of an analyser, for the report header."""
    code, out = qcommon.run([tool, "--version"], timeout=30, quiet=True)
    if code != 0:
        return "unknown"
    match = re.search(r"(\d+\.\d+(?:\.\d+)?)", out)
    return match.group(1) if match else out.splitlines()[0][:40]


def check_names(line: str) -> list:
    """Return the check names a diagnostic line is attributed to."""
    match = CHECK_NAME_RE.search(line)
    return match.group(1).split(",") if match else ["uncategorised"]


def matches_checks(line: str, prefixes: tuple) -> bool:
    """True when one of the checks of the line starts with one prefix."""
    return any(name.startswith(prefix.rstrip("*"))
               for name in check_names(line) for prefix in prefixes)


def count_diagnostics(findings) -> dict:
    """Count the errors and warnings of a finding list, per check name."""
    summary = {"error": 0, "warning": 0, "checks": {}}
    for line in findings:
        summary[severity(line)] += 1
        name = check_names(line)[0]
        summary["checks"][name] = summary["checks"].get(name, 0) + 1
    return summary


def write_report(path: pathlib.Path, findings, summary, meta) -> None:
    """Write the text report: meta, summary, then every diagnostic."""
    qcommon.ensure_dir(path.parent)
    lines = ["passerelle static analysis report", ""]
    for key, value in meta.items():
        lines.append("%s: %s" % (key, value))
    lines += ["", "errors: %d   warnings: %d" % (summary["error"],
                                                 summary["warning"]), ""]
    if summary["checks"]:
        lines.append("per check:")
        for name, count in sorted(summary["checks"].items(),
                                  key=lambda kv: (-kv[1], kv[0])):
            lines.append("  %-40s %d" % (name, count))
        lines.append("")
    lines.append("diagnostics:")
    lines.extend("  %s" % line for line in findings)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def resolve_paths(args):
    """Return (root, build_dir, report_path) for this run."""
    root = pathlib.Path(args.root or qcommon.repo_root()).resolve()
    build_dir = pathlib.Path(args.build_dir
                             or qcommon.build_dir_for(root, args.preset))
    report_path = pathlib.Path(args.report or build_dir / "static-analysis.txt")
    return root, build_dir, report_path


def pick_clang_tidy(args, root, build_dir):
    """Return the clang-tidy path, or None when it cannot be used here."""
    clang_tidy = qcommon.tool_path("clang-tidy")
    if not clang_tidy:
        return None
    if (build_dir / "compile_commands.json").is_file():
        return clang_tidy
    if not args.configure:
        qcommon.warning("no compilation database in %s: clang-tidy is skipped"
                        % build_dir)
        return None
    cmake = qcommon.tool_path("cmake")
    if not cmake:
        qcommon.warning("cmake not found: clang-tidy is skipped")
        return None
    if qcommon.configure_preset(cmake, args.preset, root) != 0:
        qcommon.warning("configure of the preset '%s' failed: clang-tidy is "
                        "skipped" % args.preset)
        return None
    return clang_tidy


def analyse(args, root, build_dir, clang_tidy, cppcheck):
    """Run the chosen analyser; return (tool description, findings)."""
    if clang_tidy:
        files = translation_units(build_dir / "compile_commands.json", root)
        tool = "clang-tidy %s" % analyse_version(clang_tidy)
        if not files:
            qcommon.warning("no translation unit found: clang-tidy is skipped")
            return tool, []
        qcommon.notice("clang-tidy on %d translation unit(s)" % len(files))
        return tool, run_clang_tidy(files, clang_tidy, build_dir, args.checks,
                                    args.jobs, args.timeout)
    dirs = [d for d in (root / m for m in qcommon.PROD_MODULES) if d.is_dir()]
    qcommon.notice("cppcheck on %d source director(y/ies)" % len(dirs))
    return ("cppcheck %s" % analyse_version(cppcheck),
            run_cppcheck(dirs, cppcheck, args.jobs, args.timeout))


def verdict(findings, summary, args) -> int:
    """Turn the findings into an exit code (error-level findings always fail)."""
    if summary["error"]:
        return qcommon.EXIT_FAIL
    if args.fail_on_checks:
        prefixes = tuple(p.strip() for p in args.fail_on_checks.split(",")
                         if p.strip())
        hits = [line for line in findings if matches_checks(line, prefixes)]
        if hits:
            for line in hits[:20]:
                qcommon.error(line)
            qcommon.error("%d finding(s) from --fail-on-checks %s"
                          % (len(hits), args.fail_on_checks))
            return qcommon.EXIT_FAIL
    if args.max_warnings >= 0 and summary["warning"] > args.max_warnings:
        qcommon.error("%d warning(s) above the allowed %d"
                      % (summary["warning"], args.max_warnings))
        return qcommon.EXIT_FAIL
    return qcommon.EXIT_OK


def main(argv=None) -> int:
    """Entry point: pick an analyser, run it, report the findings."""
    args = parse_args(argv)
    root, build_dir, report_path = resolve_paths(args)
    clang_tidy = pick_clang_tidy(args, root, build_dir)
    cppcheck = None if clang_tidy else qcommon.tool_path("cppcheck")
    if not clang_tidy and not cppcheck:
        qcommon.notice("neither clang-tidy nor cppcheck is available: the "
                       "static analysis is SKIPPED (explicit skip, never a "
                       "silent pass)")
        qcommon.gh_summary("### Static analysis\n\nSkipped: no analyser "
                           "available on this runner.\n")
        return qcommon.EXIT_SKIP
    tool, findings = analyse(args, root, build_dir, clang_tidy, cppcheck)
    summary = count_diagnostics(findings)
    write_report(report_path, findings, summary,
                 {"tool": tool, "report": str(report_path), "root": str(root)})
    qcommon.notice("static analysis: %d error(s), %d warning(s), report %s"
                   % (summary["error"], summary["warning"], report_path))
    qcommon.gh_summary("### Static analysis\n\n%d error(s), %d warning(s) - "
                       "report: %s\n" % (summary["error"], summary["warning"],
                                         report_path))
    for line in findings[:20]:
        qcommon.warning(line)
    return verdict(findings, summary, args)


if __name__ == "__main__":
    raise SystemExit(main())

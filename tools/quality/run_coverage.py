#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the tree with coverage instrumentation, run the tests, report.

One command does the whole chain so a local run and the CI job are identical:

    python3 tools/quality/run_coverage.py            # preset `coverage`
    python3 tools/quality/run_coverage.py --help

Steps (each one bounded by a timeout, none of them unbounded):
  1. configure   cmake --preset coverage, or the equivalent explicit flags
                 when the installed cmake does not know presets (< 3.21);
  2. build       cmake --build (instrumented: -fprofile-arcs -ftest-coverage);
  3. test        ctest, without the `rules` label (that guard belongs to
                 .github/workflows/ci.yml, not to a coverage measurement);
  4. collect     gcov --json-format on every .gcda (no lcov, no gcovr);
  5. report      summary.txt, badge.svg, coverage.xml, index.html and the
                 build.log of steps 1-3, all inside the build directory,
                 ready to be uploaded as a workflow artefact.

Only the C stack is measured. The Python host tools are not instrumented:
the project rule is "standard library only", and Python coverage needs the
third-party `coverage` package. That limit is stated in the report itself.

Exit codes: 0 ok, 1 failure (configure/build/test/gcov), 2 usage,
3 explicit skip (cmake or gcov missing) - a skip is never a green build.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import sys

if __package__ in (None, ""):  # allow `python3 tools/quality/run_coverage.py`
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import covcollect
import covreport
import qcommon

DEFAULT_MODULES = "common;virtio;proto9p;protofuse;rmfs;drivers;tests"
# -Og keeps the line numbers meaningful (unlike -O2, which inlines and moves
# code around) while staying fast enough for the campaigns.
COVERAGE_C_FLAGS = "-Og -g -fprofile-arcs -ftest-coverage -fprofile-abs-path"
COVERAGE_LINK_FLAGS = "--coverage"
BUILD_LOG = "build.log"


def parse_args(argv=None) -> argparse.Namespace:
    """Parse the command line (see the module docstring for the behaviour)."""
    ap = argparse.ArgumentParser(
        description="Instrumented build + ctest + gcov coverage report.",
        epilog="exit codes: 0 ok, 1 failure, 2 usage, 3 explicit skip")
    ap.add_argument("--source-dir", default=None, help="repository root")
    ap.add_argument("--preset", default="coverage",
                    help="CMake configure/build preset (default: coverage)")
    ap.add_argument("--build-dir", default=None,
                    help="build tree (default: build/<preset>)")
    ap.add_argument("--report-dir", default=None,
                    help="where the reports go (default: the build tree)")
    ap.add_argument("--modules", default=DEFAULT_MODULES,
                    help="PGW_MODULES passed to the fallback configure")
    ap.add_argument("--jobs", type=int, default=0,
                    help="parallel build jobs (0 = auto, capped at 8)")
    ap.add_argument("--werror", default="OFF", choices=("ON", "OFF"),
                    help="PGW_WERROR for the instrumented build (default OFF: "
                         "the warning gate lives in ci.yml)")
    ap.add_argument("--gcov", default=None,
                    help="gcov executable (default: gcov, then gcov-<gcc major>)")
    ap.add_argument("--ctest-label-exclude", default="rules",
                    help="ctest -LE value (empty to run everything)")
    ap.add_argument("--configure-timeout", type=int, default=300)
    ap.add_argument("--build-timeout", type=int, default=900)
    ap.add_argument("--ctest-timeout", type=int, default=600)
    ap.add_argument("--gcov-timeout", type=int, default=60)
    ap.add_argument("--min-coverage", type=float, default=0.0,
                    help="fail below this production line coverage percentage")
    ap.add_argument("--skip-build", action="store_true",
                    help="reuse an existing instrumented build tree")
    ap.add_argument("--ignore-test-failures", action="store_true",
                    help="report coverage even when ctest fails (local runs)")
    return ap.parse_args(argv)


def resolve_gcov(requested: str | None) -> str | None:
    """Find gcov, falling back to the versioned binary matching gcc."""
    if requested:
        return qcommon.tool_path(requested)
    found = qcommon.tool_path("gcov")
    if found:
        return found
    gcc = qcommon.tool_path("gcc") or qcommon.tool_path("cc")
    if not gcc:
        return None
    code, out = qcommon.run([gcc, "-dumpversion"], quiet=True, timeout=30)
    major = re.match(r"\d+", out.strip()) if code == 0 else None
    return qcommon.tool_path("gcov-%s" % major.group(0)) if major else None


def tool_versions(tools: dict) -> dict:
    """Return {tool: version} for the report header."""
    versions = {}
    for name, path in tools.items():
        if not path:
            versions[name] = "missing"
            continue
        code, out = qcommon.run([path, "--version"], quiet=True, timeout=30)
        first = out.splitlines()[0] if out else "?"
        versions[name] = first.split()[-1] if code == 0 else "?"
    return versions


def configure(args, paths, log) -> int:
    """Configure the instrumented build tree; return the exit code."""
    if args.preset:
        code, _ = qcommon.run([paths["cmake"], "--preset", args.preset],
                              cwd=args.source_dir, timeout=args.configure_timeout,
                              log=log)
        if code == 0:
            return 0
        qcommon.warning("cmake --preset %s failed (code %d): falling back to "
                        "explicit flags" % (args.preset, code))
    cmd = [paths["cmake"], "-S", str(args.source_dir), "-B", str(args.build_dir),
           "-DCMAKE_BUILD_TYPE=Debug", "-DPGW_MODULES=%s" % args.modules,
           "-DPGW_WERROR=%s" % args.werror,
           "-DCMAKE_C_FLAGS=%s" % COVERAGE_C_FLAGS,
           "-DCMAKE_EXE_LINKER_FLAGS=%s" % COVERAGE_LINK_FLAGS,
           "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
    code, _ = qcommon.run(cmd, cwd=args.source_dir,
                          timeout=args.configure_timeout, log=log)
    return code


def build(args, paths, log) -> int:
    """Build the instrumented tree; return the exit code."""
    jobs = args.jobs or min(os.cpu_count() or 2, 8)
    if args.preset:
        code, out = qcommon.run([paths["cmake"], "--build", "--preset",
                                 args.preset, "-j", jobs],
                                cwd=args.source_dir, timeout=args.build_timeout,
                                log=log)
        if code == 0 or "no such preset" not in out.lower():
            return code
        qcommon.warning("no build preset '%s': building the tree directly"
                        % args.preset)
    code, _ = qcommon.run([paths["cmake"], "--build", str(args.build_dir),
                           "-j", jobs], cwd=args.source_dir,
                          timeout=args.build_timeout, log=log)
    return code


def run_tests(args, paths, log) -> int:
    """Run ctest on the instrumented tree; return the exit code."""
    cmd = [paths["ctest"], "--test-dir", str(args.build_dir),
           "--output-on-failure", "--no-tests=error",
           "--timeout", str(args.ctest_timeout)]
    if args.ctest_label_exclude:
        cmd += ["-LE", args.ctest_label_exclude]
    code, _ = qcommon.run(cmd, cwd=args.source_dir, timeout=args.ctest_timeout
                          + 120, log=log)
    return code


def resolve_dirs(args) -> None:
    """Fill in the default source, build and report directories."""
    args.source_dir = pathlib.Path(args.source_dir or qcommon.repo_root())
    if args.build_dir is None:
        args.build_dir = args.source_dir / "build" / (args.preset or "coverage")
    args.build_dir = pathlib.Path(args.build_dir)
    args.report_dir = pathlib.Path(args.report_dir or args.build_dir)


def main(argv=None) -> int:
    """Entry point: configure, build, test, collect, report."""
    args = parse_args(argv)
    resolve_dirs(args)
    paths = {"cmake": qcommon.tool_path("cmake"),
             "ctest": qcommon.tool_path("ctest"),
             "gcov": resolve_gcov(args.gcov)}
    if not paths["cmake"] or not paths["ctest"]:
        qcommon.error("cmake/ctest not found - coverage measurement skipped")
        return qcommon.EXIT_SKIP
    if not paths["gcov"]:
        qcommon.notice("gcov not found - coverage measurement skipped "
                       "(explicit skip, never a silent green)")
        return qcommon.EXIT_SKIP
    qcommon.ensure_dir(args.report_dir)
    args.build_dir.mkdir(parents=True, exist_ok=True)
    log = args.build_dir / BUILD_LOG
    log.write_text("passerelle coverage run\n", encoding="utf-8")
    versions = tool_versions(paths)
    if not args.skip_build:
        code = configure(args, paths, log)
        if code != 0:
            qcommon.error("configure failed (code %d) - see %s" % (code, log))
            return qcommon.EXIT_FAIL
        code = build(args, paths, log)
        if code != 0:
            qcommon.error("build failed (code %d) - see %s" % (code, log))
            return qcommon.EXIT_FAIL
    code = run_tests(args, paths, log)
    if code != 0 and not args.ignore_test_failures:
        qcommon.error("ctest failed (code %d) - see %s" % (code, log))
        return qcommon.EXIT_FAIL
    if code != 0:
        qcommon.warning("ctest failed (code %d): coverage reported anyway "
                        "(--ignore-test-failures)" % code)
    return report(args, paths, versions, log)


def report(args, paths, versions, log) -> int:
    """Collect the gcov data, write the artefacts and gate on the score."""
    agg, counts = covcollect.collect(args.build_dir, args.source_dir,
                                    paths["gcov"], args.gcov_timeout, log)
    print("gcov: %(gcda)d data file(s), %(failed)d failure(s), "
          "%(sources)d source(s) measured" % counts)
    if not agg:
        qcommon.error("no coverage data collected (no .gcda under %s)"
                      % args.build_dir)
        return qcommon.EXIT_FAIL
    stats = covreport.file_stats(agg)
    prod = covreport.write_summary(args.report_dir / "summary.txt", stats,
                                   counts, versions)
    covreport.write_cobertura(args.report_dir / "coverage.xml", agg, stats)
    covreport.write_html(args.report_dir / "index.html", stats, counts)
    text = (args.report_dir / "summary.txt").read_text(encoding="utf-8")
    print(text)
    qcommon.gh_summary("### Coverage\n\n```\n%s```\n" % text)
    qcommon.notice("coverage: %.2f%% of production lines (%d/%d), reports in %s"
                   % (prod["pct"], prod["covered"], prod["lines"],
                      args.report_dir))
    if args.min_coverage and prod["pct"] < args.min_coverage:
        qcommon.error("coverage %.2f%% below the required %.2f%%"
                      % (prod["pct"], args.min_coverage))
        return qcommon.EXIT_FAIL
    return qcommon.EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())

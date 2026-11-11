#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check code-size rules: every source file < 500 lines, every function < 50 lines."""
from __future__ import annotations

import argparse
import pathlib
import sys

C_EXTS = {".c", ".h"}
PY_EXTS = {".py"}
# Any directory whose name starts with "build" is a build tree (per-module
# build-<module> and build-<module>-asan); plus the usual noise directories.
SKIP_DIR_PREFIX = ("build",)
SKIP_DIRS = {".git", "__pycache__", ".venv", "venv", ".pytest_cache", "node_modules"}

# Vendored CI socle, maintained centrally in ~/Bureau/hermes-port and copied
# verbatim into every repository of this account. Refactoring it here would
# fork the socle, so it is excluded from the project's own size rules; the
# exclusion is deliberate and narrow (two directories, both CI-only).
SKIP_PREFIXES = (".github/hermes/", ".github/ci-budget/")


def c_func_violations(lines: list[str], path: pathlib.Path, max_fn: int) -> list[str]:
    issues: list[str] = []
    depth = 0
    in_fn = False
    fn_start = 0
    fn_name = ""
    prev_sig = ""
    in_block_comment = False

    for idx, raw in enumerate(lines, 1):
        s = raw.strip()
        if in_block_comment:
            if "*/" in s:
                in_block_comment = False
            continue
        if s.startswith("/*"):
            if "*/" not in s:
                in_block_comment = True
            continue
        if s.startswith("//") or s.startswith("#"):
            continue

        opens = s.count("{")
        closes = s.count("}")

        if depth == 0 and opens > 0 and not in_fn:
            cand = prev_sig if s == "{" else s.split("{", 1)[0].strip()
            is_type = any(k in cand for k in ("struct ", "enum ", "union ", "typedef "))
            if "(" in cand and not is_type and not cand.endswith("="):
                in_fn = True
                fn_start = idx
                fn_name = cand[:60]

        if depth == 0 and opens == 0 and s:
            prev_sig = s if not s.endswith(";") else ""

        depth += opens - closes
        if depth < 0:
            depth = 0

        if in_fn and depth == 0:
            span = idx - fn_start + 1
            if span >= max_fn:
                issues.append(f"{path}:{fn_start}: C function '{fn_name}' is {span} lines (>= {max_fn})")
            in_fn = False
    return issues


def py_func_violations(lines: list[str], path: pathlib.Path, max_fn: int) -> list[str]:
    issues: list[str] = []
    stack: list[tuple[int, int, str]] = []
    for idx, raw in enumerate(lines, 1):
        stripped = raw.lstrip(" ")
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(raw) - len(stripped)
        while stack and indent <= stack[-1][0]:
            _, start, name = stack.pop()
            span = idx - start
            if span >= max_fn:
                issues.append(f"{path}:{start}: Python function '{name}' is {span} lines (>= {max_fn})")
        if stripped.startswith("def ") or stripped.startswith("async def "):
            name = stripped.split("(", 1)[0].replace("async ", "").replace("def ", "").strip()
            stack.append((indent, idx, name))
    end_line = len(lines) + 1
    while stack:
        _, start, name = stack.pop()
        span = end_line - start
        if span >= max_fn:
            issues.append(f"{path}:{start}: Python function '{name}' is {span} lines (>= {max_fn})")
    return issues


def check_tree(root: pathlib.Path, max_file: int, max_fn: int) -> list[str]:
    issues: list[str] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or any(part in SKIP_DIRS for part in path.parts):
            continue
        if any(part.startswith(SKIP_DIR_PREFIX) for part in path.parts):
            continue
        ext = path.suffix
        if ext not in C_EXTS and ext not in PY_EXTS:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()
        rel = path.relative_to(root)
        if rel.as_posix().startswith(SKIP_PREFIXES):
            continue
        if len(lines) >= max_file:
            issues.append(f"{rel}: {len(lines)} lines (>= {max_file})")
        if ext == ".c":
            issues.extend(c_func_violations(lines, rel, max_fn))
        elif ext == ".py":
            issues.extend(py_func_violations(lines, rel, max_fn))
    return issues


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--max-file", type=int, default=500)
    ap.add_argument("--max-fn", type=int, default=50)
    args = ap.parse_args()
    issues = check_tree(pathlib.Path(args.root).resolve(), args.max_file, args.max_fn)
    if issues:
        for item in issues:
            print(f"SIZE VIOLATION: {item}", file=sys.stderr)
        return 1
    print("size-guard: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

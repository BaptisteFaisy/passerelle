#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate the requirement-to-proof traceability matrix (docs/traceability.md).

Why this tool exists
--------------------
The hackathon submission states a verification level for every requirement of
``docs/spec-en.md``. A matrix written by hand goes stale the day a test is
renamed or a file moves, so it is generated instead:

* identifiers and short statements come from the spec itself (the section 6
  tables) and are never retyped anywhere else;
* state, proof and note come from ``tools/traceability-map.yml``, whose
  restricted YAML format (no PyYAML) is documented in ``tools/traceability_map.py``;
* every cited file must exist, and every cited test must exist in the build:
  CTest names declared in the ``CMakeLists.txt`` files, ``PGW_TEST()`` cases of
  the C suites, and ``test_*`` functions of the pytest suites;
* a requirement with no entry is an error, and so is an entry that no
  requirement asks for (a phantom): the map is never allowed to be stale.

States
------
``tested``  the proof runs in the CI on the host PC, or is measured and published.
``partial`` part of the requirement is exercised; the rest needs a QNX guest (SDP).
``missing`` not started; extreme tier, de-scoped first when the schedule slips.

A ``tested`` entry must cite a test when the spec verifies the requirement by
test (``T``), and an evidence file when the spec verifies it by inspection,
measurement or demonstration (``I``, ``M``, ``D``). A ``partial`` entry must
say what is missing. A ``missing`` entry must cite no test at all.

Usage
-----
::

    python3 tools/traceability.py            # write docs/traceability.md
    python3 tools/traceability.py --print    # print it, write nothing
    python3 tools/traceability.py --check    # exit 1 when the copy differs

Exit status: 0 written or up to date, 1 the committed matrix is stale
(``--check``), 2 the map is inconsistent or malformed.

The output is deterministic: rows follow the order of the spec, and neither a
date nor a machine path appears in it, so ``--check`` is stable in CI.
"""

from __future__ import annotations

import argparse
import importlib.util
import pathlib
import re
import sys

_HERE = pathlib.Path(__file__).resolve().parent


def _load_sibling(name: str):
    """Load a module sitting next to this script, without touching sys.path.

    ``tools/`` is not a package, and adding it to ``sys.path`` would shadow
    standard-library names with the directories that live there, so the sibling
    is loaded by file path instead.
    """
    spec = importlib.util.spec_from_file_location(name, _HERE / ("%s.py" % name))
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


tm = _load_sibling("traceability_map")

# Public surface of the parser module, re-exported so a reader (and the test
# suite) only has to import this script.
Requirement = tm.Requirement
Entry = tm.Entry
ID_RE = tm.ID_RE
STATES = tm.STATES
parse_spec = tm.parse_spec
parse_map = tm.parse_map
test_catalog = tm.test_catalog

REPO_ROOT = _HERE.parent
STATE_MEANING = {
    "tested": "automated proof runs in the CI on the host PC, or measured and published",
    "partial": "code exists and part of it is exercised; the rest needs a QNX guest (SDP)",
    "missing": "not started; extreme tier, first item de-scoped when the schedule slips",
}
STATEMENT_LIMIT = 96


def _state_rules(entry: Entry, req: Requirement) -> list[str]:
    """Per-state consistency rules: a state must carry the proof it claims.

    The spec's verification column decides what counts as a proof: a
    requirement verified by test (``T``) must cite a test, one verified by
    inspection, measurement or demonstration (``I``, ``M``, ``D``) must cite
    the file that carries the evidence.
    """
    problems: list[str] = []
    by_test = "T" in req.verif
    if entry.state == "tested":
        if by_test and not entry.tests:
            problems.append("%s: state 'tested' cites no test, but the spec "
                            "verifies it by test (%s)" % (entry.rid, req.verif or "?"))
        if not by_test and not entry.files:
            problems.append("%s: state 'tested' cites no evidence file, but the "
                            "spec verifies it by %s" % (entry.rid, req.verif or "?"))
    elif entry.state == "partial":
        if not entry.files:
            problems.append("%s: state 'partial' cites no file" % entry.rid)
        if not entry.note:
            problems.append("%s: state 'partial' must say what is missing" % entry.rid)
    elif entry.tests:
        problems.append("%s: state 'missing' must cite no test (nothing runs yet)" % entry.rid)
    if not entry.note and entry.state != "tested":
        problems.append("%s: state '%s' needs a note" % (entry.rid, entry.state))
    return problems


def validate(requirements: list[Requirement], entries: dict[str, Entry],
             catalog: set[str], root: pathlib.Path) -> list[str]:
    """Return every inconsistency between spec, map, tests and files."""
    problems: list[str] = []
    known = {req.rid: req for req in requirements}
    for rid in sorted(set(known) - set(entries)):
        problems.append("%s: in the spec but absent from the map (map is stale)" % rid)
    for rid in sorted(set(entries) - set(known)):
        problems.append("%s: in the map but not a requirement of the spec (phantom)" % rid)
    for rid, entry in sorted(entries.items()):
        if rid in known:
            problems.extend(_state_rules(entry, known[rid]))
        for name in entry.tests:
            if name not in catalog:
                problems.append("%s: test '%s' exists in no CMakeLists.txt, "
                                "PGW_TEST() or pytest suite" % (rid, name))
        for name in entry.files:
            if not (root / name).is_file():
                problems.append("%s: file '%s' does not exist" % (rid, name))
    return problems


def shorten(text: str, limit: int = STATEMENT_LIMIT) -> str:
    """Collapse the statement to one line, cut on a word boundary."""
    flat = re.sub(r"\s+", " ", text).strip()
    if len(flat) <= limit:
        return flat
    return flat[:limit].rsplit(" ", 1)[0].rstrip(",;:") + "..."


def _cell(text: str) -> str:
    """Escape a value for a Markdown table cell."""
    return text.replace("|", "\\|") or "-"


def _tally(entries: dict[str, Entry]) -> dict[str, int]:
    """Count the entries per state (deterministic key order)."""
    return {state: sum(1 for e in entries.values() if e.state == state) for state in STATES}


def _row(req: Requirement, entry: Entry) -> str:
    """One Markdown row of the matrix."""
    return "| %s | %s | %s | %s | %s | %s |" % (
        req.rid, _cell(shorten(req.statement)), entry.state,
        _cell(", ".join("`%s`" % t for t in entry.tests)),
        _cell(", ".join("`%s`" % f for f in entry.files)),
        _cell(entry.note))


def _header(count: int, tally: dict[str, int]) -> list[str]:
    """The generated preamble: provenance, tally and the state vocabulary."""
    lines = [
        "# Requirement traceability",
        "",
        "> **Generated file - do not edit.** `python3 tools/traceability.py` builds it",
        "> from `docs/spec-en.md` (identifiers and statements) and",
        "> `tools/traceability-map.yml` (state, proof, note). CI re-runs the generator",
        "> with `--check`, so a stale map fails the build instead of lying to a reader.",
        "",
        "%d requirements: %s." % (count, ", ".join(
            "%d `%s`" % (tally[state], state) for state in STATES)),
        "",
        "## States",
        "",
        "| State | Meaning |",
        "|---|---|",
    ]
    return lines + ["| `%s` | %s |" % (state, STATE_MEANING[state]) for state in STATES]


def render(requirements: list[Requirement], entries: dict[str, Entry]) -> str:
    """Build the whole generated document (no date, no path: deterministic)."""
    tally = _tally(entries)
    lines = _header(len(requirements), tally)
    lines += ["", "## Matrix", "",
              "| ID | Requirement (spec-en.md section 6) | State | Proof (test) | Files | Note |",
              "|---|---|---|---|---|---|"]
    lines += [_row(req, entries[req.rid]) for req in requirements if req.rid in entries]
    lines += ["", "## Tally", "", "| State | Count |", "|---|---:|"]
    lines += ["| `%s` | %d |" % (state, tally[state]) for state in STATES]
    lines += ["| total | %d |" % len(requirements), ""]
    return "\n".join(lines)


def _first_difference(current: str, expected: str) -> str:
    """Describe where the committed copy starts to differ (for --check)."""
    old, new = current.splitlines(), expected.splitlines()
    for index in range(max(len(old), len(new))):
        before = old[index] if index < len(old) else "<missing>"
        after = new[index] if index < len(new) else "<missing>"
        if before != after:
            return "line %d:\n  committed: %s\n  generated: %s" % (index + 1, before, after)
    return "no textual difference"


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Command line: default writes, --print shows, --check compares."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(REPO_ROOT), help="repository root")
    parser.add_argument("--spec", default="", help="spec (default: <root>/docs/spec-en.md)")
    parser.add_argument("--map", dest="map_path", default="",
                        help="map (default: <root>/tools/traceability-map.yml)")
    parser.add_argument("--out", default="",
                        help="output (default: <root>/docs/traceability.md)")
    parser.add_argument("--print", dest="to_stdout", action="store_true",
                        help="print the document instead of writing it")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 when the written document would change")
    return parser.parse_args(argv)


def _paths(args: argparse.Namespace) -> tuple[pathlib.Path, pathlib.Path,
                                              pathlib.Path, pathlib.Path]:
    """Resolve root, spec, map and output paths from the command line."""
    root = pathlib.Path(args.root).resolve()
    spec = pathlib.Path(args.spec) if args.spec else root / "docs" / "spec-en.md"
    map_path = (pathlib.Path(args.map_path) if args.map_path
                else root / "tools" / "traceability-map.yml")
    out = pathlib.Path(args.out) if args.out else root / "docs" / "traceability.md"
    return root, spec, map_path, out


def _summary(count: int, tally: dict[str, int]) -> str:
    """One line telling how many requirements fall in each state."""
    return "%d requirements (%s)" % (
        count, ", ".join("%d %s" % (tally[state], state) for state in STATES))


def main(argv: list[str] | None = None) -> int:
    """Validate the map, then write / print / compare the matrix."""
    args = parse_args(argv)
    root, spec, map_path, out = _paths(args)
    requirements = parse_spec(spec)
    try:
        entries = parse_map(map_path)
    except ValueError as error:
        print("traceability: MAP ERROR: %s" % error, file=sys.stderr)
        return 2
    problems = validate(requirements, entries, test_catalog(root), root)
    for problem in problems:
        print("traceability: ERROR: %s" % problem, file=sys.stderr)
    if problems:
        print("traceability: %d problem(s); the map is stale or dishonest"
              % len(problems), file=sys.stderr)
        return 2
    text = render(requirements, entries)
    summary = _summary(len(requirements), _tally(entries))
    if args.to_stdout:
        sys.stdout.write(text)
        return 0
    if args.check:
        current = out.read_text(encoding="utf-8") if out.is_file() else ""
        if current != text:
            print("traceability: %s is out of date" % out, file=sys.stderr)
            print(_first_difference(current, text), file=sys.stderr)
            print("run: python3 tools/traceability.py", file=sys.stderr)
            return 1
        print("traceability: OK - %s" % summary)
        return 0
    out.write_text(text, encoding="utf-8")
    relative = out.relative_to(root) if root in out.parents else out
    print("traceability: wrote %s - %s" % (relative, summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

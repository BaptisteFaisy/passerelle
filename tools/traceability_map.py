#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Parsers behind tools/traceability.py: the spec, the map and the test catalogue.

This module reads three things and invents nothing:

* ``docs/spec-en.md`` - the requirement identifiers and their statement, taken
  from the section 6 tables (first cell is the identifier, then requirement,
  tier, verification method, prior art);
* ``tools/traceability-map.yml`` - the state, the proof and the note of every
  requirement, in the restricted YAML subset documented below;
* the tree itself - the set of test names the build knows about, so a proof
  that points at a test nobody runs is caught instead of published.

Map file format (no PyYAML, standard library only)
--------------------------------------------------
``tools/traceability-map.yml`` is a restricted flow-style YAML subset: one
requirement per line, and nothing else::

    VIO-01: {state: tested, tests: [test_virtio_pci], files: [virtio/src/pci.c], note: "..."}

Grammar accepted by :func:`parse_map`:

* a line is blank, a ``#`` comment, or exactly one entry;
* an entry is ``<ID>: {key: value, ...}`` with the four keys ``state``,
  ``tests``, ``files``, ``note``, each exactly once, in any order;
* ``state`` is a bare word among ``tested``, ``partial``, ``missing``;
* ``tests`` and ``files`` are ``[a, b]`` lists of bare words (``[]`` = empty);
* ``note`` is a double-quoted string; ``\\"`` and ``\\\\`` are the only escapes;
* no nesting, no anchors, no multi-line value, no inline comment.

Anything else raises ``ValueError`` with the line number: the parser reports a
malformed map instead of guessing what the author meant.
"""

from __future__ import annotations

import dataclasses
import itertools
import pathlib
import re

PREFIXES = ("VIO", "P9", "FS", "QNX", "PERF", "ROB", "SEC", "DX", "VFS", "SRV")
ID_RE = re.compile(r"^(?:%s)-\d{2}$" % "|".join(PREFIXES))
STATES = ("tested", "partial", "missing")
SKIP_DIR_PREFIX = ("build",)
SKIP_DIRS = {".git", "__pycache__", ".venv", "venv", ".pytest_cache", "node_modules"}

CMAKE_TEST_RE = re.compile(r"(?:add_test\s*\(\s*NAME|pgw_test\s*\()\s*([A-Za-z0-9_${}\-]+)")
CMAKE_SET_RE = re.compile(r"\bset\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s+(.*?)\)", re.DOTALL)
CMAKE_FOREACH_RE = re.compile(r"^foreach\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*(.*?)\)\s*$")
PGW_CASE_RE = re.compile(r"^PGW_TEST\(([A-Za-z_][A-Za-z0-9_]*)\)")
PYTEST_CASE_RE = re.compile(r"^def (test_[A-Za-z0-9_]+)\s*\(")
ENTRY_RE = re.compile(r"^([A-Z0-9]+-\d{2}):\s*\{(.*)\}$")
ITEM_RE = re.compile(r"^[A-Za-z0-9_./+\-]+$")
VAR_RE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")
MAP_KEYS = ("state", "tests", "files", "note")


@dataclasses.dataclass(frozen=True)
class Requirement:
    """One row of the section 6 tables of the specification."""

    rid: str
    statement: str
    tier: str
    verif: str
    prior: str


@dataclasses.dataclass(frozen=True)
class Entry:
    """One row of tools/traceability-map.yml."""

    rid: str
    state: str
    tests: tuple[str, ...]
    files: tuple[str, ...]
    note: str


def _skip(path: pathlib.Path) -> bool:
    """True for build trees and noise directories (never scanned)."""
    for part in path.parts:
        if part in SKIP_DIRS or part.startswith(SKIP_DIR_PREFIX):
            return True
    return False


def _source_files(root: pathlib.Path, suffixes: tuple[str, ...]) -> list[pathlib.Path]:
    """Every file of *suffixes* under *root*, sorted, build trees excluded."""
    return sorted(p for p in root.rglob("*")
                  if p.is_file() and p.suffix in suffixes and not _skip(p.relative_to(root)))


def _table_cells(line: str) -> list[str]:
    """Split one Markdown table row into cells ([] when it is not a row)."""
    text = line.strip()
    if not text.startswith("|"):
        return []
    return [cell.strip() for cell in text.strip("|").split("|")]


def parse_spec(path: pathlib.Path) -> list[Requirement]:
    """Read the requirement rows of the spec, in spec order, deduplicated."""
    rows: list[Requirement] = []
    seen: set[str] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        cells = _table_cells(line)
        if not cells or not ID_RE.match(cells[0]) or cells[0] in seen:
            continue
        seen.add(cells[0])
        body = cells[1:]
        statement = " | ".join(body[:-3]) if len(body) > 4 else (body[0] if body else "")
        tail = (body[-3:] + ["", "", ""])[:3]
        rows.append(Requirement(cells[0], statement, tail[0], tail[1], tail[2]))
    return rows


def _cmake_variables(text: str) -> dict[str, list[str]]:
    """Collect the ``set(VAR a b c)`` lists of one CMakeLists.txt (any layout)."""
    return {match.group(1): match.group(2).split()
            for match in CMAKE_SET_RE.finditer(text)}


def _foreach_items(rest: str, variables: dict[str, list[str]]) -> list[str]:
    """Resolve the item list of ``foreach(var ...)``: literals or IN LISTS."""
    text = rest.strip()
    listed = re.match(r"(?:IN\s+(?:LISTS|ITEMS)\s+)(.+)$", text, re.IGNORECASE)
    if listed:
        items: list[str] = []
        for name in listed.group(1).split():
            items.extend(variables.get(VAR_RE.sub("", name), []))
        return items
    return [item for token in text.split() for item in ([token] if "${" not in token
                                                        else variables.get(token[2:-1], []))]


def _expand_name(raw: str, variables: dict[str, list[str]],
                 scopes: list[tuple[str, list[str]]]) -> list[str]:
    """Expand ``${var}`` in a test name using the foreach scopes, then set()."""
    names = VAR_RE.findall(raw)
    if not names:
        return [raw]
    choices: list[list[str]] = []
    for name in names:
        values = next((items for var, items in scopes if var == name), None)
        if values is None:
            values = variables.get(name)
        if not values:
            return []
        choices.append(values)
    expanded: list[str] = []
    for combo in itertools.product(*choices):
        text = raw
        for name, value in zip(names, combo):
            text = text.replace("${%s}" % name, value)
        expanded.append(text)
    return expanded


def _cmake_names(text: str) -> set[str]:
    """CTest names declared by one CMakeLists.txt, foreach loops expanded."""
    variables = _cmake_variables(text)
    names: set[str] = set()
    scopes: list[tuple[str, list[str]]] = []
    for line in text.splitlines():
        stripped = line.strip()
        loop = CMAKE_FOREACH_RE.match(stripped)
        if loop:
            scopes.append((loop.group(1), _foreach_items(loop.group(2), variables)))
        elif stripped.startswith("endforeach"):
            if scopes:
                scopes.pop()
        match = CMAKE_TEST_RE.search(stripped)
        if match:
            names.update(_expand_name(match.group(1), variables, scopes))
    return names


def cmake_test_names(root: pathlib.Path) -> set[str]:
    """Every CTest name declared anywhere in the tree."""
    names: set[str] = set()
    for path in _source_files(root, (".txt",)):
        if path.name == "CMakeLists.txt":
            names.update(_cmake_names(path.read_text(encoding="utf-8", errors="replace")))
    return names


def pgw_case_names(root: pathlib.Path) -> set[str]:
    """Every C test case registered with ``PGW_TEST(name)``."""
    names: set[str] = set()
    for path in _source_files(root, (".c", ".h")):
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            match = PGW_CASE_RE.match(line.strip())
            if match:
                names.add(match.group(1))
    return names


def pytest_case_names(root: pathlib.Path) -> set[str]:
    """Every ``def test_*(...)`` of the pytest suites (the host-tools CI job)."""
    names: set[str] = set()
    for path in _source_files(root, (".py",)):
        if not path.name.startswith("test_"):
            continue
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            match = PYTEST_CASE_RE.match(line)
            if match:
                names.add(match.group(1))
    return names


def test_catalog(root: pathlib.Path) -> set[str]:
    """Union of the three catalogues of test names known to the build."""
    return cmake_test_names(root) | pgw_case_names(root) | pytest_case_names(root)


def _split_pairs(body: str) -> list[str]:
    """Split ``k: v, k: v`` on top-level commas, honouring quotes and lists."""
    parts: list[str] = []
    depth = 0
    quoted = False
    current: list[str] = []
    index = 0
    while index < len(body):
        char = body[index]
        if char == "\\" and quoted:
            current.append(body[index:index + 2])
            index += 2
            continue
        if char == '"':
            quoted = not quoted
        elif not quoted:
            if char == "[":
                depth += 1
            elif char == "]":
                depth -= 1
            elif char == "," and depth == 0:
                parts.append("".join(current))
                current = []
                index += 1
                continue
        current.append(char)
        index += 1
    parts.append("".join(current))
    return [part.strip() for part in parts if part.strip()]


def _parse_list(rid: str, key: str, raw: str, lineno: int) -> tuple[str, ...]:
    """Parse one ``[a, b]`` list value; raise ValueError on a malformed item."""
    text = raw.strip()
    if not (text.startswith("[") and text.endswith("]")):
        raise ValueError("%s:%d: %s: '%s' is not a [a, b] list" % (rid, lineno, key, text))
    items = [item.strip() for item in text[1:-1].split(",") if item.strip()]
    for item in items:
        if not ITEM_RE.match(item):
            raise ValueError("%s:%d: %s: '%s' is not a bare word" % (rid, lineno, key, item))
    return tuple(items)


def _parse_note(rid: str, raw: str, lineno: int) -> str:
    """Parse one double-quoted note; raise ValueError when unquoted."""
    text = raw.strip()
    if len(text) < 2 or not (text.startswith('"') and text.endswith('"')):
        raise ValueError('%s:%d: note must be a double-quoted string, got %r'
                         % (rid, lineno, text))
    return text[1:-1].replace('\\"', '"').replace("\\\\", "\\")


def _entry_fields(rid: str, body: str, lineno: int) -> dict[str, object]:
    """Collect the four key/value pairs of one entry, rejecting unknown keys."""
    fields: dict[str, object] = {}
    for pair in _split_pairs(body):
        key, _, value = pair.partition(":")
        key, value = key.strip(), value.strip()
        if key not in MAP_KEYS:
            raise ValueError("%s:%d: unknown key '%s'" % (rid, lineno, key))
        if key in fields:
            raise ValueError("%s:%d: key '%s' given twice" % (rid, lineno, key))
        if key == "state":
            fields[key] = value
        elif key == "note":
            fields[key] = _parse_note(rid, value, lineno)
        else:
            fields[key] = _parse_list(rid, key, value, lineno)
    missing = [key for key in MAP_KEYS if key not in fields]
    if missing:
        raise ValueError("%s:%d: missing key(s) %s" % (rid, lineno, ", ".join(missing)))
    return fields


def _entry_from_line(line: str, lineno: int) -> Entry:
    """Turn one map line into an Entry; raise ValueError with the line number."""
    match = ENTRY_RE.match(line.strip())
    if not match:
        raise ValueError("line %d: not '<ID>: {state: ..., tests: [...], "
                         "files: [...], note: \"...\"}'" % lineno)
    rid = match.group(1)
    fields = _entry_fields(rid, match.group(2), lineno)
    state = str(fields["state"])
    if state not in STATES:
        raise ValueError("%s:%d: state '%s' is not one of %s"
                         % (rid, lineno, state, ", ".join(STATES)))
    return Entry(rid, state, tuple(fields["tests"]), tuple(fields["files"]),
                 str(fields["note"]))


def parse_map(path: pathlib.Path) -> dict[str, Entry]:
    """Read the whole map file; raise ValueError on the first malformed line."""
    entries: dict[str, Entry] = {}
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        text = line.strip()
        if not text or text.startswith("#"):
            continue
        entry = _entry_from_line(text, lineno)
        if entry.rid in entries:
            raise ValueError("%s: duplicate entry (first on line %d)" % (entry.rid, lineno))
        entries[entry.rid] = entry
    return entries

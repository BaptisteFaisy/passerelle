# SPDX-License-Identifier: Apache-2.0
"""Tests for the traceability generator (tools/traceability.py).

These run on the real repository: the real spec (docs/spec-en.md), the real
map (tools/traceability-map.yml) and the real tree, so a renamed test or a
moved file breaks the suite instead of silently rotting the submission
matrix. The failure modes are exercised on throwaway copies in tmp_path, so
nothing here writes into the repository.
"""

from __future__ import annotations

import importlib.util
import pathlib
import sys

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = REPO_ROOT / "tools" / "traceability.py"
MAP = REPO_ROOT / "tools" / "traceability-map.yml"
SPEC = REPO_ROOT / "docs" / "spec-en.md"
MATRIX = REPO_ROOT / "docs" / "traceability.md"
REQUIREMENT_COUNT = 77  # section 6 of docs/spec-en.md, the hackathon scope


def _load_tool():
    """Import tools/traceability.py without installing it (stdlib only)."""
    spec = importlib.util.spec_from_file_location("pgw_traceability", TOOL)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["pgw_traceability"] = module
    spec.loader.exec_module(module)
    return module


tr = _load_tool()


@pytest.fixture(scope="module")
def requirements() -> list:
    """Every requirement row of the real spec, in spec order."""
    return tr.parse_spec(SPEC)


@pytest.fixture(scope="module")
def entries() -> dict:
    """Every entry of the real map."""
    return tr.parse_map(MAP)


@pytest.fixture(scope="module")
def catalog() -> set:
    """Every test name the build knows about."""
    return tr.test_catalog(REPO_ROOT)


def test_spec_yields_exactly_the_expected_requirements(requirements) -> None:
    """The parser reads the section 6 tables and nothing else."""
    assert len(requirements) == REQUIREMENT_COUNT
    ids = [req.rid for req in requirements]
    assert len(set(ids)) == REQUIREMENT_COUNT, "a requirement is listed twice"
    assert ids[0] == "VIO-01" and ids[-1] == "SRV-02"
    assert all(tr.ID_RE.match(req.rid) for req in requirements)


def test_every_requirement_is_covered(requirements, entries) -> None:
    """No requirement of the spec is left out of the map."""
    spec_ids = {req.rid for req in requirements}
    assert spec_ids - set(entries) == set(), "the map is stale"


def test_map_has_no_unknown_or_phantom_identifier(requirements, entries) -> None:
    """Every map key is a real requirement: no typo, no phantom."""
    spec_ids = {req.rid for req in requirements}
    phantoms = set(entries) - spec_ids
    assert phantoms == set(), f"phantom requirement(s): {sorted(phantoms)}"
    assert all(tr.ID_RE.match(rid) for rid in entries)


def test_every_cited_file_exists(entries) -> None:
    """A proof that points at a missing file is not a proof."""
    missing = [f"{rid}: {name}" for rid, entry in entries.items()
               for name in entry.files if not (REPO_ROOT / name).is_file()]
    assert missing == []


def test_every_cited_test_exists_in_the_build(entries, catalog) -> None:
    """A cited test must be declared by CMake, a PGW_TEST() case or pytest."""
    unknown = [f"{rid}: {name}" for rid, entry in entries.items()
               for name in entry.tests if name not in catalog]
    assert unknown == []
    assert catalog, "the catalogue is empty: the tree was not scanned"


def test_generator_reports_no_inconsistency(requirements, entries, catalog) -> None:
    """The tool's own validation, on the real inputs."""
    assert tr.validate(requirements, entries, catalog, REPO_ROOT) == []


def test_states_are_the_documented_vocabulary(entries) -> None:
    """Only the three states exist, and each carries its justification."""
    assert {entry.state for entry in entries.values()} <= set(tr.STATES)
    for rid, entry in entries.items():
        if entry.state in ("partial", "missing"):
            assert entry.note.strip(), f"{rid}: {entry.state} needs a note"


def test_unstarted_requirements_are_extreme_tier(requirements, entries) -> None:
    """A 'missing' entry is only honest for the X tier the spec de-scopes."""
    tier = {req.rid: req.tier for req in requirements}
    offenders = [rid for rid, entry in entries.items()
                 if entry.state == "missing" and tier[rid] != "X"]
    assert offenders == [], f"not X tier: {offenders}"


def test_tested_entries_carry_the_proof_the_spec_asks_for(requirements, entries) -> None:
    """T requirements cite a test; I/M/D requirements cite an evidence file."""
    by_id = {req.rid: req for req in requirements}
    for rid, entry in entries.items():
        if entry.state != "tested":
            continue
        if "T" in by_id[rid].verif:
            assert entry.tests, f"{rid}: tested by test, cites none"
        else:
            assert entry.files, f"{rid}: verified by {by_id[rid].verif}, cites no file"


def test_generator_prints_the_whole_matrix(capsys) -> None:
    """--print writes the document on stdout and changes nothing."""
    assert tr.main(["--root", str(REPO_ROOT), "--print"]) == 0
    text = capsys.readouterr().out
    assert text.startswith("# Requirement traceability")
    assert f"{REQUIREMENT_COUNT} requirements:" in text
    for marker in ("| VIO-01 |", "| PERF-01 |", "| SRV-02 |", "## Tally"):
        assert marker in text


def test_generator_is_deterministic(capsys) -> None:
    """Two runs give the same bytes: no date, no path, stable ordering."""
    assert tr.main(["--root", str(REPO_ROOT), "--print"]) == 0
    first = capsys.readouterr().out
    assert tr.main(["--root", str(REPO_ROOT), "--print"]) == 0
    assert capsys.readouterr().out == first


def test_committed_matrix_is_up_to_date() -> None:
    """--check: docs/traceability.md is what the generator produces now."""
    assert MATRIX.is_file(), "run: python3 tools/traceability.py"
    assert tr.main(["--root", str(REPO_ROOT), "--check"]) == 0


def _mini_tree(tmp_path: pathlib.Path, map_text: str) -> pathlib.Path:
    """A throwaway repository: one requirement, one map, no test at all."""
    (tmp_path / "docs").mkdir(exist_ok=True)
    (tmp_path / "tools").mkdir(exist_ok=True)
    (tmp_path / "docs" / "spec-en.md").write_text(
        "| ID | Requirement | Tier | Verif. | Prior Art |\n"
        "|---|---|---|---|---|\n"
        "| VIO-01 | Discover the device. | S | T | - |\n", encoding="utf-8")
    (tmp_path / "tools" / "traceability-map.yml").write_text(map_text, encoding="utf-8")
    return tmp_path


def _run(root: pathlib.Path, capsys) -> int:
    status = tr.main(["--root", str(root), "--print"])
    capsys.readouterr()
    return status


def test_stale_map_is_refused(tmp_path, capsys) -> None:
    """A spec requirement with no entry fails, loudly."""
    root = _mini_tree(tmp_path, "# nothing here\n")
    assert _run(root, capsys) == 2


def test_phantom_entry_is_refused(tmp_path, capsys) -> None:
    """An entry the spec does not ask for fails, loudly."""
    root = _mini_tree(tmp_path, 'VIO-01: {state: tested, tests: [], files: [], note: "x"}\n'
                                'VIO-99: {state: tested, tests: [], files: [], note: "x"}\n')
    assert _run(root, capsys) == 2


def test_missing_file_and_unknown_test_are_refused(tmp_path, capsys) -> None:
    """Both halves of the evidence must exist."""
    root = _mini_tree(tmp_path, 'VIO-01: {state: tested, tests: [no_such_test], '
                                'files: [no/such/file.c], note: "x"}\n')
    assert _run(root, capsys) == 2


def test_malformed_map_line_is_refused(tmp_path, capsys) -> None:
    """The restricted YAML subset is parsed strictly, never guessed."""
    root = _mini_tree(tmp_path, "VIO-01: state: tested\n")
    assert _run(root, capsys) == 2
    root = _mini_tree(tmp_path, 'VIO-01: {state: maybe, tests: [], files: [], note: "x"}\n')
    assert _run(root, capsys) == 2
    root = _mini_tree(tmp_path, 'VIO-01: {state: tested, tests: [], files: []}\n')
    assert _run(root, capsys) == 2


def test_tested_without_a_test_is_refused(tmp_path, capsys) -> None:
    """A T requirement cannot be 'tested' on the strength of a note alone."""
    root = _mini_tree(tmp_path, 'VIO-01: {state: tested, tests: [], files: [], note: "x"}\n')
    assert _run(root, capsys) == 2


def test_missing_entry_must_not_claim_a_test(requirements) -> None:
    """Nothing runs for an unstarted requirement, so no test may be cited."""
    entry = tr.Entry("VIO-01", "missing", ("common",), (), "not started")
    problems = tr.validate([requirements[0]], {"VIO-01": entry}, {"common"}, REPO_ROOT)
    assert any("must cite no test" in item for item in problems), problems

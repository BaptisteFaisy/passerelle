# SPDX-License-Identifier: Apache-2.0
"""Tests of the release tooling: one version everywhere, one coherent archive.

These are the automated half of `docs/release.md`. `host/pyproject.toml` cannot
read `../VERSION` (a published sdist only contains its own directory), so the
version is a literal copy there and *this file* is what keeps the copy honest:
if the two drift, `make release-check` and the release workflow both fail.

Run with:
    python3 -m pytest tools/release -q
"""

from __future__ import annotations

import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import changelog_section  # noqa: E402
import make_tarball  # noqa: E402
import verify_release  # noqa: E402
from releaseinfo import (  # noqa: E402
    VERSION_RE,
    find_section,
    pyproject_version,
    read_sections,
    read_version,
    released_versions,
)

ROOT = pathlib.Path(__file__).resolve().parents[2]
REQUIRED_SUBSECTIONS = ("### Added", "### Changed", "### Fixed", "### Known limitations")
REQUIREMENT_IDS = ("VIO-", "P9-", "FS-", "QNX-", "PERF-", "ROB-", "SEC-", "DX-", "VFS-", "SRV-")


@pytest.fixture(scope="module")
def version() -> str:
    """The version this tree claims."""
    return read_version(ROOT)


def test_version_file_is_semver(version: str) -> None:
    """VERSION is the single source of truth, so it must be a clean version."""
    assert VERSION_RE.match(version), version
    assert (ROOT / "VERSION").read_text(encoding="utf-8").strip() == version


def test_pyproject_matches_version_file(version: str) -> None:
    """The literal copy in host/pyproject.toml must equal VERSION."""
    assert pyproject_version(ROOT) == version


def test_changelog_has_section_for_version(version: str) -> None:
    """CHANGELOG.md documents the released version, with a date."""
    section = find_section(read_sections(ROOT), version)
    assert section is not None, f"no '## [{version}] - date' section"
    assert section.date, "the section has no release date"
    assert section.body.strip(), "the section is empty"


def test_changelog_newest_section_is_the_released_version(version: str) -> None:
    """An older section may not sit above the released one."""
    released = released_versions(read_sections(ROOT))
    assert released, "CHANGELOG.md has no released version"
    assert released[0] == version


def test_changelog_section_has_the_four_subsections(version: str) -> None:
    """Keep a Changelog plus the project's honest 'Known limitations'."""
    body = find_section(read_sections(ROOT), version).body
    for subsection in REQUIRED_SUBSECTIONS:
        assert subsection in body, f"missing {subsection!r}"


def test_changelog_entries_cite_requirement_ids(version: str) -> None:
    """Entries trace back to the specification's requirement identifiers."""
    body = find_section(read_sections(ROOT), version).body
    cited = [prefix for prefix in REQUIREMENT_IDS if prefix in body]
    assert len(cited) >= 5, f"only {cited} requirement families are cited"


def test_changelog_admits_the_qnx_limitation(version: str) -> None:
    """No claim of a QNX run: there is no SDP on the workstation."""
    body = find_section(read_sections(ROOT), version).body.lower()
    assert "sdp" in body, "Known limitations must state the missing QNX SDP"


@pytest.mark.parametrize(
    "relpath",
    [
        "LICENSE",
        "NOTICE",
        "README.md",
        "Makefile",
        "CONTRIBUTING.md",
        "SECURITY.md",
        "CODE_OF_CONDUCT.md",
        "docs/release.md",
        ".github/workflows/release.yml",
    ],
)
def test_release_files_exist(relpath: str) -> None:
    """Every file the release process points at is in the repository."""
    assert (ROOT / relpath).is_file(), relpath


def test_verify_release_passes_on_this_tree() -> None:
    """The command `make release-check` runs must be green here."""
    assert verify_release.main(["--root", str(ROOT)]) == 0


def test_changelog_section_script_prints_the_body(version: str, capsys) -> None:
    """The release notes come from the CHANGELOG, not from a hand-written text."""
    assert changelog_section.main([version, "--root", str(ROOT)]) == 0
    printed = capsys.readouterr().out
    assert "### Added" in printed, printed[:120]
    assert f"## [{version}]" not in printed


def test_changelog_section_script_rejects_an_unknown_version() -> None:
    """An unknown version is an error, never an empty release note."""
    assert changelog_section.main(["9.9.9", "--root", str(ROOT)]) == 1


def test_tarball_is_deterministic(tmp_path: pathlib.Path, version: str) -> None:
    """Two builds of the same tree give the same bytes, hence the same SHA256."""
    first = make_tarball.build_tarball(ROOT, version, tmp_path / "a", 1794355200, f"p-{version}")
    second = make_tarball.build_tarball(ROOT, version, tmp_path / "b", 1794355200, f"p-{version}")
    assert first[1] == second[1], "the archive is not reproducible"
    assert first[0].read_bytes() == second[0].read_bytes()


def test_tarball_contains_the_release_contract(tmp_path: pathlib.Path, version: str) -> None:
    """LICENSE, the attribution NOTICE, README.md and docs/ travel with it."""
    files = make_tarball.collect_files(ROOT)
    assert make_tarball.missing_required(files) == []
    for entry in ("LICENSE", "NOTICE", "README.md", "docs/release.md", "VERSION"):
        assert entry in files, entry
    path, digest, count, total = make_tarball.build_tarball(
        ROOT, version, tmp_path, 1794355200, f"passerelle-{version}"
    )
    assert count > 100 and total > 100_000
    sidecar = make_tarball.write_checksum(path, digest)
    assert sidecar.read_text(encoding="utf-8").split() == [digest, path.name]


def test_tarball_excludes_build_trees(tmp_path: pathlib.Path) -> None:
    """No compiled object, no CMake tree, no cache in a source release."""
    files = make_tarball.collect_files(ROOT)
    assert not any(name.startswith("build") for name in files), files[:5]
    assert not any("__pycache__" in name or name.endswith(".o") for name in files)

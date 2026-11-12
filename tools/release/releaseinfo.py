#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Shared helpers of the release tooling: the version string and the CHANGELOG.

The three release scripts (`make_tarball.py`, `verify_release.py`,
`changelog_section.py`) need the same three facts, so the facts are written
once here instead of three times:

  * the version of the project lives in the top-level `VERSION` file, which is
    the single source of truth (`host/pyproject.toml` carries a literal copy
    because a published sdist cannot read a file outside its own directory;
    `test_version_sync.py` checks the copy);
  * `CHANGELOG.md` follows Keep a Changelog: sections are written
    `## [X.Y.Z] - YYYY-MM-DD`, newest first, and hold `### Added` / `### Changed`
    / `### Fixed` sub-sections whose entries cite the requirement identifiers of
    the specification (VIO-*, P9-*, FS-*, QNX-*, PERF-*, ROB-*, SEC-*, DX-*,
    VFS-*, SRV-*);
  * a git tag is `v` + the version, with no leading zero padding.

This module serves the developer-experience requirement DX-05 (the project must
be readable and reproducible from its own documentation) and closes the
publication gaps B1/30-42 of `docs/gap-analysis.md`. Standard library only.
"""

from __future__ import annotations

import pathlib
import re
from typing import NamedTuple

#: Fallback timestamp (2026-11-11T00:00:00Z) when neither `SOURCE_DATE_EPOCH`
#: nor a git history can give the tarball a deterministic mtime.
DEFAULT_EPOCH = 1794355200

#: A released version is MAJOR.MINOR.PATCH, optionally with a SemVer suffix.
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$")

#: `## [0.1.0] - 2026-11-11` (the date is optional for `[Unreleased]`).
SECTION_RE = re.compile(r"^##\s+\[(?P<version>[^\]]+)\](?:\s*-\s*(?P<date>\S+))?\s*$")

CHANGELOG_NAME = "CHANGELOG.md"
VERSION_NAME = "VERSION"
PYPROJECT_PATH = pathlib.Path("host") / "pyproject.toml"


class ReleaseError(Exception):
    """A release precondition is not met (bad version, missing section, ...)."""


class Section(NamedTuple):
    """One `## [version] - date` block of CHANGELOG.md."""

    version: str
    date: str
    heading: str
    body: str
    line: int


def repo_root(start: pathlib.Path | None = None) -> pathlib.Path:
    """Return the repository root (the directory that holds VERSION)."""
    if start is not None:
        return pathlib.Path(start).resolve()
    return pathlib.Path(__file__).resolve().parents[2]


def read_version(root: pathlib.Path | None = None) -> str:
    """Return the project version from the VERSION file (single source of truth)."""
    path = (root or repo_root()) / VERSION_NAME
    try:
        text = path.read_text(encoding="utf-8").strip()
    except OSError as err:
        raise ReleaseError(f"cannot read {path}: {err}") from err
    if not VERSION_RE.match(text):
        raise ReleaseError(f"{path}: {text!r} is not a MAJOR.MINOR.PATCH version")
    return text


def normalize_tag(tag: str, version: str) -> str:
    """Return the expected git tag for a version, tolerating a `v` prefix."""
    cleaned = tag.strip()
    if cleaned.startswith("refs/tags/"):
        cleaned = cleaned[len("refs/tags/") :]
    return cleaned if cleaned else f"v{version}"


def pyproject_version(root: pathlib.Path | None = None) -> str | None:
    """Return `project.version` of host/pyproject.toml, or None when absent."""
    path = (root or repo_root()) / PYPROJECT_PATH
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return None
    try:  # Python 3.11+: the real parser, so comments cannot fool us.
        import tomllib

        data = tomllib.loads(text)
        value = data.get("project", {}).get("version")
        return str(value) if value is not None else None
    except ImportError:
        pass
    match = re.search(r'^\s*version\s*=\s*"([^"]+)"', text, re.MULTILINE)
    return match.group(1) if match else None


def parse_changelog(text: str) -> list[Section]:
    """Split CHANGELOG.md into its `## [version]` sections, in file order."""
    lines = text.splitlines()
    marks: list[tuple[int, re.Match[str]]] = [
        (idx, match) for idx, line in enumerate(lines) for match in [SECTION_RE.match(line)] if match
    ]
    sections: list[Section] = []
    for pos, (idx, match) in enumerate(marks):
        end = marks[pos + 1][0] if pos + 1 < len(marks) else len(lines)
        sections.append(_section(lines, idx, end, match))
    return sections


def _section(lines: list[str], start: int, end: int, match: re.Match[str]) -> Section:
    """Build one Section from its heading line and its body range."""
    body = [line for line in lines[start + 1 : end] if not _is_link_definition(line)]
    while body and not body[-1].strip():
        body.pop()
    while body and not body[0].strip():
        body.pop(0)
    return Section(
        version=match.group("version").strip(),
        date=(match.group("date") or "").strip(),
        heading=lines[start],
        body="\n".join(body),
        line=start + 1,
    )


def _is_link_definition(line: str) -> bool:
    """True for the `[0.1.0]: https://...` reference links of Keep a Changelog."""
    stripped = line.strip()
    return bool(re.match(r"^\[[^\]]+\]:\s*\S", stripped))


def read_sections(root: pathlib.Path | None = None) -> list[Section]:
    """Return the sections of the repository's CHANGELOG.md."""
    path = (root or repo_root()) / CHANGELOG_NAME
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as err:
        raise ReleaseError(f"cannot read {path}: {err}") from err
    return parse_changelog(text)


def find_section(sections: list[Section], version: str) -> Section | None:
    """Return the section of `version`, or None when the CHANGELOG lacks it."""
    for section in sections:
        if section.version == version:
            return section
    return None


def released_versions(sections: list[Section]) -> list[str]:
    """Return the versions that look released (SemVer), newest first."""
    return [s.version for s in sections if VERSION_RE.match(s.version)]

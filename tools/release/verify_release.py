#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check that a release is coherent before it is tagged, built or published.

One command answers "can this tree become vX.Y.Z?", which is what
`make release-check` and the `verify` job of `.github/workflows/release.yml`
run. A release is coherent when all of the following hold:

  1. `VERSION` exists and is a MAJOR.MINOR.PATCH string;
  2. `CHANGELOG.md` has a `## [VERSION] - date` section, and it is the newest
     released section (an older version may not be released as the new one);
  3. `host/pyproject.toml` carries the same version - it is a literal copy,
     because a published sdist cannot read `../VERSION`; the copy is what makes
     `pip install` and `qshare --version` agree with the tag;
  4. the git tag, when one is given or detected, is `v` + VERSION;
  5. the tarball, when one is given, matches its `.sha256` sidecar.

Usage:
    python3 tools/release/verify_release.py
    python3 tools/release/verify_release.py --tag v0.1.0 --require-tag
    python3 tools/release/verify_release.py --tarball dist/passerelle-0.1.0.tar.gz

Exit status: 0 when every check passed, 1 otherwise (never a silent green).
Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from releaseinfo import (  # noqa: E402  (import after the sys.path fix above)
    ReleaseError,
    find_section,
    normalize_tag,
    pyproject_version,
    read_sections,
    read_version,
    released_versions,
    repo_root,
)


def detect_tag(root: pathlib.Path) -> str | None:
    """Return the tag on HEAD, or the tag of $GITHUB_REF, or None."""
    ref = os.environ.get("GITHUB_REF", "")
    if ref.startswith("refs/tags/"):
        return normalize_tag(ref, "")
    try:
        out = subprocess.run(
            ["git", "describe", "--tags", "--exact-match", "HEAD"],
            cwd=root,
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return out.stdout.strip() or None if out.returncode == 0 else None


def check_changelog(root: pathlib.Path, version: str, problems: list[str]) -> None:
    """Require a CHANGELOG section for `version`, and that it is the newest."""
    try:
        sections = read_sections(root)
    except ReleaseError as err:
        problems.append(str(err))
        return
    section = find_section(sections, version)
    if section is None:
        problems.append(f"CHANGELOG.md: no '## [{version}] - date' section")
        return
    if not section.date:
        problems.append(f"CHANGELOG.md:{section.line}: section [{version}] has no date")
    released = released_versions(sections)
    if released and released[0] != version:
        problems.append(
            f"CHANGELOG.md: newest released section is {released[0]}, not {version}"
        )


def check_pyproject(root: pathlib.Path, version: str, problems: list[str]) -> None:
    """Require host/pyproject.toml to carry the same version as VERSION."""
    found = pyproject_version(root)
    path = root / "host" / "pyproject.toml"
    if found is None:
        problems.append(f"{path}: no [project] version found")
    elif found != version:
        problems.append(f"{path}: version {found!r} != VERSION {version!r}")


def check_tag(root: pathlib.Path, version: str, args: argparse.Namespace, problems: list[str]) -> None:
    """Require the git tag, when known, to be `v` + VERSION."""
    expected = f"v{version}"
    tag = normalize_tag(args.tag, version) if args.tag else detect_tag(root)
    if tag is None:
        if args.require_tag:
            problems.append(f"no git tag on HEAD (expected {expected}); use --tag v{version}")
        else:
            print(f"tag: none on HEAD (expected {expected} at release time) - not required here")
        return
    if tag != expected:
        problems.append(f"git tag {tag!r} != {expected!r} (VERSION says {version})")
    else:
        print(f"tag: {tag} matches VERSION")


def check_tarball(tarball: pathlib.Path, problems: list[str]) -> None:
    """Require the tarball to match its `.sha256` sidecar."""
    sidecar = pathlib.Path(str(tarball) + ".sha256")
    if not tarball.is_file():
        problems.append(f"tarball not found: {tarball}")
        return
    digest = hashlib.sha256(tarball.read_bytes()).hexdigest()
    if not sidecar.is_file():
        problems.append(f"missing checksum file: {sidecar}")
        return
    recorded = sidecar.read_text(encoding="utf-8").split()[0] if sidecar.read_text() else ""
    if recorded != digest:
        problems.append(f"{sidecar}: recorded {recorded} != computed {digest}")
    else:
        print(f"tarball: {tarball.name} sha256 {digest} verified")


def main(argv: list[str] | None = None) -> int:
    """Run every coherence check and report."""
    parser = argparse.ArgumentParser(description="Verify release coherence.")
    parser.add_argument("--root", default=None, help="repository root (default: auto)")
    parser.add_argument("--tag", default=None, help="git tag to check (e.g. v0.1.0)")
    parser.add_argument(
        "--require-tag", action="store_true", help="fail when no tag is given or detected"
    )
    parser.add_argument("--tarball", default=None, help="tarball whose .sha256 must match")
    parser.add_argument(
        "--version", default=None, help="version to verify (default: the VERSION file)"
    )
    args = parser.parse_args(argv)

    root = pathlib.Path(args.root).resolve() if args.root else repo_root()
    problems: list[str] = []
    try:
        version = args.version or read_version(root)
        print(f"VERSION: {version}")
    except ReleaseError as err:
        print(f"verify-release: FAILED\n  {err}", file=sys.stderr)
        return 1

    check_changelog(root, version, problems)
    check_pyproject(root, version, problems)
    check_tag(root, version, args, problems)
    if args.tarball:
        check_tarball(pathlib.Path(args.tarball), problems)

    if problems:
        print("verify-release: FAILED", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1
    print(f"verify-release: OK (VERSION {version} is coherent with CHANGELOG.md and pyproject)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

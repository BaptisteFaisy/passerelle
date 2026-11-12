#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Extract one version's section of CHANGELOG.md, for the release notes.

The GitHub release created by `.github/workflows/release.yml` is not written by
hand: its body is the `## [X.Y.Z] - date` block of CHANGELOG.md, so the notes a
reader finds on the release page and the notes in the repository cannot drift
apart (DX-05, gap-analysis B1/31).

Usage:
    python3 tools/release/changelog_section.py            # version from VERSION
    python3 tools/release/changelog_section.py 0.1.0
    python3 tools/release/changelog_section.py 0.1.0 --with-heading
    python3 tools/release/changelog_section.py --list

Exit status: 0 when the section was printed, 1 when it does not exist.
Standard library only (project rule for production code).
"""

from __future__ import annotations

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from releaseinfo import (  # noqa: E402  (import after the sys.path fix above)
    ReleaseError,
    read_sections,
    read_version,
    released_versions,
)


def main(argv: list[str] | None = None) -> int:
    """Print the requested CHANGELOG section."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("version", nargs="?", help="version to extract (default: VERSION file)")
    parser.add_argument("--root", default=None, help="repository root (default: auto)")
    parser.add_argument(
        "--with-heading", action="store_true", help="include the '## [x.y.z] - date' line"
    )
    parser.add_argument("--list", action="store_true", help="list the released versions and exit")
    args = parser.parse_args(argv)

    try:
        root = pathlib.Path(args.root).resolve() if args.root else None
        sections = read_sections(root)
        if args.list:
            for version in released_versions(sections):
                print(version)
            return 0
        version = args.version or read_version(root)
        section = next((s for s in sections if s.version == version), None)
        if section is None:
            known = ", ".join(released_versions(sections)) or "none"
            print(f"changelog: no section for {version} (known: {known})", file=sys.stderr)
            return 1
        if args.with_heading:
            print(section.heading)
            print()
        print(section.body)
        return 0
    except ReleaseError as err:
        print(f"changelog: {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

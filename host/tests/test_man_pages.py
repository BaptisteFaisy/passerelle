# SPDX-License-Identifier: Apache-2.0
"""Tests of the man-page generator (gap 98, DX-05).

Why the CI wiring is a pytest and not a new workflow: the `host-tools` job of
ci.yml runs `pytest host/tests` on every pull request (no path filter) and is
a required check of .mergify.yml, while a brand-new workflow is not required
- a red one would not block a merge. Testing `build_man.py --check` here
makes a stale man page fail the PR at zero extra CI cost.

Covered: the committed pages pass --check (the CI gate itself), generation
is byte-deterministic, one page per command with the manual sections, every
CLI option of every parser appears in its page, and --check reports
outdated, missing and stale pages.
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys

import pytest

REPO = pathlib.Path(__file__).resolve().parents[2]
MAN = REPO / "man"
BUILDER = REPO / "tools" / "man" / "build_man.py"

sys.path.insert(0, str(REPO / "tools" / "man"))

import build_man  # noqa: E402

PAGES = sorted(f"{prog}.1" for prog in build_man.COMMANDS)
SECTIONS = (
    ".SH NAME",
    ".SH SYNOPSIS",
    ".SH DESCRIPTION",
    ".SH OPTIONS",
    ".SH EXIT STATUS",
    ".SH FILES",
    ".SH SEE ALSO",
)


def run_builder(*args: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
    """Run build_man.py in a subprocess, from the repository root."""
    return subprocess.run(
        [sys.executable, str(BUILDER), *args],
        cwd=REPO, capture_output=True, text=True, timeout=120, check=False, env=env,
    )


def plain(page: str) -> str:
    """Undo the roff escapes so option strings can be searched verbatim."""
    return page.replace("\\", "")


def test_committed_pages_pass_check() -> None:
    result = run_builder("--check")
    assert result.returncode == 0, result.stderr
    assert "up to date" in result.stdout


def test_generation_is_deterministic(tmp_path: pathlib.Path) -> None:
    assert build_man.generate() == build_man.generate()
    first, second = tmp_path / "a", tmp_path / "b"
    build_man.write_pages(first)
    build_man.write_pages(second)
    for page in PAGES:
        assert (first / page).read_bytes() == (second / page).read_bytes()


def test_source_date_epoch_is_honoured(tmp_path: pathlib.Path) -> None:
    env = {"SOURCE_DATE_EPOCH": "0", "PATH": "/usr/bin:/bin"}
    result = run_builder("--out", str(tmp_path), env=env)
    assert result.returncode == 0, result.stderr
    assert '"1970-01-01"' in (tmp_path / "qshare.1").read_text(encoding="utf-8")


@pytest.mark.parametrize("page", PAGES)
def test_page_structure(page: str) -> None:
    text = (MAN / page).read_text(encoding="utf-8")
    lines = text.splitlines()
    assert lines[0].startswith('.\\" SPDX-License-Identifier: Apache-2.0')
    assert lines[1].startswith(".TH ")
    for section in SECTIONS:
        assert section in text, f"{page}: missing {section}"
    version = (REPO / "VERSION").read_text(encoding="utf-8").strip()
    date = (REPO / "tools" / "man" / "PAGE_DATE").read_text(encoding="utf-8").strip()
    assert f'"{date}" "passerelle {version}"' in lines[1]


def test_one_page_per_command() -> None:
    assert sorted(p.name for p in MAN.glob("*.1")) == PAGES


@pytest.mark.parametrize("prog", sorted(build_man.COMMANDS))
def test_every_option_appears_in_its_page(prog: str) -> None:
    parser = build_man.load_parser(prog)
    page = plain((MAN / f"{prog}.1").read_text(encoding="utf-8"))
    for action in parser._actions:  # noqa: SLF001 - argparse tree, as the generator walks it
        for opt in action.option_strings:
            assert opt in page, f"{prog}: option {opt} missing from the page"
        if not action.option_strings and not isinstance(
            action, argparse._SubParsersAction  # noqa: SLF001
        ):
            assert action.dest.upper() in page, f"{prog}: {action.dest} missing"
    for name, _, sub in build_man.subparsers_of(parser):
        assert name in page, f"{prog}: subcommand {name} missing from the page"
        for action in sub._actions:  # noqa: SLF001
            for opt in action.option_strings:
                assert opt in page, f"{prog} {name}: option {opt} missing"


def test_check_fails_on_outdated_page(tmp_path: pathlib.Path) -> None:
    stale = tmp_path / "man"
    shutil.copytree(MAN, stale)
    (stale / "qshare.1").write_text("outdated\n", encoding="utf-8")
    result = run_builder("--check", "--out", str(stale))
    assert result.returncode == 1
    assert "outdated page: qshare.1" in result.stderr


def test_check_reports_missing_and_stale_pages(tmp_path: pathlib.Path) -> None:
    scratch = tmp_path / "man"
    scratch.mkdir()
    shutil.copy(MAN / "qrun.1", scratch / "qrun.1")
    (scratch / "ghost.1").write_text("x\n", encoding="utf-8")
    result = run_builder("--check", "--out", str(scratch))
    assert result.returncode == 1
    assert "missing page: qshare.1" in result.stderr
    assert "stale page: ghost.1" in result.stderr

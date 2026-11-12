# SPDX-License-Identifier: Apache-2.0
"""Tests of `qshare init` (gap 93, DX-01).

The scaffolder must be safe to run twice and safe to run on a directory
that already holds something: it writes the example tree, the share
options and the README, it refuses to replace a file whose content
differs, and it does nothing at all the second time.

Nothing here depends on the machine: the share is a temporary directory
and the options come from the environment, not from the configuration file
of the user running the test.
"""

from __future__ import annotations

import pathlib

import pytest

from qshare.cli import main


@pytest.fixture()
def share(tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch) -> pathlib.Path:
    """A share to scaffold, with no configuration file in the way."""
    monkeypatch.setenv("PASSERELLE_CONFIG", str(tmp_path / "absent.ini"))
    monkeypatch.setenv("PASSERELLE_COMMON_TAG", "projet")
    monkeypatch.setenv("PASSERELLE_COMMON_PROTO", "9p")
    return tmp_path / "projet"


def test_init_writes_a_ready_project(share: pathlib.Path) -> None:
    """The tree, the options, the guest snippet and the README are there."""
    assert main(["init", str(share)]) == 0
    assert (share / "cmake_example" / "CMakeLists.txt").is_file()
    assert (share / "cmake_example" / "adder.c").is_file()
    assert (share / "cmake_example" / "run_tests.sh").stat().st_mode & 0o111
    assert (share / "README.md").is_file()
    assert (share / "qemu-options.txt").is_file()
    assert (share / "guest-setup.txt").is_file()


def test_init_uses_the_shared_configuration(share: pathlib.Path) -> None:
    """The tag and the protocol come from the one configuration file."""
    assert main(["init", str(share)]) == 0
    options = (share / "qemu-options.txt").read_text(encoding="utf-8")
    assert "projet" in options
    assert "-fsdev" in options
    readme = (share / "README.md").read_text(encoding="utf-8")
    assert "projet" in readme
    assert "run_tests.sh" in readme


def test_init_guest_snippet_mounts_the_share(share: pathlib.Path) -> None:
    """The guest snippet installs the driver and mounts the share."""
    assert main(["init", str(share)]) == 0
    snippet = (share / "guest-setup.txt").read_text(encoding="utf-8")
    assert "projet" in snippet
    assert "passerelle" in snippet


def test_init_is_idempotent(share: pathlib.Path, capsys) -> None:
    """Running it twice changes nothing the second time."""
    assert main(["init", str(share)]) == 0
    before = {path: path.stat().st_mtime_ns for path in share.rglob("*") if path.is_file()}
    capsys.readouterr()
    assert main(["init", str(share)]) == 0
    out = capsys.readouterr().out
    assert "nothing to write" in out
    after = {path: path.stat().st_mtime_ns for path in share.rglob("*") if path.is_file()}
    assert after == before


def test_init_refuses_to_overwrite(share: pathlib.Path, capsys) -> None:
    """A file with a different content is never replaced silently."""
    assert main(["init", str(share)]) == 0
    readme = share / "README.md"
    readme.write_text("my own notes\n", encoding="utf-8")
    assert main(["init", str(share)]) == 2
    err = capsys.readouterr().err
    assert "refusing to overwrite" in err
    assert "README.md" in err
    assert readme.read_text(encoding="utf-8") == "my own notes\n"


def test_init_force_replaces_the_file(share: pathlib.Path) -> None:
    """--force is what allows the replacement."""
    assert main(["init", str(share)]) == 0
    readme = share / "README.md"
    readme.write_text("my own notes\n", encoding="utf-8")
    assert main(["init", str(share), "--force"]) == 0
    assert "my own notes" not in readme.read_text(encoding="utf-8")


def test_init_can_write_both_examples(share: pathlib.Path) -> None:
    """--example all writes the CMake and the Rust trees."""
    assert main(["init", str(share), "--example", "all"]) == 0
    assert (share / "cmake_example" / "CMakeLists.txt").is_file()
    assert (share / "rust_example" / "Cargo.toml").is_file()
    assert (share / "rust_example" / "src" / "lib.rs").is_file()


def test_init_creates_the_directory(share: pathlib.Path) -> None:
    """The share directory is created when it does not exist yet."""
    assert not share.exists()
    assert main(["init", str(share)]) == 0
    assert share.is_dir()


def test_init_rejects_a_file_as_share(tmp_path: pathlib.Path, capsys) -> None:
    """A path that is not a directory is an invalid input, not a crash."""
    target = tmp_path / "a-file"
    target.write_text("not a directory\n", encoding="utf-8")
    assert main(["init", str(target)]) == 2
    assert "not a directory" in capsys.readouterr().err

# SPDX-License-Identifier: Apache-2.0
"""Coherence of the packaged examples, without any compiler (gap 95, DX-04).

The examples workflow (.github/workflows/examples.yml) builds the CMake
example for real, and the Rust one when cargo exists on the runner. This
module covers the compiler-free half: both examples are scaffolded into a
temporary share with the real command (`qshare --example all`) and the trees
are checked for coherence - every file present, the CMakeLists structurally
parses and only references sources that exist, the Cargo.toml declares the
right package, and the guest launch scripts are executable.

test_examples.py exercises the writer functions one by one; this module
checks the shipped entry point and the tree a user actually gets.
"""

from __future__ import annotations

import pathlib
import re

import pytest

CMAKE_COMMAND = re.compile(r"(\w+)\s*\(([^()]*)\)")
EXPECTED_CMAKE = [
    "cmake_minimum_required",
    "project",
    "add_library",
    "add_executable",
    "target_link_libraries",
    "enable_testing",
    "add_test",
]


@pytest.fixture()
def scaffolded(share: pathlib.Path) -> pathlib.Path:
    """Scaffold both examples into the temporary share with the real CLI."""
    from qshare.cli import main

    assert main([str(share), "--example", "all"]) == 0
    return share


def cmake_commands(text: str) -> list[tuple[str, str]]:
    """Structural parse of CMake code: balanced parens, (command, args) pairs."""
    assert text.count("(") == text.count(")"), "unbalanced parentheses in CMakeLists.txt"
    return [(m.group(1).lower(), m.group(2)) for m in CMAKE_COMMAND.finditer(text)]


def test_both_trees_are_complete(scaffolded: pathlib.Path) -> None:
    cmake = scaffolded / "cmake_example"
    rust = scaffolded / "rust_example"
    for path in (
        cmake / "CMakeLists.txt", cmake / "adder.c", cmake / "adder.h",
        cmake / "adder_test.c", cmake / "run_tests.sh",
        rust / "Cargo.toml", rust / "src" / "lib.rs", rust / "run_tests.sh",
    ):
        assert path.is_file(), f"missing example file: {path}"


def test_cmake_lists_parses_and_only_references_existing_sources(
    scaffolded: pathlib.Path,
) -> None:
    base = scaffolded / "cmake_example"
    commands = cmake_commands((base / "CMakeLists.txt").read_text(encoding="utf-8"))
    names = [name for name, _ in commands]
    for expected in EXPECTED_CMAKE:
        assert expected in names, f"{expected}() missing from CMakeLists.txt"
    project = dict((n, a) for n, a in commands)["project"]
    assert "passerelle_example" in project
    sources = {"adder.c", "adder.h", "adder_test.c"}
    for _, args in commands:
        for word in args.split():
            if word in sources:
                assert (base / word).is_file(), f"CMakeLists references absent {word}"


def test_cargo_toml_declares_the_right_package(scaffolded: pathlib.Path) -> None:
    text = (scaffolded / "rust_example" / "Cargo.toml").read_text(encoding="utf-8")
    try:
        import tomllib  # Python >= 3.11

        data = tomllib.loads(text)
        assert data["package"]["name"] == "passerelle_example"
        assert data["package"]["edition"] == "2021"
        assert data["dependencies"] == {}
    except ModuleNotFoundError:  # Python 3.10 has no tomllib
        assert 'name = "passerelle_example"' in text
        assert 'edition = "2021"' in text


def test_guest_scripts_are_executable_and_self_contained(
    scaffolded: pathlib.Path,
) -> None:
    for kind, tool in (("cmake_example", "cmake"), ("rust_example", "cargo")):
        script = scaffolded / kind / "run_tests.sh"
        assert script.stat().st_mode & 0o111, f"{kind}: run_tests.sh not executable"
        text = script.read_text(encoding="utf-8")
        assert text.startswith("#!/bin/sh")
        assert tool in text


def test_rust_library_carries_its_own_test(scaffolded: pathlib.Path) -> None:
    lib = (scaffolded / "rust_example" / "src" / "lib.rs").read_text(encoding="utf-8")
    assert "pub fn add" in lib
    assert "#[cfg(test)]" in lib and "#[test]" in lib

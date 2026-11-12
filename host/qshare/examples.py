# SPDX-License-Identifier: Apache-2.0
"""Project examples for DX-04: running a CMake and a Rust test suite in the guest.

The examples are generated on demand so the share never carries a stale copy
and so the tool has no template files to keep in sync. Each example is a
complete, minimal project whose tests can be launched from the guest with
`qrun`:

    qshare --example cmake ~/projet      # writes the CMake example in the share
    qrun --console /tmp/ser.sock --share ~/projet cmake_example/run_tests.sh
    qshare --example rust ~/projet       # same for Cargo

`qshare init` scaffolds the same trees, so the tree of an example is
described once, here: `plan_example` returns what a project is made of,
the writers put it on disk.
"""

from __future__ import annotations

import pathlib
from dataclasses import dataclass

from pgwutil import ValidationError, write_if_changed

CMAKE_DIR = "cmake_example"
RUST_DIR = "rust_example"

CMAKE_LISTS = """cmake_minimum_required(VERSION 3.20)
project(passerelle_example C)

add_library(adder adder.c)
target_include_directories(adder PUBLIC .)

add_executable(adder_test adder_test.c)
target_link_libraries(adder_test PRIVATE adder)

enable_testing()
add_test(NAME adder COMMAND adder_test)
"""

CMAKE_ADDER = """/* SPDX-License-Identifier: Apache-2.0 */
/* Tiny library used by the CMake example: proves that a program built on
 * the PC runs from the share, with its shared objects, inside the guest. */
int pgw_example_add(int left, int right) { return left + right; }
"""

CMAKE_HEADER = """/* SPDX-License-Identifier: Apache-2.0 */
#ifndef PGW_EXAMPLE_ADDER_H
#define PGW_EXAMPLE_ADDER_H
int pgw_example_add(int left, int right);
#endif
"""

CMAKE_TEST = """/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include "adder.h"

int main(void) {
    if (pgw_example_add(2, 3) != 5) {
        fprintf(stderr, "adder: 2+3 != 5\\n");
        return 1;
    }
    printf("adder: ok\\n");
    return 0;
}
"""

CMAKE_SCRIPT = """#!/bin/sh
# Build on the PC, run from the share in the guest (DX-04).
set -e
cd "$(dirname "$0")"
cmake -S . -B build >/dev/null
cmake --build build >/dev/null
exec ./build/adder_test
"""

CARGO_TOML = """[package]
name = "passerelle_example"
version = "0.1.0"
edition = "2021"

[dependencies]
"""

RUST_LIB = """// SPDX-License-Identifier: Apache-2.0
//! Tiny library used by the Rust example: same purpose as the CMake one.

/// Add two integers.
pub fn add(left: i32, right: i32) -> i32 {
    left + right
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn adds() {
        assert_eq!(add(2, 3), 5);
    }
}
"""

RUST_SCRIPT = """#!/bin/sh
# Build on the PC, run from the share in the guest (DX-04).
set -e
cd "$(dirname "$0")"
exec cargo test --quiet
"""

# The files of each example, as relative path -> content.
TREES: dict[str, dict[str, str]] = {
    "cmake": {
        "CMakeLists.txt": CMAKE_LISTS,
        "adder.c": CMAKE_ADDER,
        "adder.h": CMAKE_HEADER,
        "adder_test.c": CMAKE_TEST,
        "run_tests.sh": CMAKE_SCRIPT,
    },
    "rust": {
        "Cargo.toml": CARGO_TOML,
        "src/lib.rs": RUST_LIB,
        "run_tests.sh": RUST_SCRIPT,
    },
}

# The directory each example lives in, inside the share.
DIRS = {"cmake": CMAKE_DIR, "rust": RUST_DIR}

# The files of an example that must stay executable.
EXECUTABLE = frozenset({"run_tests.sh"})


@dataclass(frozen=True)
class Example:
    """One generated example project."""

    kind: str
    directory: pathlib.Path
    files: list[pathlib.Path]

    def run_command(self) -> str:
        """Command the guest runs to execute the example's tests."""
        return f"{self.directory.name}/run_tests.sh"


def example_base(kind: str, share: pathlib.Path | str) -> pathlib.Path:
    """Directory of the example named *kind*, inside the share."""
    if kind not in TREES:
        raise ValidationError(f"unknown example '{kind}'; choose from {', '.join(TREES)}")
    base = pathlib.Path(share).expanduser().resolve() / DIRS[kind]
    if not base.parent.is_dir():
        raise ValidationError(f"share directory does not exist: {base.parent}")
    return base


def plan_example(kind: str, share: pathlib.Path | str) -> list[tuple[pathlib.Path, str]]:
    """Files of the example named *kind*, as (absolute path, content) pairs.

    The one description of an example: the writers put it on disk, and
    `qshare init` compares it with what is already there.
    """
    base = example_base(kind, share)
    return [(base / relative, content) for relative, content in TREES[kind].items()]


def _write_example(kind: str, share: pathlib.Path | str) -> Example:
    """Write the example named *kind* into the share and describe it."""
    planned = plan_example(kind, share)
    files = []
    for path, content in planned:
        write_if_changed(path, content, executable=path.name in EXECUTABLE)
        files.append(path)
    return Example(kind=kind, directory=example_base(kind, share), files=files)


def write_cmake_example(share: pathlib.Path | str) -> Example:
    """Write the CMake example project into the share."""
    return _write_example("cmake", share)


def write_rust_example(share: pathlib.Path | str) -> Example:
    """Write the Rust example project into the share."""
    return _write_example("rust", share)


EXAMPLES = {"cmake": write_cmake_example, "rust": write_rust_example}


def write_example(kind: str, share: pathlib.Path | str) -> Example:
    """Write the example named *kind* ('cmake' or 'rust')."""
    if kind not in EXAMPLES:
        raise ValidationError(f"unknown example '{kind}'; choose from {', '.join(EXAMPLES)}")
    return EXAMPLES[kind](share)


def write_all(share: pathlib.Path | str) -> list[Example]:
    """Write every example into the share."""
    return [write_example(kind, share) for kind in EXAMPLES]

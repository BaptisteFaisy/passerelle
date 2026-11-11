# SPDX-License-Identifier: Apache-2.0
"""Shared pytest configuration for the Passerelle host tools.

The tools are plain packages on disk, so the tests work without any
installation: the host directory is prepended to sys.path. Fixtures for
the QEMU trace and for a temporary share live here.
"""

from __future__ import annotations

import pathlib
import sys

import pytest

HOST_ROOT = pathlib.Path(__file__).resolve().parents[1]
if str(HOST_ROOT) not in sys.path:
    sys.path.insert(0, str(HOST_ROOT))

FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures"


@pytest.fixture()
def trace_path() -> pathlib.Path:
    """Path of the recorded 9P trace used by the roundtrip tests."""
    path = FIXTURES / "qemu-9p-trace.log"
    assert path.is_file(), f"missing trace fixture: {path}"
    return path


@pytest.fixture()
def real_trace_path() -> pathlib.Path:
    """A real QEMU 8.2 9P trace, captured from a Linux guest on this host."""
    path = FIXTURES / "qemu-9p-real-trace.log"
    assert path.is_file(), f"missing trace fixture: {path}"
    return path


@pytest.fixture()
def share(tmp_path: pathlib.Path) -> pathlib.Path:
    """A temporary directory that plays the role of the shared folder."""
    directory = tmp_path / "share"
    directory.mkdir()
    return directory

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


@pytest.fixture(autouse=True)
def isolated_config(tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Keep the shared configuration file of the four tools out of the tests.

    `pgwutil.config` reads $PASSERELLE_CONFIG, else
    $XDG_CONFIG_HOME/passerelle/passerelle.ini. A test must never depend on
    what the developer of this machine happens to have configured, so both
    are redirected to a file that does not exist; a test that wants a
    configuration writes one and sets PASSERELLE_CONFIG itself (gap 99).
    """
    monkeypatch.setenv("PASSERELLE_CONFIG", str(tmp_path / "absent-passerelle.ini"))
    monkeypatch.delenv("XDG_CONFIG_HOME", raising=False)

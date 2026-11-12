# SPDX-License-Identifier: Apache-2.0
"""Tests for the packaging of the host tools: pipx and apt (gap 96, DX-05).

Two audiences, two contracts, both checked statically so the 5-minute CI
budget survives:

  * pipx / pip: `pip install ./host` must yield working console tools. The
    release workflow's `install-check` job already performs a real venv
    install; these tests add the missing cheap half - every console script
    declared in `host/pyproject.toml` must import and expose a callable
    `main`, and the declared version must equal the VERSION file (the
    version check itself is delegated to tools/release/releaseinfo.py).
  * apt: the `packaging/debian/` skeleton must be reviewable and coherent -
    required control fields, a changelog version equal to VERSION, existing
    paths in debian/install, dh+pybuild rules, a Format-1.0 Apache-2.0
    copyright. A real `dpkg-buildpackage` is out of CI scope (see
    packaging/README.md).
"""

from __future__ import annotations

import importlib
import pathlib
import re
import sys

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
HOST_DIR = REPO_ROOT / "host"
PACKAGING = REPO_ROOT / "packaging" / "debian"

sys.path.insert(0, str(REPO_ROOT / "tools" / "release"))
import releaseinfo  # noqa: E402  (import after the sys.path fix above)

EXPECTED_SCRIPTS = {"qshare", "qrun", "fsproof", "pbench"}


def console_scripts() -> dict[str, str]:
    """Return the {name: "module:function"} table of [project.scripts]."""
    text = (HOST_DIR / "pyproject.toml").read_text(encoding="utf-8")
    try:  # Python 3.11+: the real parser.
        import tomllib

        data = tomllib.loads(text)
        return dict(data["project"]["scripts"])
    except ImportError:
        pass
    section = re.search(
        r"^\[project\.scripts\]\n(.*?)(?=^\[|\Z)", text, re.MULTILINE | re.DOTALL
    )
    assert section, "host/pyproject.toml has no [project.scripts] section"
    return dict(re.findall(r'^(\w[\w-]*)\s*=\s*"([^"]+)"', section.group(1), re.MULTILINE))


def control_fields() -> tuple[dict[str, str], dict[str, str]]:
    """Return (source stanza fields, binary stanza fields) of debian/control."""
    text = (PACKAGING / "control").read_text(encoding="utf-8")
    source: dict[str, str] = {}
    binary: dict[str, str] = {}
    target = source
    key = ""
    for line in text.splitlines():
        if not line.strip():
            continue
        if line[0] in " \t":  # continuation of the previous field
            target[key] += " " + line.strip()
            continue
        match = re.match(r"^([A-Za-z0-9-]+):\s*(.*)$", line)
        assert match, f"debian/control: unparsable line {line!r}"
        key, value = match.group(1), match.group(2)
        if key == "Package":
            target = binary
        target[key] = value
    return source, binary


def test_console_scripts_importable_and_expose_main() -> None:
    """Every declared console tool imports and its entry point is callable."""
    scripts = console_scripts()
    assert set(scripts) == EXPECTED_SCRIPTS, f"console scripts drifted: {sorted(scripts)}"
    for name, target in scripts.items():
        module_name, _, func_name = target.partition(":")
        assert func_name, f"{name}: entry point {target!r} has no :function part"
        module = importlib.import_module(module_name)
        func = getattr(module, func_name, None)
        assert callable(func), f"{name}: {module_name}.{func_name} is not callable"


def test_pyproject_version_equals_version_file() -> None:
    """The pipx-visible version is the VERSION file (releaseinfo owns the read)."""
    assert releaseinfo.pyproject_version(REPO_ROOT) == releaseinfo.read_version(REPO_ROOT)


@pytest.fixture(scope="module")
def stanzas() -> tuple[dict[str, str], dict[str, str]]:
    """The parsed debian/control, once for the module."""
    assert (PACKAGING / "control").is_file(), "missing packaging/debian/control"
    return control_fields()


def test_control_source_fields(stanzas: tuple[dict[str, str], dict[str, str]]) -> None:
    """The source stanza carries the fields a reviewable package needs."""
    source, _ = stanzas
    for field in ("Source", "Section", "Priority", "Maintainer",
                  "Standards-Version", "Build-Depends", "Rules-Requires-Root"):
        assert source.get(field), f"debian/control: source stanza lacks {field}"
    assert re.match(r"^\d+\.\d+\.\d+$", source["Standards-Version"])
    assert source["Rules-Requires-Root"] == "no"
    assert "debhelper-compat" in source["Build-Depends"]


def test_control_binary_fields(stanzas: tuple[dict[str, str], dict[str, str]]) -> None:
    """python3 in Depends; cmake and qemu-system-x86 in Recommends."""
    _, binary = stanzas
    assert binary.get("Package") == "passerelle-host"
    assert binary.get("Architecture") == "all"
    depends = [dep.strip() for dep in binary["Depends"].split(",")]
    assert "python3" in depends and "${python3:Depends}" in depends
    recommends = [dep.strip() for dep in binary.get("Recommends", "").split(",")]
    assert "cmake" in recommends, "cmake belongs in Recommends (CONTRIBUTING §1)"
    assert "qemu-system-x86" in recommends, "QEMU belongs in Recommends (CONTRIBUTING §1)"
    assert binary.get("Description"), "the binary stanza needs a Description"


def test_changelog_version_equals_version_file() -> None:
    """debian/changelog's native version is exactly VERSION."""
    text = (PACKAGING / "changelog").read_text(encoding="utf-8")
    match = re.match(r"^(\S+) \(([^)]+)\) (\S+); urgency=(\S+)\n", text)
    assert match, "debian/changelog does not start with a valid header line"
    version = match.group(2)
    assert "-" not in version, f"{version}: a native package carries no debian revision"
    assert version == releaseinfo.read_version(REPO_ROOT), (
        f"debian/changelog says {version}, VERSION says {releaseinfo.read_version(REPO_ROOT)}"
    )
    # RFC-2822-ish trailer date, the part dpkg-parsechangelog is strict about.
    trailer = re.search(r"^ -- (.+?) <[^>]+>\s+(\w{3}, \d{2} \w{3} \d{4} "
                        r"\d{2}:\d{2}:\d{2} [+-]\d{4})\s*$", text, re.MULTILINE)
    assert trailer, "debian/changelog lacks a valid ' -- Maintainer  Date' trailer"


def test_install_paths_exist() -> None:
    """Every source path of debian/install resolves in the repository."""
    install = PACKAGING / "install"
    if not install.is_file():
        pytest.skip("no debian/install: pybuild installs the tools (documented choice)")
    for line in install.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        source = line.split()[0]
        matches = list(REPO_ROOT.glob(source))
        assert matches, f"debian/install: {source!r} matches nothing in the repository"


def test_rules_uses_dh_pybuild_and_is_executable() -> None:
    """debian/rules is an executable dh+pybuild makefile pointed at host/."""
    rules = PACKAGING / "rules"
    assert rules.is_file(), "missing debian/rules"
    assert rules.stat().st_mode & 0o111, "debian/rules must be executable"
    text = rules.read_text(encoding="utf-8")
    assert "dh $@" in text and "pybuild" in text
    assert "host" in text, "pybuild must be pointed at host/ (where pyproject.toml lives)"
    assert "PYBUILD_NAME = passerelle-host" in text


def test_copyright_is_format_one_apache() -> None:
    """debian/copyright follows the machine-readable Format 1.0, Apache-2.0."""
    text = (PACKAGING / "copyright").read_text(encoding="utf-8")
    assert text.startswith(
        "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
    )
    assert "License: Apache-2.0" in text
    assert (PACKAGING / "source" / "format").read_text(encoding="utf-8").strip() == "3.0 (native)"

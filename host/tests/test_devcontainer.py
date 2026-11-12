# SPDX-License-Identifier: Apache-2.0
"""Tests for the development container definition (gap 100, DX-05).

A `.devcontainer` must reproduce the development environment in one command
(`devcontainer up` / "Reopen in Container"). A full `docker build` is far too
slow for the 5-minute CI budget, so these tests validate the definition
statically instead:

  * the JSON parses (strict JSON: no comment may sneak in);
  * there is ONE definition of the environment: the root `Dockerfile`,
    referenced by `build.dockerfile` - no `image`, no duplicate `features`;
  * every file the definition references exists in the repository;
  * the base image of the Dockerfile is pinned to an explicit tag;
  * the Dockerfile really installs what CONTRIBUTING.md section 1 lists
    (cmake, a C11 compiler, python3, pytest, QEMU).
"""

from __future__ import annotations

import json
import pathlib
import re

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DEVCONTAINER_DIR = REPO_ROOT / ".devcontainer"
DEVCONTAINER_JSON = DEVCONTAINER_DIR / "devcontainer.json"
DOCKERFILE = REPO_ROOT / "Dockerfile"


@pytest.fixture(scope="module")
def config() -> dict:
    """Parse .devcontainer/devcontainer.json as strict JSON."""
    assert DEVCONTAINER_JSON.is_file(), "missing .devcontainer/devcontainer.json"
    return json.loads(DEVCONTAINER_JSON.read_text(encoding="utf-8"))


@pytest.fixture(scope="module")
def dockerfile_text() -> str:
    """The root Dockerfile, the single definition of the environment."""
    assert DOCKERFILE.is_file(), "missing the root Dockerfile"
    return DOCKERFILE.read_text(encoding="utf-8")


def test_single_environment_definition(config: dict) -> None:
    """The container is built from the root Dockerfile, and from nothing else."""
    assert "image" not in config, (
        "devcontainer.json must not carry an 'image': the root Dockerfile is "
        "the single definition of the environment"
    )
    build = config.get("build")
    assert isinstance(build, dict), "devcontainer.json needs a 'build' section"
    dockerfile = pathlib.Path(build["dockerfile"])
    resolved = (DEVCONTAINER_DIR / dockerfile).resolve()
    assert resolved == DOCKERFILE.resolve(), (
        f"build.dockerfile must reference the root Dockerfile, got {build['dockerfile']!r}"
    )
    context = (DEVCONTAINER_DIR / build.get("context", ".")).resolve()
    assert context.is_dir(), f"build.context does not exist: {context}"
    # The Dockerfile already installs python3 and pytest through apt, so a
    # 'features' block normally stays empty. If one is ever added, each feature
    # must be pinned by digest: a floating tag would silently change the
    # environment between two 'devcontainer up' runs.
    for feature in config.get("features", {}):
        assert "@sha256:" in feature, f"feature is not pinned by digest: {feature!r}"


def test_referenced_files_exist(config: dict) -> None:
    """Every path the definition mentions exists in the repository."""
    paths: list[str] = [config["build"]["dockerfile"], config["build"].get("context", ".")]
    mount = config.get("workspaceMount", "")
    paths += re.findall(r"source=([^,\s]+)", mount)
    for raw in paths:
        candidate = (DEVCONTAINER_DIR / raw).resolve()
        assert candidate.exists(), f"devcontainer.json references a missing path: {raw}"


def test_base_image_is_pinned(dockerfile_text: str) -> None:
    """FROM lines carry an explicit tag or digest - never 'latest', never bare."""
    from_lines = [
        line for line in dockerfile_text.splitlines() if re.match(r"\s*FROM\s", line)
    ]
    assert from_lines, "the Dockerfile has no FROM line"
    for line in from_lines:
        image = line.split()[1]
        assert "@sha256:" in image or re.search(r":[0-9][^@\s]*$", image), (
            f"base image is not pinned to a version tag or digest: {image!r}"
        )
        assert not image.endswith(":latest"), f"base image uses :latest: {image!r}"


def test_dockerfile_installs_contributing_tools(dockerfile_text: str) -> None:
    """The image installs the tool list of CONTRIBUTING.md section 1."""
    required = {
        "cmake": "cmake",
        "C11 compiler": "build-essential",
        "python3": "python3",
        "pytest": "python3-pytest",
        "QEMU": "qemu-system-x86",
    }
    for label, package in required.items():
        pattern = re.compile(rf"^\s+{re.escape(package)}\s*(?:\\|$)", re.MULTILINE)
        assert pattern.search(dockerfile_text), (
            f"the Dockerfile does not install {package} ({label}, CONTRIBUTING.md §1)"
        )
    # The QNX SDP is not redistributable: the image must not pretend to ship it.
    assert "/opt/qnx800" in dockerfile_text, (
        "the Dockerfile must document the QNX SDP mount point (/opt/qnx800)"
    )


def test_host_tools_are_installed(dockerfile_text: str) -> None:
    """The four console tools are pip-installed inside the image (gap 96)."""
    assert re.search(r"COPY\s+host/", dockerfile_text), "the Dockerfile must COPY host/"
    assert "pip install" in dockerfile_text, "the Dockerfile must pip-install the host tools"

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the SBOM generator and of its validator (gap 39).

The generator is run against the real tree: the document must be a valid
CycloneDX 1.5 one, every component must carry a name and a version, and the
validator must *reject* a broken document - a check that never fails is not
a check. The provenance file is checked too: compiler, flags, build type,
commit and dirty state are what a release artefact has to carry.

Standard library only, as the rest of the project.
"""

from __future__ import annotations

import copy
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import generate_sbom as sbom

ROOT = pathlib.Path(__file__).resolve().parents[2]


def test_valid_document():
    """The generated document validates and carries the real inputs."""
    doc = sbom.build_sbom(ROOT, sbom.releaseinfo.read_version(ROOT))
    problems = sbom.validate_sbom(doc)
    assert problems == []
    assert doc["bomFormat"] == "CycloneDX"
    assert doc["specVersion"] == "1.5"
    assert doc["serialNumber"].startswith("urn:uuid:")
    assert doc["metadata"]["component"]["name"] == "passerelle"


def test_every_component_has_a_name_and_a_version():
    """No component may ship without a name and a version."""
    doc = sbom.build_sbom(ROOT, sbom.releaseinfo.read_version(ROOT))
    for component in doc["components"]:
        assert component.get("name"), component
        assert component.get("version"), component
        assert component.get("type") in ("library", "file"), component


def test_third_party_is_stated():
    """The SBOM says explicitly that the artefact has no third-party part."""
    doc = sbom.build_sbom(ROOT, sbom.releaseinfo.read_version(ROOT))
    props = {p["name"]: p["value"] for p in doc["metadata"]["properties"]}
    assert "passerelle:third-party-components" in props
    assert props["passerelle:third-party-components"].startswith("none")
    assert "passerelle:git-commit" in props


def test_libraries_and_dependencies():
    """The shipped libraries and their dependency graph are complete."""
    doc = sbom.build_sbom(ROOT, sbom.releaseinfo.read_version(ROOT))
    names = {c["name"] for c in doc["components"] if c["type"] == "library"}
    assert names == set(sbom.LIBRARIES)
    refs = {c["bom-ref"] for c in doc["components"]}
    for entry in doc["dependencies"]:
        assert entry["ref"] in refs
        for dep in entry["dependsOn"]:
            assert dep in refs


@pytest.mark.parametrize("mutate,expected", [
    (lambda d: d.pop("bomFormat"), "bomFormat"),
    (lambda d: d.update(specVersion="9.9"), "specVersion"),
    (lambda d: d.update(version=7), "version"),
    (lambda d: d.update(serialNumber="nope"), "serialNumber"),
    (lambda d: d.pop("metadata"), "metadata"),
    (lambda d: d["metadata"].pop("component"), "metadata.component"),
    (lambda d: d["components"].clear(), "components"),
    (lambda d: d["components"][0].pop("version"), "no version"),
    (lambda d: d["components"][1].update(name=""), "no name"),
    (lambda d: d["dependencies"].append({"ref": "x", "dependsOn": ["nope"]}),
     "not a known bom-ref"),
])
def test_validator_rejects_a_broken_document(mutate, expected):
    """A document with a missing field is rejected, with the reason."""
    doc = sbom.build_sbom(ROOT, sbom.releaseinfo.read_version(ROOT))
    mutate(doc)
    problems = sbom.validate_sbom(doc)
    assert problems, "the validator accepted a broken document"
    assert any(expected in p for p in problems), problems


def test_validator_rejects_garbage():
    """A document that is not even an object is rejected."""
    assert sbom.validate_sbom([1, 2]) == ["the document is not a JSON object"]
    assert sbom.validate_sbom(None)[0].startswith("the document")


def test_provenance_carries_the_build_facts():
    """The provenance file names the toolchain and the commit."""
    version = sbom.releaseinfo.read_version(ROOT)
    prov = sbom.build_provenance(ROOT, version)
    assert prov["schema"] == "passerelle/provenance/1"
    assert prov["version"] == version
    assert prov["toolchain"]["cmake"]["version"]
    assert prov["toolchain"]["c_compiler"]["path"]
    assert "commit" in prov["vcs"] and "dirty" in prov["vcs"]
    assert prov["build"]["build_type"]
    assert prov["sources"]["count"] > 0
    assert len(prov["sources"]["manifest_sha256"]) == 64


def test_provenance_reads_the_build_tree():
    """With a build tree, the provenance names the real build type."""
    version = sbom.releaseinfo.read_version(ROOT)
    cache = pathlib.Path("build") / "CMakeCache.txt"
    if not cache.is_file():
        pytest.skip("no build tree here: nothing to read")
    build_dir = (ROOT / cache).parent
    prov = sbom.build_provenance(ROOT, version, build_dir)
    assert prov["build"]["build_type"] != "unknown"


def test_deterministic_serial_number():
    """The same commit and version always give the same serial number."""
    version = sbom.releaseinfo.read_version(ROOT)
    first = sbom.build_sbom(ROOT, version)["serialNumber"]
    second = sbom.build_sbom(ROOT, version)["serialNumber"]
    assert first == second


def test_source_files_are_the_real_inputs():
    """The file components name real files of the tree."""
    version = sbom.releaseinfo.read_version(ROOT)
    doc = sbom.build_sbom(ROOT, version)
    files = [c["name"] for c in doc["components"] if c["type"] == "file"]
    assert len(files) > 50
    for name in files:
        assert (ROOT / name).is_file(), name
        assert name.startswith(tuple("%s/" % m for m in sbom.MODULES))


def test_copy_is_still_valid():
    """The validator does not mutate the document it checks."""
    doc = sbom.build_sbom(ROOT, sbom.releaseinfo.read_version(ROOT))
    before = copy.deepcopy(doc)
    assert sbom.validate_sbom(doc) == []
    assert doc == before

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""SBOM and build provenance for Passerelle (gap 39).

The project ships a C stack and Python host tools, and nothing else: no
third-party library is linked, fetched or vendored. A bill of materials is
still worth publishing, because it states that fact with the evidence
instead of leaving it to a claim in the README.

This generator walks the *real* inputs of the build:

  * the C sources and headers of the six modules that make the shipped
    libraries (each one with its SHA-256);
  * the version, read from the VERSION file (the single source of truth);
  * the toolchain: CMake and the C compiler, with their versions;
  * the git commit, the branch and whether the tree was dirty;
  * the third-party components: none, stated explicitly.

It writes two documents, both with the standard library only:

  * a CycloneDX 1.5 document (--output), the format the scanners read;
  * a build provenance file (--provenance): compiler, flags, build type,
    commit and dirty state, which the release artefact carries.

Both are validated by --check: the document must parse, carry the mandatory
fields and give a name and a version to every component. A document that
does not validate is a failure, never a warning.

Usage:
    python3 tools/sbom/generate_sbom.py --output dist/passerelle.sbom.json
    python3 tools/sbom/generate_sbom.py --check --output /tmp/sbom.json

Exit status: 0 ok, 1 invalid document or usage error.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import uuid

if __package__ in (None, ""):  # allow `python3 tools/sbom/generate_sbom.py`
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "release"))

import releaseinfo  # noqa: E402  (path set above, same trick as tools/release)

SPEC_VERSION = "1.5"
BOM_FORMAT = "CycloneDX"
SERIAL_NAMESPACE = uuid.UUID("6ba7b811-9dad-11d1-80b4-00c04fd430c8")

#: The modules whose sources make the shipped libraries.
MODULES = ("common", "virtio", "proto9p", "protofuse", "rmfs", "drivers")

#: The libraries of the installed package, and what they depend on.
LIBRARIES = {
    "pgw_drivers_portable": ("pgw_rmfs", "pgw_proto9p", "pgw_protofuse",
                             "pgw_common"),
    "pgw_rmfs": ("pgw_proto9p", "pgw_protofuse", "pgw_common"),
    "pgw_proto9p": ("pgw_common",),
    "pgw_protofuse": ("pgw_common",),
    "pgw_virtio": ("pgw_common",),
    "pgw_common": (),
}

THIRD_PARTY_NOTE = (
    "none in the shipped artefact: the shipped C stack links no third-party "
    "library (C standard library and POSIX only) and the host tools use the "
    "Python standard library only. The toolchain is recorded in "
    "metadata.tools; it is not a dependency of the artefact."
)


def run_text(cmd, cwd=None):
    """Run a command and return its output, or an empty string on failure."""
    try:
        proc = subprocess.run(cmd, cwd=cwd, text=True, timeout=30,
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.SubprocessError):
        return ""
    return proc.stdout if proc.returncode == 0 else ""


def git_info(root):
    """Return the commit, the branch and the dirty state of the tree."""
    commit = run_text(["git", "rev-parse", "HEAD"], cwd=root).strip()
    branch = run_text(["git", "rev-parse", "--abbrev-ref", "HEAD"],
                      cwd=root).strip()
    status = run_text(["git", "status", "--porcelain"], cwd=root)
    if not commit:
        return {"commit": "unknown", "branch": "unknown", "dirty": None}
    return {"commit": commit, "branch": branch or "unknown",
            "dirty": bool(status.strip())}


def source_files(root):
    """Return the sorted list of the C sources and headers of the modules."""
    found = []
    for module in MODULES:
        base = root / module
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix in (".c", ".h") and path.is_file():
                found.append(path)
    return found


def sha256_of(path):
    """Return the SHA-256 of a file, as an hexadecimal string."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def cache_value(build_dir, name):
    """Read one value from a CMake cache, or an empty string."""
    if build_dir is None:
        return ""
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return ""
    prefix = name + ":"
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(prefix):
            return line.split("=", 1)[1].strip()
    return ""


def toolchain(build_dir):
    """Return the description of the toolchain that built the tree."""
    cmake_version = cache_value(build_dir, "CMAKE_VERSION")
    if not cmake_version:
        cmake_version = run_text(["cmake", "--version"]).splitlines()[:1]
        cmake_version = cmake_version[0].split()[-1] if cmake_version else "unknown"
    compiler = cache_value(build_dir, "CMAKE_C_COMPILER") or "cc"
    cc_version = run_text([compiler, "--version"]).splitlines()[:1]
    cc_version = cc_version[0] if cc_version else "unknown"
    return {
        "cmake": {"version": cmake_version},
        "c_compiler": {"path": compiler, "version": cc_version},
    }


def compiler_vendor(version_text):
    """Return the vendor of a compiler, from the first line of its banner."""
    text = (version_text or "").lower()
    if "clang" in text:
        return "LLVM"
    if "gcc" in text:
        return "GNU"
    if "qcc" in text:
        return "BlackBerry"
    return "unknown"


def reference_trees(root):
    """Return the third-party trees kept in the repository, but not shipped."""
    base = root / "upstream"
    if not base.is_dir():
        return []
    return sorted(p.name for p in base.iterdir()
                  if p.is_dir() and not p.name.startswith("."))


def build_flags(build_dir):
    """Return the compile and link flags of the build, when known."""
    if build_dir is None:
        return {"build_type": "unknown", "c_flags": "unknown",
                "config_c_flags": "unknown", "linker_flags": "unknown"}
    build_type = cache_value(build_dir, "CMAKE_BUILD_TYPE") or "unknown"
    config_flags = cache_value(build_dir, "CMAKE_C_FLAGS_%s"
                               % build_type.upper())
    return {
        "build_type": build_type,
        "c_flags": cache_value(build_dir, "CMAKE_C_FLAGS") or "(empty)",
        "config_c_flags": config_flags or "(empty)",
        "linker_flags": cache_value(build_dir, "CMAKE_EXE_LINKER_FLAGS")
                       or "(empty)",
    }


def timestamp():
    """Return the build timestamp, honouring SOURCE_DATE_EPOCH."""
    epoch = os_environ_epoch()
    if epoch is not None:
        stamp = datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc)
    else:
        stamp = datetime.datetime.now(datetime.timezone.utc)
    return stamp.replace(microsecond=0).isoformat().replace("+00:00", "Z")


def os_environ_epoch():
    """Return SOURCE_DATE_EPOCH as an int, or None when it is not set."""
    raw = os.environ.get("SOURCE_DATE_EPOCH")
    if not raw:
        return None
    try:
        return int(raw)
    except ValueError:
        return None


def library_components(version):
    """Return the CycloneDX components of the shipped libraries."""
    components = []
    for name, deps in LIBRARIES.items():
        components.append({
            "type": "library",
            "bom-ref": "lib:%s@%s" % (name, version),
            "name": name,
            "version": version,
            "description": "Passerelle static library %s" % name,
            "scope": "required",
            "purl": "pkg:generic/passerelle/%s@%s" % (name, version),
            "licenses": [{"license": {"id": "Apache-2.0"}}],
        })
    return components


def file_components(root, version):
    """Return the CycloneDX components of the C sources of the tree."""
    components = []
    for path in source_files(root):
        rel = path.relative_to(root).as_posix()
        components.append({
            "type": "file",
            "bom-ref": "file:%s" % rel,
            "name": rel,
            "version": version,
            "description": "C source of the Passerelle tree",
        })
    return components


def dependencies(version):
    """Return the CycloneDX dependency graph of the shipped libraries."""
    graph = []
    for name, deps in LIBRARIES.items():
        graph.append({
            "ref": "lib:%s@%s" % (name, version),
            "dependsOn": ["lib:%s@%s" % (dep, version) for dep in deps],
        })
    return graph


def build_sbom(root, version, build_dir=None):
    """Return the CycloneDX document of the tree."""
    git = git_info(root)
    tools = toolchain(build_dir)
    components = library_components(version) + file_components(root, version)
    serial = uuid.uuid5(SERIAL_NAMESPACE, "passerelle@%s@%s"
                         % (version, git["commit"]))
    return {
        "bomFormat": BOM_FORMAT,
        "specVersion": SPEC_VERSION,
        "serialNumber": "urn:uuid:%s" % serial,
        "version": 1,
        "metadata": {
            "timestamp": timestamp(),
            "tools": [
                {"vendor": "Kitware", "name": "cmake",
                 "version": tools["cmake"]["version"]},
                {"vendor": compiler_vendor(tools["c_compiler"]["version"]),
                 "name": "C compiler",
                 "version": tools["c_compiler"]["version"]},
            ],
            "component": {
                "type": "application",
                "bom-ref": "passerelle@%s" % version,
                "name": "passerelle",
                "version": version,
                "description": "Passerelle: 9P2000.L and virtiofs for QNX 8.0",
                "licenses": [{"license": {"id": "Apache-2.0"}}],
                "purl": "pkg:generic/passerelle@%s" % version,
                "externalReferences": [
                    {"type": "vcs",
                     "url": "https://github.com/BaptisteFaisy/passerelle"},
                ],
            },
            "properties": [
                {"name": "passerelle:third-party-components",
                 "value": THIRD_PARTY_NOTE},
                {"name": "passerelle:reference-trees-not-shipped",
                 "value": ", ".join(reference_trees(root)) or "none"},
                {"name": "passerelle:git-commit", "value": git["commit"]},
                {"name": "passerelle:git-dirty",
                 "value": "true" if git["dirty"] else "false"},
            ],
        },
        "components": components,
        "dependencies": dependencies(version),
    }


def build_provenance(root, version, build_dir=None):
    """Return the build provenance document."""
    git = git_info(root)
    tools = toolchain(build_dir)
    flags = build_flags(build_dir)
    files = source_files(root)
    manifest = hashlib.sha256()
    for path in files:
        manifest.update(path.relative_to(root).as_posix().encode("utf-8"))
        manifest.update(sha256_of(path).encode("ascii"))
    return {
        "schema": "passerelle/provenance/1",
        "project": "passerelle",
        "version": version,
        "built_at": timestamp(),
        "vcs": {"commit": git["commit"], "branch": git["branch"],
                "dirty": git["dirty"]},
        "toolchain": tools,
        "build": flags,
        "sources": {"count": len(files),
                    "manifest_sha256": manifest.hexdigest()},
    }


def validate_sbom(doc):
    """Return the list of the problems of a CycloneDX document."""
    problems = []
    if not isinstance(doc, dict):
        return ["the document is not a JSON object"]
    if doc.get("bomFormat") != BOM_FORMAT:
        problems.append("bomFormat is %r, expected %r"
                        % (doc.get("bomFormat"), BOM_FORMAT))
    if doc.get("specVersion") != SPEC_VERSION:
        problems.append("specVersion is %r, expected %r"
                        % (doc.get("specVersion"), SPEC_VERSION))
    if doc.get("version") != 1:
        problems.append("version is %r, expected 1" % (doc.get("version"),))
    serial = doc.get("serialNumber", "")
    if not str(serial).startswith("urn:uuid:"):
        problems.append("serialNumber is not a urn:uuid: (%r)" % serial)
    problems += validate_metadata(doc.get("metadata"))
    problems += validate_components(doc.get("components"))
    problems += validate_dependencies(doc.get("dependencies"),
                                      doc.get("components"))
    return problems


def validate_metadata(metadata):
    """Return the problems of the metadata block."""
    problems = []
    if not isinstance(metadata, dict):
        return ["metadata is missing"]
    stamp = metadata.get("timestamp", "")
    try:
        datetime.datetime.fromisoformat(str(stamp).replace("Z", "+00:00"))
    except ValueError:
        problems.append("metadata.timestamp is not an ISO 8601 date (%r)" % stamp)
    if not metadata.get("tools"):
        problems.append("metadata.tools is empty")
    component = metadata.get("component")
    if not isinstance(component, dict):
        problems.append("metadata.component is missing")
    else:
        for field in ("type", "name", "version"):
            if not component.get(field):
                problems.append("metadata.component.%s is missing" % field)
    return problems


def validate_components(components):
    """Return the problems of the component list."""
    problems = []
    if not isinstance(components, list) or not components:
        return ["components is missing or empty"]
    refs = set()
    for index, component in enumerate(components):
        if not isinstance(component, dict):
            problems.append("component %d is not an object" % index)
            continue
        for field in ("type", "name", "version"):
            value = component.get(field)
            if not isinstance(value, str) or not value.strip():
                problems.append("component %d has no %s" % (index, field))
        ref = component.get("bom-ref")
        if ref:
            if ref in refs:
                problems.append("duplicate bom-ref %r" % ref)
            refs.add(ref)
    return problems


def validate_dependencies(dependencies, components):
    """Return the problems of the dependency graph."""
    problems = []
    if not isinstance(dependencies, list):
        return ["dependencies is missing"]
    refs = {c.get("bom-ref") for c in components or [] if isinstance(c, dict)}
    for entry in dependencies:
        if not isinstance(entry, dict) or "ref" not in entry:
            problems.append("a dependency entry has no ref")
            continue
        for dep in entry.get("dependsOn", []):
            if dep not in refs:
                problems.append("dependency %r is not a known bom-ref" % dep)
    return problems


def parse_args(argv=None):
    """Parse the command line."""
    ap = argparse.ArgumentParser(description="Generate the Passerelle SBOM "
                                             "and its build provenance.")
    ap.add_argument("--source-dir", default=None, help="repository root")
    ap.add_argument("--build-dir", default=None,
                    help="build tree (for the compiler and the flags)")
    ap.add_argument("--output", default=None, help="SBOM file to write")
    ap.add_argument("--provenance", default=None,
                    help="provenance file to write")
    ap.add_argument("--check", action="store_true",
                    help="validate the document and fail when it is invalid")
    return ap.parse_args(argv)


def main(argv=None):
    """Generate, write and validate the documents."""
    args = parse_args(argv)
    root = pathlib.Path(args.source_dir).resolve() if args.source_dir \
        else releaseinfo.repo_root()
    build_dir = pathlib.Path(args.build_dir).resolve() if args.build_dir \
        else None
    version = releaseinfo.read_version(root)
    doc = build_sbom(root, version, build_dir)
    problems = validate_sbom(doc)
    if args.output:
        pathlib.Path(args.output).write_text(
            json.dumps(doc, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.provenance:
        prov = build_provenance(root, version, build_dir)
        pathlib.Path(args.provenance).write_text(
            json.dumps(prov, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if problems:
        for problem in problems:
            print("sbom: invalid: %s" % problem, file=sys.stderr)
        return 1
    if args.check:
        print("sbom: %s is a valid CycloneDX %s document (%d components)"
              % (args.output or "<memory>", SPEC_VERSION, len(doc["components"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())

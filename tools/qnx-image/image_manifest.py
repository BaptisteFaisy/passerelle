#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""image_manifest.py - the QNX 7.1 guest disk, pinned and verified (gap 74).

Why this exists: the guest disk of the CI job is QNX software, so it is not in
the repository (rule "no binary in the repository"). It is fetched at run time
from a public community container. Without a pin, "the disk CI used" is a
moving target: the tag can be re-pushed, and nobody can tell whether the disk
they boot is the one the project measured.

The pin lives in `qnx71-image.manifest.json`, next to this file:

  - `source` - the container, by **digest** (the digest is the checksum of the
    manifest of the image: docker verifies every layer against it during the
    pull, so pulling by digest is pulling by checksum);
  - `disk` - the file extracted out of that container: its path inside the
    container, its size and its SHA256.

This tool is the only place that reads the manifest, so the shell script that
fetches the disk, the CI job and the unit tests all agree on what "the pinned
image" means.

Standard library only. Usage:

    image_manifest.py verify --manifest M --file F [--strict]
    image_manifest.py digest --file F
    image_manifest.py show --manifest M [--field FIELD]
    image_manifest.py pin --manifest M --file F

Exit status of `verify`:
    0  the file matches the pinned size and SHA256
    1  mismatch: the size or the digest is not the pinned one (loud failure)
    2  usage error, or the manifest cannot be read
    3  the manifest pins no SHA256 yet: the observed digest is printed
       (`--strict` turns this into a failure, which is what CI asks for once
       the digest is pinned)
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import sys
from pathlib import Path

EXIT_OK = 0
EXIT_MISMATCH = 1
EXIT_USAGE = 2
EXIT_UNPINNED = 3

CHUNK = 4 * 1024 * 1024
FIELDS = ("docker-reference", "digest", "disk-sha256", "disk-size",
          "disk-path", "verified-on", "reference-by-tag")


def sha256_of(path: Path) -> str:
    """SHA256 of a file, read by blocks (the disk is 6.1 GiB)."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def load_manifest(path: Path) -> dict:
    """Read the manifest and check it has the sections this tool needs."""
    with path.open(encoding="utf-8") as handle:
        data = json.load(handle)
    for section in ("source", "disk"):
        if section not in data:
            raise ValueError("manifest %s has no %r section" % (path, section))
    source = data["source"]
    for key in ("registry", "repository", "digest"):
        if key not in source:
            raise ValueError("manifest %s: source has no %r" % (path, key))
    return data


def docker_reference(manifest: dict) -> str:
    """The reference to pull: the image by digest, never by tag alone."""
    source = manifest["source"]
    return "%s/%s@%s" % (source["registry"], source["repository"],
                         source["digest"])


def docker_tag_reference(manifest: dict) -> str:
    """The human-readable reference (repository:tag), for messages."""
    source = manifest["source"]
    return "%s/%s:%s" % (source["registry"], source["repository"],
                         source.get("tag", "latest"))


def field_value(manifest: dict, field: str) -> str:
    """One field of the manifest, for `show --field`."""
    source = manifest["source"]
    disk = manifest["disk"]
    if field == "docker-reference":
        return docker_reference(manifest)
    if field == "reference-by-tag":
        return docker_tag_reference(manifest)
    if field == "digest":
        return str(source["digest"])
    if field == "disk-sha256":
        return str(disk.get("sha256") or "")
    if field == "disk-size":
        return str(disk.get("size_bytes", ""))
    if field == "disk-path":
        return str(disk.get("path_in_source", ""))
    if field == "verified-on":
        return str(manifest.get("verified_on", ""))
    raise ValueError("unknown field %r" % field)


def compare(manifest: dict, path: Path, strict: bool = False) -> tuple[int, str]:
    """Compare a file with the pinned disk entry: (exit code, message).

    `strict` turns "the manifest pins no SHA256 yet" (exit 3, the caller may
    accept it) into a failure (exit 1), which is what CI asks for once the
    digest is pinned and recorded.
    """
    disk = manifest["disk"]
    expected_size = int(disk["size_bytes"])
    expected_sha = str(disk.get("sha256") or "")
    if not path.is_file():
        return EXIT_MISMATCH, "%s does not exist" % path
    actual_size = path.stat().st_size
    if actual_size != expected_size:
        return EXIT_MISMATCH, (
            "size mismatch: the manifest pins %d bytes, %s has %d"
            % (expected_size, path, actual_size))
    actual_sha = sha256_of(path)
    if not expected_sha:
        observed = ("the manifest pins no SHA256 yet; %s has sha256 %s (%d bytes)"
                    % (path, actual_sha, actual_size))
        return (EXIT_MISMATCH, "strict: " + observed) if strict \
            else (EXIT_UNPINNED, observed)
    if actual_sha != expected_sha:
        return EXIT_MISMATCH, (
            "sha256 mismatch: the manifest pins %s, %s is %s"
            % (expected_sha, path, actual_sha))
    return EXIT_OK, ("sha256 %s verified against the manifest (%d bytes)"
                     % (actual_sha, actual_size))


def pin(manifest_path: Path, manifest: dict, path: Path) -> int:
    """Record the SHA256 of a freshly fetched disk in the manifest."""
    digest = sha256_of(path)
    manifest["disk"]["sha256"] = digest
    manifest["disk"]["size_bytes"] = path.stat().st_size
    manifest["verified_on"] = datetime.date.today().isoformat()
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=False) + "\n", encoding="utf-8")
    print("pinned sha256 %s (%d bytes) into %s, verified on %s"
          % (digest, manifest["disk"]["size_bytes"], manifest_path,
             manifest["verified_on"]))
    return EXIT_OK


def main(argv: list[str] | None = None) -> int:
    """Dispatch the four sub-commands."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    p_verify = sub.add_parser("verify", help="check a file against the pin")
    p_verify.add_argument("--manifest", required=True)
    p_verify.add_argument("--file", required=True)
    p_verify.add_argument("--strict", action="store_true",
                           help="fail when the manifest pins no SHA256")

    p_digest = sub.add_parser("digest", help="print the SHA256 of a file")
    p_digest.add_argument("--file", required=True)

    p_show = sub.add_parser("show", help="print a field of the manifest")
    p_show.add_argument("--manifest", required=True)
    p_show.add_argument("--field", default="docker-reference",
                         choices=FIELDS)

    p_pin = sub.add_parser("pin", help="record the SHA256 of a disk in the manifest")
    p_pin.add_argument("--manifest", required=True)
    p_pin.add_argument("--file", required=True)

    args = parser.parse_args(argv)
    try:
        if args.command == "digest":
            path = Path(args.file)
            print("%s  %d  %s" % (sha256_of(path), path.stat().st_size, path))
            return EXIT_OK
        manifest = load_manifest(Path(args.manifest))
        if args.command == "show":
            print(field_value(manifest, args.field))
            return EXIT_OK
        if args.command == "pin":
            return pin(Path(args.manifest), manifest, Path(args.file))
        code, message = compare(manifest, Path(args.file), args.strict)
        print("image_manifest: %s" % message, file=sys.stderr if code else sys.stdout)
        return code
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print("image_manifest: %s" % exc, file=sys.stderr)
        return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main())

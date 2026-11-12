# SPDX-License-Identifier: Apache-2.0
"""Tests of the pinned manifest of the QNX 7.1 guest disk (gap 74).

The disk is 6.1 GiB and is never in the repository, so what is tested here is
the *pin*: the manifest that says which artefact the CI job uses, and the tool
that refuses to use anything else. The tests use small synthetic files - no
gigabytes, no docker, no network.

What is covered:
  - the committed manifest is well formed, pins the source by digest and
    records the size of the disk;
  - a file that matches the pin is accepted;
  - a file whose content differs is rejected loudly (this is the whole point:
    the job must never boot a disk it did not expect);
  - a file of the wrong size is rejected before it is even hashed;
  - a manifest without a pinned SHA256 is reported as "not pinned" (exit 3),
    which is a different answer from "mismatch" (exit 1) and from "ok" (0);
  - the reference handed to docker is the digest one, never the tag alone.
"""

from __future__ import annotations

import hashlib
import json
import pathlib
import sys

import pytest

REPO = pathlib.Path(__file__).resolve().parents[2]
TOOL_DIR = REPO / "tools" / "qnx-image"
MANIFEST = TOOL_DIR / "qnx71-image.manifest.json"

sys.path.insert(0, str(TOOL_DIR))
import image_manifest  # noqa: E402  (import after the sys.path fix above)


def write_manifest(tmp_path: pathlib.Path, **disk: object) -> pathlib.Path:
    """A manifest with the same shape as the committed one."""
    data = {
        "schema": 1,
        "verified_on": "2026-11-12",
        "source": {
            "kind": "docker-image",
            "registry": "docker.io",
            "repository": "autosarai/qemu_qnx7_1",
            "tag": "1.1.0",
            "digest": "sha256:" + "ab" * 32,
        },
        "disk": {
            "path_in_source": "/home/popcornsar/fs_3GB.img",
            "size_bytes": 4,
            "sha256": None,
        },
    }
    data["disk"].update(disk)
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(data), encoding="utf-8")
    return path


def test_committed_manifest_pins_the_source_by_digest() -> None:
    """The source must be pinned by digest: a tag can be re-pushed."""
    manifest = image_manifest.load_manifest(MANIFEST)
    reference = image_manifest.docker_reference(manifest)
    assert reference == ("docker.io/autosarai/qemu_qnx7_1@"
                         "sha256:07319f04f85ea25f2629e7b6cf30b4572c47f37e7031ba9150c86226b2de553a")
    assert ":" not in reference.split("@")[1].replace("sha256:", "")


def test_committed_manifest_records_the_disk_size() -> None:
    """The disk is 6 500 959 232 bytes: the size is part of the pin."""
    manifest = image_manifest.load_manifest(MANIFEST)
    assert manifest["disk"]["size_bytes"] == 6500959232
    assert manifest["disk"]["path_in_source"] == "/home/popcornsar/fs_3GB.img"
    assert manifest["source"]["layers"] == 11


def test_a_matching_file_is_accepted(tmp_path: pathlib.Path) -> None:
    """The happy path: the digest and the size match the pin."""
    payload = b"passerelle"
    disk = tmp_path / "disk.img"
    disk.write_bytes(payload)
    digest = hashlib.sha256(payload).hexdigest()
    manifest = write_manifest(tmp_path, sha256=digest, size_bytes=len(payload))
    code, message = image_manifest.compare(image_manifest.load_manifest(manifest), disk)
    assert code == image_manifest.EXIT_OK, message
    assert digest in message


def test_a_different_file_is_rejected_loudly(tmp_path: pathlib.Path) -> None:
    """One bit different must fail the verification, not pass it."""
    disk = tmp_path / "disk.img"
    disk.write_bytes(b"passerellf")
    manifest = write_manifest(tmp_path, sha256=hashlib.sha256(b"passerelle").hexdigest(),
                              size_bytes=len(b"passerelle"))
    code, message = image_manifest.compare(image_manifest.load_manifest(manifest), disk)
    assert code == image_manifest.EXIT_MISMATCH
    assert "mismatch" in message


def test_a_wrong_size_is_rejected_before_hashing(tmp_path: pathlib.Path) -> None:
    """A truncated extraction must not reach the hashing stage."""
    disk = tmp_path / "disk.img"
    disk.write_bytes(b"passerelle")
    manifest = write_manifest(tmp_path, sha256=hashlib.sha256(b"passerelle").hexdigest(),
                              size_bytes=len(b"passerelle") + 1)
    code, message = image_manifest.compare(image_manifest.load_manifest(manifest), disk)
    assert code == image_manifest.EXIT_MISMATCH
    assert "size mismatch" in message


def test_a_missing_file_is_a_mismatch(tmp_path: pathlib.Path) -> None:
    """No file at all is never a verification."""
    manifest = write_manifest(tmp_path, sha256="ab" * 32)
    absent = tmp_path / "absent.img"
    code, message = image_manifest.compare(image_manifest.load_manifest(manifest), absent)
    assert code == image_manifest.EXIT_MISMATCH
    assert "does not exist" in message


def test_an_unpinned_manifest_is_not_a_mismatch(tmp_path: pathlib.Path) -> None:
    """'Not pinned yet' is its own answer, with the observed digest."""
    disk = tmp_path / "disk.img"
    disk.write_bytes(b"passerelle")
    manifest = write_manifest(tmp_path, sha256=None, size_bytes=len(b"passerelle"))
    code, message = image_manifest.compare(image_manifest.load_manifest(manifest), disk)
    assert code == image_manifest.EXIT_UNPINNED
    assert hashlib.sha256(b"passerelle").hexdigest() in message


def test_strict_turns_unpinned_into_a_failure(tmp_path: pathlib.Path) -> None:
    """CI asks for strict: an unpinned disk must not pass silently."""
    disk = tmp_path / "disk.img"
    disk.write_bytes(b"passerelle")
    manifest = write_manifest(tmp_path, sha256=None, size_bytes=len(b"passerelle"))
    assert image_manifest.main(["verify", "--manifest", str(manifest),
                                "--file", str(disk), "--strict"]) == \
        image_manifest.EXIT_MISMATCH
    # Without --strict the same situation is "not pinned yet", not a mismatch.
    assert image_manifest.main(["verify", "--manifest", str(manifest),
                                "--file", str(disk)]) == \
        image_manifest.EXIT_UNPINNED

def test_verify_returns_the_mismatch_code_on_the_command_line(tmp_path: pathlib.Path) -> None:
    """The command line is what the shell script calls: same answers."""
    disk = tmp_path / "disk.img"
    disk.write_bytes(b"passerelle")
    manifest = write_manifest(tmp_path, sha256="00" * 32, size_bytes=len(b"passerelle"))
    assert image_manifest.main(["verify", "--manifest", str(manifest),
                                "--file", str(disk)]) == image_manifest.EXIT_MISMATCH


def test_pin_records_the_digest_and_the_date(tmp_path: pathlib.Path) -> None:
    """Pinning is what the first CI run does by hand, on a real disk."""
    disk = tmp_path / "disk.img"
    disk.write_bytes(b"passerelle")
    manifest_path = write_manifest(tmp_path, sha256=None)
    assert image_manifest.main(["pin", "--manifest", str(manifest_path),
                                "--file", str(disk)]) == image_manifest.EXIT_OK
    pinned = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert pinned["disk"]["sha256"] == hashlib.sha256(b"passerelle").hexdigest()
    assert pinned["disk"]["size_bytes"] == len(b"passerelle")
    assert pinned["verified_on"]


def test_show_prints_the_digest_reference(tmp_path: pathlib.Path) -> None:
    """The shell script asks for the reference to pull, not for the JSON."""
    manifest = write_manifest(tmp_path)
    assert image_manifest.field_value(image_manifest.load_manifest(manifest),
                                      "docker-reference") == \
        "docker.io/autosarai/qemu_qnx7_1@sha256:" + "ab" * 32


def test_an_unknown_field_is_refused() -> None:
    """A typo in a field name must fail, not print something plausible."""
    with pytest.raises(ValueError):
        image_manifest.field_value({"source": {}, "disk": {}}, "nope")

# SPDX-License-Identifier: Apache-2.0
"""Integration test: the real ``virtiofsd``, driven by our vhost-user front end.

Covers VFS-02, VFS-03, VFS-04 and VFS-06 end to end, without QEMU, without a
guest and without QNX: this process *is* the hypervisor side of the vhost-user
socket, it publishes a memfd as guest memory, arms a split virtqueue with
eventfds, and exchanges real FUSE messages with the daemon.

The test is skipped with an explicit reason when ``virtiofsd`` is missing, so a
machine without it stays green instead of lying. When the daemon is there the
whole scenario runs in well under the CI budget (measured: about one second,
including the daemon start-up).
"""

from __future__ import annotations

import pathlib

import pytest

from qshare.runner import find_virtiofsd
from vhost.driver import format_report, run_scenario

VIRTIOFSD = find_virtiofsd()

pytestmark = pytest.mark.skipif(
    VIRTIOFSD is None,
    reason=(
        "virtiofsd is not installed (looked for /usr/lib/qemu/virtiofsd, "
        "/usr/libexec/virtiofsd, /usr/bin/virtiofsd and on PATH): the real "
        "vhost-user backend this test drives is absent, so there is nothing "
        "to prove here - the fake backend of host/vhost/fake_backend.py is "
        "exercised by the unit tests instead"
    ),
)

EXPECTED_CHECKS = {
    "init.version",
    "init.minor",
    "init.max_write",
    "lookup.ino",
    "lookup.size",
    "lookup.mode",
    "getattr.size",
    "getattr.nlink",
    "read.bytes",
    "write.count",
    "write.on_disk",
    "readdir.names",
}


@pytest.fixture(scope="module")
def report(tmp_path_factory) -> dict:
    """Run the whole scenario once, against a temporary shared directory."""
    share = tmp_path_factory.mktemp("virtiofsd-share") / "shared"
    return run_scenario(share, timeout=15.0)


def test_virtiofsd_answers_every_operation_of_the_sequence(report):
    assert report["ok"], format_report(report)
    names = {check["name"] for check in report["checks"]}
    assert EXPECTED_CHECKS <= names, "missing checks: %s" % sorted(EXPECTED_CHECKS - names)


def test_the_answers_match_the_truth_on_the_shared_directory(report):
    """Every check compares a FUSE answer with the host's own view of the file."""
    for check in report["checks"]:
        assert check["ok"], "%s failed: %s" % (check["name"], check["detail"])


def test_the_handshake_negotiates_modern_virtio_and_reply_ack(report):
    handshake = report["handshake"]
    assert "VERSION_1" in handshake["acked_features"]
    assert "PROTOCOL_FEATURES" in handshake["acked_features"]
    assert handshake["acked_protocol_features"] == "REPLY_ACK"
    assert "REPLY_ACK" in handshake["device_protocol_features"]


def test_the_scenario_stays_inside_the_ci_budget(report):
    assert report["duration_s"] < 20.0, format_report(report)


def test_the_command_line_runs_standalone(tmp_path: pathlib.Path):
    """``python3 -m vhost`` is the documented way to reproduce this proof."""
    from vhost.__main__ import main

    share = tmp_path / "cli-share"
    share.mkdir()
    assert main(["--share", str(share), "--timeout", "15"]) == 0

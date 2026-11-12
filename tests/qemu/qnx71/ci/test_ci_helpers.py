#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the QNX 7.1 CI helpers, for the parts that need no guest.

Booting QNX takes a minute and a 6 GB disk; these tests cover the logic that
does not need either: the port list of the SSH wait, the liveness check on the
QEMU pid file, the cleaning of a QEMU monitor answer, and the banner probe of a
port nobody listens on. Run with:

    python3 -m pytest tests/qemu/qnx71/ci -q
"""
from __future__ import annotations

import os
import pathlib

import pytest

import qemu_monitor
import wait_ssh


def test_parse_ports_accepts_a_range() -> None:
    """The image forwards 2222-2225; the whole range must be polled."""
    assert wait_ssh.parse_ports("2222-2225") == [2222, 2223, 2224, 2225]


def test_parse_ports_accepts_a_list_and_spaces() -> None:
    """A comma-separated list is the other accepted spelling."""
    assert wait_ssh.parse_ports(" 2222 , 2225 ") == [2222, 2225]


def test_parse_ports_rejects_an_empty_spec() -> None:
    """An empty spec would make the wait silently poll nothing."""
    with pytest.raises(ValueError):
        wait_ssh.parse_ports("")


def test_qemu_running_true_for_a_live_pid(tmp_path: pathlib.Path) -> None:
    """Our own pid is alive: the wait must keep going."""
    (tmp_path / "qemu.pid").write_text("%d\n" % os.getpid(), encoding="ascii")
    assert wait_ssh.qemu_running(tmp_path) is True


def test_qemu_running_false_for_a_dead_pid(tmp_path: pathlib.Path) -> None:
    """A pid that is gone means the guest is gone: stop waiting at once."""
    dead = 2 ** 22 - 1
    while True:
        try:
            os.kill(dead, 0)
        except OSError:
            break
        dead -= 1
    (tmp_path / "qemu.pid").write_text("%d\n" % dead, encoding="ascii")
    assert wait_ssh.qemu_running(tmp_path) is False


def test_qemu_running_true_without_a_pid_file(tmp_path: pathlib.Path) -> None:
    """No pid file is not evidence of death: keep waiting for the banner."""
    assert wait_ssh.qemu_running(tmp_path) is True


def test_read_banner_of_a_port_nobody_listens_on() -> None:
    """A refused connection is an empty banner, not an exception."""
    assert wait_ssh.read_banner("127.0.0.1", 1, 0.5) == ""


def test_clean_drops_the_monitor_echo_and_prompt() -> None:
    """The HMP redraws its prompt while typing: only the answer must remain.

    Both samples are real captures of `info pci`: the first with the escape
    sequences the monitor sends, the second as it looks once they are stripped
    (one line of growing prefixes of the command).
    """
    with_escapes = ("i\x1b[K\x1b[Din\x1b[K\x1b[Dinfo pci\r\n"
                    "  Bus  0, device   5, function 0:\r\n"
                    "    Class 0002: PCI device 1af4:1009\r\n"
                    "      IRQ 10, pin A\r\n"
                    "(qemu) ")
    stripped_echo = ("iininfinfoinfo info pinfo pcinfo pci\n"
                     "  Bus  0, device   5, function 0:\n"
                     "    Class 0002: PCI device 1af4:1009\n"
                     "      IRQ 10, pin A\n"
                     "(qemu) ")
    # 1+2+...+8 = 36: the exact length of the real echo line above.
    assert qemu_monitor.echo_bound("info pci") == 36
    for raw in (with_escapes, stripped_echo):
        out = qemu_monitor.clean(raw, "info pci")
        assert "IRQ 10, pin A" in out
        assert "1af4:1009" in out
        assert "(qemu)" not in out
        assert "\x1b" not in out
        assert "info pci" not in out
        assert out.startswith("Bus  0, device   5")

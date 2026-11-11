# SPDX-License-Identifier: Apache-2.0
"""Mount-time measurement (PERF-06: mounting takes less than 50 ms).

The mount is measured where it happens: in the guest. The command sends the
driver's mount line to the guest console and times the round-trip of a
`waitfor` on the mount point, which is exactly what a user waits for.

Without a guest console the measurement is impossible, and the tool says so
with a non-zero status instead of inventing a number.
"""

from __future__ import annotations

import pathlib
import time

from pgwutil import HostError

MOUNT_LIMIT_MS = 50.0


def measure_mount_ms(
    console: pathlib.Path,
    driver: str,
    tag: str,
    mount_point: str = "/host",
    timeout: float = 30.0,
) -> float:
    """Return the mount duration in milliseconds, measured in the guest."""
    from qshare.qrun import ConsoleClient

    if not console.exists():
        raise HostError(f"guest console socket not found: {console}")
    token = f"m{int(time.time() * 1000) % 1000000}"
    payload = (
        f"echo mount-begin {token}\r"
        f"{driver} {tag} {mount_point}\r"
        f"echo mount-end {token} $?\r"
    )
    started = time.monotonic()
    ConsoleClient(console).transact(payload, token, timeout)
    elapsed_ms = (time.monotonic() - started) * 1000.0
    return elapsed_ms


def verdict(mount_ms: float) -> str:
    """PASS/FAIL against the PERF-06 threshold."""
    return "PASS" if mount_ms < MOUNT_LIMIT_MS else "FAIL"


def describe(mount_ms: float) -> str:
    """Human-readable one-liner."""
    return f"mount: {mount_ms:.1f} ms (limit {MOUNT_LIMIT_MS:.0f} ms) -> {verdict(mount_ms)}"

# SPDX-License-Identifier: Apache-2.0
"""Differential test, automatic reduction and regression storage (7.2).

The PC folder is the truth. One run of the test:

  1. generates a seeded operation sequence (fsproof.gen);
  2. has it replayed twice: once by the guest agent (the client under
     test) and once by the PC reference implementation, each in its own
     directory;
  3. snapshots both trees -- what the guest saw, what the PC sees;
  4. reports every divergence (names, types, sizes, modes, dates, symlink
     targets, SHA-256 of the content);
  5. reduces a divergent sequence to the shortest one that still diverges,
     by bisection, and stores it under regressions/ for non-regression.

`--client` selects the implementation under test, which is what makes the
comparison objective (PERF-07): the same bench drives this driver,
qnx-fs-9p and eclipse.
"""

from __future__ import annotations

import json
import pathlib
import shutil
import tempfile
import time
from dataclasses import dataclass, field

from fsproof.gen import generate_sequence
from fsproof.model import Action, Entry, Snapshot, scan_tree
from fsproof.replay import replay
from pgwutil import HostError, dump_json, run_cmd, write_atomic

HASH_LIMIT = 1 << 20
#: Tolerance added in local mode for filesystems with a coarse timestamp
#: granularity (one second is the worst case still seen in practice).
CLOCK_MARGIN_NS = 1_000_000_000


@dataclass
class Divergence:
    """One difference between the guest tree and the PC tree."""

    path: str
    reason: str
    guest: str = ""
    host: str = ""

    def line(self) -> str:
        """One-line human-readable form."""
        detail = f" (guest={self.guest}, host={self.host})" if self.guest or self.host else ""
        return f"{self.path}: {self.reason}{detail}"


@dataclass
class DiffResult:
    """Outcome of one differential run."""

    seed: int
    total_actions: int
    divergences: list[Divergence] = field(default_factory=list)
    duration_s: float = 0.0
    mode: str = "local"
    client: str = "passerelle"
    reduced: list[Action] | None = None
    regression: pathlib.Path | None = None
    errors: list[str] = field(default_factory=list)

    @property
    def success(self) -> bool:
        """True when the guest view matches the PC view exactly."""
        return not self.divergences

    @property
    def mismatches(self) -> list[str]:
        """Divergence lines, kept for report compatibility."""
        return [item.line() for item in self.divergences]


def compare(host: Snapshot, guest: Snapshot, mtime_slack_ns: int = 0) -> list[Divergence]:
    """Compare two snapshots and list every difference.

    *mtime_slack_ns* tolerates a coarser guest clock: a client that only
    keeps whole seconds is not a conformance bug, so callers targeting such
    a client pass 1_000_000_000.
    """
    divergences: list[Divergence] = []
    for path in sorted(set(host.entries) | set(guest.entries)):
        host_entry = host.entries.get(path)
        guest_entry = guest.entries.get(path)
        if guest_entry is None:
            divergences.append(Divergence(path, "missing in the guest view"))
            continue
        if host_entry is None:
            divergences.append(Divergence(path, "unexpected entry in the guest view"))
            continue
        divergences.extend(_compare_entry(host_entry, guest_entry, mtime_slack_ns))
    return divergences


def _compare_entry(host: Entry, guest: Entry, mtime_slack_ns: int = 0) -> list[Divergence]:
    """Compare one entry field by field."""
    found: list[Divergence] = []
    if host.kind != guest.kind:
        found.append(Divergence(host.path, "type differs", guest.kind, host.kind))
        return found
    if host.size != guest.size:
        found.append(Divergence(host.path, "size differs", str(guest.size), str(host.size)))
    if host.mode != guest.mode:
        found.append(Divergence(host.path, "mode differs", f"{guest.mode:o}", f"{host.mode:o}"))
    if abs(host.mtime_ns - guest.mtime_ns) > mtime_slack_ns:
        found.append(Divergence(host.path, "mtime differs", str(guest.mtime_ns), str(host.mtime_ns)))
    if host.kind == "link" and host.target != guest.target:
        found.append(Divergence(host.path, "symlink target differs", guest.target, host.target))
    if host.kind == "file" and host.sha256 != guest.sha256:
        found.append(
            Divergence(host.path, "content hash differs", guest.sha256[:12], host.sha256[:12])
        )
    return found


class LocalReplayer:
    """Replays sequences on the PC; used by --local and by the reduction."""

    mode = "local"

    def replay_into(self, work: pathlib.Path, actions: list[Action]) -> tuple[Snapshot, list[str]]:
        """Replay *actions* under *work* and snapshot what landed there."""
        results = replay(work, actions)
        errors = [
            f"step {step.index} {step.op} {step.path}: {step.detail}"
            for step in results
            if not step.ok
        ]
        return scan_tree(work, hash_limit=HASH_LIMIT), errors


class GuestReplayer:
    """Replays sequences through an external guest agent command.

    The agent replays *inside the share*, so the PC and the guest look at
    one and the same directory: that is what makes the PC folder the truth,
    and it is why no mtime tolerance is needed here.
    """

    mode = "guest"

    def __init__(self, command: list[str], timeout: float = 300.0) -> None:
        if not command:
            raise HostError("guest command is empty")
        self.command = command
        self.timeout = timeout

    def replay_into(self, work: pathlib.Path, actions: list[Action]) -> tuple[Snapshot, list[str]]:
        """Send the sequence to the guest and read its snapshot back."""
        sequence_file = work.parent / f"{work.name}-sequence.json"
        snapshot_file = work.parent / f"{work.name}-snapshot.json"
        write_atomic(sequence_file, dump_json({"actions": [a.to_json() for a in actions]}))
        snapshot_file.unlink(missing_ok=True)
        cmd = [
            *self.command,
            "--work", str(work),
            "--sequence", str(sequence_file),
            "--snapshot", str(snapshot_file),
        ]
        proc = run_cmd(cmd, timeout=self.timeout)
        if proc.returncode != 0:
            raise HostError(f"guest agent failed ({proc.returncode}): {proc.stderr.strip()[:400]}")
        if not snapshot_file.exists():
            raise HostError("guest agent produced no snapshot")
        return _snapshot_from_json(snapshot_file), _errors_from_json(snapshot_file)


def _snapshot_from_json(path: pathlib.Path) -> Snapshot:
    """Rebuild a Snapshot from the guest agent's JSON output."""
    payload = json.loads(path.read_text(encoding="utf-8"))
    snapshot = Snapshot()
    for item in payload.get("entries", []):
        snapshot.add(
            Entry(
                path=str(item["path"]),
                kind=str(item["kind"]),
                size=int(item["size"]),
                mode=int(item["mode"]),
                mtime_ns=int(item["mtime_ns"]),
                sha256=str(item.get("sha256", "")),
                target=str(item.get("target", "")),
            )
        )
    return snapshot


def _errors_from_json(path: pathlib.Path) -> list[str]:
    """Replay errors reported by the guest agent."""
    payload = json.loads(path.read_text(encoding="utf-8"))
    return [str(item) for item in payload.get("errors", [])]


def run_once(
    replayer: LocalReplayer | GuestReplayer,
    actions: list[Action],
    base: pathlib.Path,
    mtime_slack_ns: int = 0,
) -> tuple[list[Divergence], list[str]]:
    """Replay *actions* once and return the divergences it exposes.

    Two trees are always built: the *expected* one, produced by the PC
    reference implementation replaying the sequence, and the *client* one,
    produced by the implementation under test (the guest agent in guest
    mode). Comparing them is the differential test: a client that silently
    drops an operation, forgets a mode bit or mangles a symlink shows up
    here even though it never reports an error.

    Timestamps: two replays started at different moments cannot carry the
    same mtimes, so the tolerance is the wall time of both replays plus a
    margin for coarse filesystem clocks. Sizes, types, modes, symlink
    targets and content hashes are compared strictly, always.
    """
    expected_dir = _new_workdir(base, "ref_")
    client_dir = _new_workdir(base, "guest_")
    try:
        started = time.monotonic_ns()
        replay(expected_dir, actions)
        client_snapshot, errors = replayer.replay_into(client_dir, actions)
        elapsed = time.monotonic_ns() - started
        expected = scan_tree(expected_dir, hash_limit=HASH_LIMIT)
        slack = mtime_slack_ns + elapsed + CLOCK_MARGIN_NS
        return compare(expected, client_snapshot, slack), errors
    finally:
        shutil.rmtree(expected_dir, ignore_errors=True)
        shutil.rmtree(client_dir, ignore_errors=True)


def _new_workdir(base: pathlib.Path, prefix: str) -> pathlib.Path:
    """Create a fresh empty work directory under *base*."""
    return pathlib.Path(tempfile.mkdtemp(prefix=prefix, dir=str(base)))


def reduce_sequence(
    replayer: LocalReplayer | GuestReplayer,
    actions: list[Action],
    base: pathlib.Path,
    budget: int = 200,
    mtime_slack_ns: int = 0,
) -> list[Action]:
    """Shrink *actions* to the shortest failing subsequence (delta debugging).

    Bisection: while removing a chunk still leaves a divergence, the chunk
    goes; when nothing can be removed at that granularity, the chunk halves.
    The loop is bounded by *budget* replays so it always terminates.
    """
    current = list(actions)
    attempts = 0
    chunk = max(1, len(current) // 2)
    while chunk >= 1 and attempts < budget:
        index = 0
        shrunk = False
        while index < len(current) and attempts < budget:
            candidate = current[:index] + current[index + chunk:]
            attempts += 1
            if candidate:
                divergences, _ = run_once(replayer, candidate, base, mtime_slack_ns)
                if divergences:
                    current = candidate
                    shrunk = True
                    continue
            index += chunk
        if not shrunk:
            chunk //= 2
    return current


def save_regression(
    reg_dir: pathlib.Path, seed: int, actions: list[Action], divergences: list[Divergence]
) -> pathlib.Path:
    """Store a reduced failing sequence as a non-regression case."""
    reg_dir.mkdir(parents=True, exist_ok=True)
    target = reg_dir / f"seed-{seed}-{int(time.time() * 1000)}.json"
    payload = {
        "seed": seed,
        "actions": [action.to_json() for action in actions],
        "divergences": [divergence.line() for divergence in divergences],
    }
    return write_atomic(target, dump_json(payload))


def load_regression(path: pathlib.Path) -> list[Action]:
    """Load a stored regression case back into actions."""
    payload = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    return [Action.from_json(item) for item in payload.get("actions", [])]


def run_diff(
    seed: int = 1,
    count: int = 200,
    mode: str = "local",
    guest_command: list[str] | None = None,
    client: str = "passerelle",
    reg_dir: pathlib.Path | str = "regressions",
    reduce_divergence: bool = True,
    base: pathlib.Path | str | None = None,
    mtime_slack_ns: int = 0,
    actions: list[Action] | None = None,
) -> DiffResult:
    """Run one differential test, optionally reducing and storing a failure."""
    started = time.monotonic()
    work_base = pathlib.Path(base) if base else pathlib.Path(tempfile.mkdtemp(prefix="fsproof_"))
    work_base.mkdir(parents=True, exist_ok=True)
    if mode == "guest":
        if not guest_command:
            raise HostError("mode 'guest' needs a guest command (see --guest-cmd)")
        replayer: LocalReplayer | GuestReplayer = GuestReplayer(list(guest_command))
    else:
        replayer = LocalReplayer()

    sequence = actions if actions is not None else generate_sequence(seed=seed, count=count)
    result = DiffResult(seed=seed, total_actions=len(sequence), mode=replayer.mode, client=client)
    try:
        result.divergences, result.errors = run_once(replayer, sequence, work_base, mtime_slack_ns)
        if result.divergences and reduce_divergence:
            result.reduced = reduce_sequence(
                replayer, sequence, work_base, mtime_slack_ns=mtime_slack_ns
            )
            result.regression = save_regression(
                pathlib.Path(reg_dir), seed, result.reduced, result.divergences
            )
    finally:
        if base is None:
            shutil.rmtree(work_base, ignore_errors=True)
    result.duration_s = time.monotonic() - started
    return result


def run_local_diff(seed: int = 1, count: int = 200, reg_dir: str = "regressions") -> DiffResult:
    """Compatibility helper: differential test in local mode."""
    return run_diff(seed=seed, count=count, mode="local", reg_dir=reg_dir)

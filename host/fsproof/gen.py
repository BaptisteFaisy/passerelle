# SPDX-License-Identifier: Apache-2.0
"""Replayable random operation sequences, seeded and shrinkable (7.2).

The generator is a pure function of its seed: the same seed always yields
the same sequence, which is what makes a divergence replayable and what
allows the report to publish a one-line reproduction command.
"""

from __future__ import annotations

import random

from fsproof.model import NAME_MAX, OPS, Action

ALPHABET = "abcdefghijklmnopqrstuvwxyz0123456789"
DEFAULT_DIRS = 4
DEFAULT_FILES = 8
MAX_ACTIONS_PER_STEP = 4096


class SequenceGenerator:
    """Deterministic generator of filesystem operations for one tree."""

    def __init__(self, seed: int, ops: int = 200) -> None:
        self.seed = seed
        self.ops = ops
        self.rng = random.Random(seed)
        self.dirs: list[str] = ["."]
        self.files: list[str] = []
        self.links: list[str] = []

    def generate(self) -> list[Action]:
        """Produce the whole sequence for this seed."""
        actions: list[Action] = []
        while len(actions) < self.ops:
            action = self._one()
            if action is not None:
                actions.append(action)
        return actions

    def _name(self, prefix: str) -> str:
        """Random short name with a stable prefix."""
        suffix = "".join(self.rng.choice(ALPHABET) for _ in range(6))
        return f"{prefix}_{suffix}"

    def _child(self, parent: str, name: str) -> str:
        """Join a parent directory ('.' is the root) with a name."""
        return name if parent == "." else f"{parent}/{name}"

    def _pick_dir(self) -> str:
        """Pick an existing directory, creating new ones early on."""
        if len(self.dirs) < DEFAULT_DIRS:
            return self._make_dir()
        return self.rng.choice(self.dirs)

    def _make_dir(self) -> str:
        """Create a directory action target and remember it."""
        parent = self.rng.choice(self.dirs)
        path = self._child(parent, self._name("d"))
        self.dirs.append(path)
        return path

    def _pick_file(self) -> str | None:
        """Pick an existing file, or None when none exists yet."""
        return self.rng.choice(self.files) if self.files else None

    def _payload(self) -> bytes:
        """Random payload whose size is biased towards small files."""
        size = self.rng.choice([0, 1, 7, 64, 512, 4096, 8192])
        return bytes(self.rng.getrandbits(8) for _ in range(size))

    def _one(self) -> Action | None:
        """Generate one weighted operation."""
        weights = [
            ("mkdir", 10), ("create", 18), ("write", 20), ("append", 8),
            ("truncate", 6), ("rename", 8), ("unlink", 8), ("symlink", 5),
            ("link", 3), ("chmod", 5), ("stat", 5), ("listdir", 4),
        ]
        names, weights_only = zip(*weights)
        op = self.rng.choices(names, weights=weights_only, k=1)[0]
        handler = getattr(self, f"_op_{op}", None)
        return handler() if handler is not None else None

    def _op_mkdir(self) -> Action:
        return Action(op="mkdir", path=self._make_dir(), mode=0o755)

    def _op_create(self) -> Action:
        directory = self._pick_dir()
        path = self._child(directory, self._name("f"))
        self.files.append(path)
        return Action(op="create", path=path, mode=0o644)

    def _op_write(self) -> Action | None:
        target = self._pick_file()
        if target is None:
            return self._op_create()
        return Action(op="write", path=target, data=self._payload(), offset=0)

    def _op_append(self) -> Action | None:
        target = self._pick_file()
        if target is None:
            return None
        return Action(op="append", path=target, data=self._payload())

    def _op_truncate(self) -> Action | None:
        target = self._pick_file()
        if target is None:
            return None
        return Action(op="truncate", path=target, offset=self.rng.choice([0, 1, 16, 4096]))

    def _op_rename(self) -> Action | None:
        target = self._pick_file()
        if target is None:
            return None
        directory = self.rng.choice(self.dirs)
        new_path = self._child(directory, self._name("r"))
        self.files.remove(target)
        self.files.append(new_path)
        return Action(op="rename", path=target, arg=new_path)

    def _op_unlink(self) -> Action | None:
        target = self._pick_file()
        if target is None:
            return None
        self.files.remove(target)
        return Action(op="unlink", path=target)

    def _op_symlink(self) -> Action | None:
        target = self._pick_file()
        if target is None:
            return None
        directory = self._pick_dir()
        link = self._child(directory, self._name("l"))
        self.links.append(link)
        return Action(op="symlink", path=link, arg=target)

    def _op_link(self) -> Action | None:
        target = self._pick_file()
        if target is None:
            return None
        directory = self._pick_dir()
        return Action(op="link", path=target, arg=self._child(directory, self._name("h")))

    def _op_chmod(self) -> Action | None:
        target = self._pick_file() or self._pick_dir()
        mode = self.rng.choice([0o600, 0o644, 0o664, 0o755, 0o400])
        return Action(op="chmod", path=target, mode=mode)

    def _op_stat(self) -> Action | None:
        target = self._pick_file() or self._pick_dir()
        return Action(op="stat", path=target)

    def _op_listdir(self) -> Action:
        return Action(op="listdir", path=self.rng.choice(self.dirs))


def generate_sequence(seed: int, count: int = 200) -> list[Action]:
    """Convenience wrapper returning the sequence for *seed*."""
    return SequenceGenerator(seed=seed, ops=count).generate()


def long_name_actions(length: int = NAME_MAX) -> list[Action]:
    """Operations using a name of exactly *length* bytes (FS-15)."""
    name = "n" * length
    return [
        Action(op="create", path=name),
        Action(op="write", path=name, data=b"max-name"),
        Action(op="stat", path=name),
        Action(op="unlink", path=name),
    ]


def all_op_names() -> tuple[str, ...]:
    """Every operation name the replayer understands."""
    return OPS

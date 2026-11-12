#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the deterministic source tarball of a release, and its SHA256.

The tarball is the artefact a judge or a downstream user downloads: the whole
repository at the state of the tag, including `LICENSE`, the attribution
`NOTICE`, `README.md` and `docs/`. It is built by `make release-check`'s
companion `make tarball` and by the `tarball` job of
`.github/workflows/release.yml`.

"Deterministic" means: two builds of the same tree give byte-identical archives,
so the published SHA256 is reproducible by anyone. Three things are therefore
fixed:

  * the member order (sorted, a directory always before its children);
  * the timestamps (one epoch for every member: `SOURCE_DATE_EPOCH`, else the
    commit time of HEAD, else 2026-11-11T00:00:00Z);
  * the ownership and the modes (uid/gid 0, `root:root`, 0644 or 0755 when the
    file is executable), and gzip stores no name and mtime 0.

The member list comes from `git ls-files --cached --others --exclude-standard`
when git is available (tracked files plus the untracked ones .gitignore keeps),
so no build tree, cache or local artefact can slip in; a plain sorted walk with
the same exclusions is the fallback.

Usage:
    python3 tools/release/make_tarball.py [--output-dir dist] [--version 0.1.0]
    python3 tools/release/make_tarball.py --list
    python3 tools/release/make_tarball.py --print-sha256

Exit status: 0 on success, 1 when a required entry is missing.
Standard library only.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import os
import pathlib
import stat
import subprocess
import sys
import tarfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from releaseinfo import DEFAULT_EPOCH, ReleaseError, read_version, repo_root  # noqa: E402

#: Entries the release contract requires (a tarball without them is a bug).
REQUIRED_ENTRIES = ("LICENSE", "NOTICE", "README.md", "VERSION", "CHANGELOG.md", "docs")
EXCLUDE_DIRS = {".git", "__pycache__", ".pytest_cache", ".venv", "venv", "node_modules", "dist"}
EXCLUDE_PREFIXES = ("build",)
FILE_MODE = 0o644
EXEC_MODE = 0o755
DIR_MODE = 0o755
OWNER = (0, 0, "root", "root")


def source_epoch(root: pathlib.Path) -> int:
    """Return the single mtime used by every member of the archive."""
    env = os.environ.get("SOURCE_DATE_EPOCH")
    if env and env.isdigit():
        return int(env)
    try:
        out = subprocess.run(
            ["git", "log", "-1", "--format=%ct"],
            cwd=root,
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return DEFAULT_EPOCH
    stamp = out.stdout.strip()
    return int(stamp) if out.returncode == 0 and stamp.isdigit() else DEFAULT_EPOCH


def tracked_files(root: pathlib.Path) -> list[str] | None:
    """Return the releasable relative paths, or None when git is unavailable.

    Tracked files plus the untracked ones that .gitignore does not exclude, so
    a tarball built from a working tree matches the one built from a tag
    checkout (where nothing is untracked) and build trees never sneak in.
    """
    try:
        out = subprocess.run(
            ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
            cwd=root,
            capture_output=True,
            text=True,
            timeout=120,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0:
        return None
    return [path for path in out.stdout.split("\0") if path]


def is_excluded(rel: str) -> bool:
    """True for build trees, caches and other directories never released."""
    parts = rel.split("/")[:-1]
    if any(part in EXCLUDE_DIRS or part.startswith(EXCLUDE_PREFIXES) for part in parts):
        return True
    return rel.endswith(".pyc") or ".egg-info/" in rel


def walk_files(root: pathlib.Path) -> list[str]:
    """Fallback member list: every regular file or symlink, exclusions applied."""
    found: list[str] = []
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root).as_posix()
        if is_excluded(rel) or path.is_dir():
            continue
        found.append(rel)
    return found


def collect_files(root: pathlib.Path) -> list[str]:
    """Return the sorted member list of the archive."""
    files = tracked_files(root) or walk_files(root)
    return sorted(rel for rel in files if not is_excluded(rel))


def missing_required(files: list[str]) -> list[str]:
    """Return the required entries absent from the member list.

    An entry is satisfied either as a file of that exact name or, for a
    directory entry such as `docs`, by at least one file under it.
    """
    names = set(files)
    missing: list[str] = []
    for entry in REQUIRED_ENTRIES:
        if entry in names:
            continue
        prefix = entry.rstrip("/") + "/"
        if not any(name.startswith(prefix) for name in names):
            missing.append(entry)
    return missing


def member_info(name: str, path: pathlib.Path, epoch: int, prefix: str) -> tarfile.TarInfo:
    """Build the TarInfo of one file, with fixed metadata."""
    info = tarfile.TarInfo(name=f"{prefix}/{name}")
    info.mtime = epoch
    info.uid, info.gid, info.uname, info.gname = OWNER
    if path.is_symlink():
        info.type = tarfile.SYMTYPE
        info.linkname = os.readlink(path)
        info.mode = EXEC_MODE
        info.size = 0
    else:
        info.type = tarfile.REGTYPE
        info.size = path.stat().st_size
        info.mode = EXEC_MODE if path.stat().st_mode & stat.S_IXUSR else FILE_MODE
    return info


def add_members(tar: tarfile.TarFile, root: pathlib.Path, files: list[str], prefix: str, epoch: int) -> int:
    """Add every member in sorted order, creating each directory once."""
    seen_dirs: set[str] = set()
    total = 0
    for rel in files:
        for parent in _parents(rel):
            if parent in seen_dirs:
                continue
            seen_dirs.add(parent)
            tar.addfile(_dir_info(f"{prefix}/{parent}", epoch))
        path = root / rel
        info = member_info(rel, path, epoch, prefix)
        total += info.size
        if info.type == tarfile.REGTYPE:
            with path.open("rb") as handle:
                tar.addfile(info, handle)
        else:
            tar.addfile(info)
    return total


def _parents(rel: str) -> list[str]:
    """Return the ancestor directories of a member, outermost first."""
    parts = rel.split("/")[:-1]
    return ["/".join(parts[: i + 1]) + "/" for i in range(len(parts))]


def _dir_info(name: str, epoch: int) -> tarfile.TarInfo:
    """Build the TarInfo of one directory member."""
    info = tarfile.TarInfo(name=name)
    info.type = tarfile.DIRTYPE
    info.mode = DIR_MODE
    info.mtime = epoch
    info.uid, info.gid, info.uname, info.gname = OWNER
    return info


def build_tarball(
    root: pathlib.Path, version: str, output_dir: pathlib.Path, epoch: int, prefix: str
) -> tuple[pathlib.Path, str, int, int]:
    """Write the archive and return (path, sha256, file count, total bytes)."""
    files = collect_files(root)
    missing = missing_required(files)
    if missing:
        raise ReleaseError(f"refusing to build a tarball without: {', '.join(missing)}")
    output_dir.mkdir(parents=True, exist_ok=True)
    path = output_dir / f"passerelle-{version}.tar.gz"
    raw = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9) as gz:
        with tarfile.open(fileobj=gz, mode="w", format=tarfile.GNU_FORMAT) as tar:
            total = add_members(tar, root, files, prefix, epoch)
    path.write_bytes(raw.getvalue())
    return path, hashlib.sha256(raw.getvalue()).hexdigest(), len(files), total


def write_checksum(path: pathlib.Path, digest: str) -> pathlib.Path:
    """Write the `sha256sum -c` compatible sidecar of the archive."""
    sidecar = pathlib.Path(str(path) + ".sha256")
    sidecar.write_text(f"{digest}  {path.name}\n", encoding="utf-8")
    return sidecar


def main(argv: list[str] | None = None) -> int:
    """Build the release tarball and print its SHA256."""
    parser = argparse.ArgumentParser(description="Build the deterministic source tarball.")
    parser.add_argument("--root", default=None, help="repository root (default: auto)")
    parser.add_argument("--output-dir", default="dist", help="where to write (default: dist)")
    parser.add_argument("--version", default=None, help="version (default: the VERSION file)")
    parser.add_argument("--epoch", type=int, default=None, help="fixed member mtime")
    parser.add_argument("--list", action="store_true", help="print the member list and exit")
    parser.add_argument("--print-sha256", action="store_true", help="print only the checksum")
    args = parser.parse_args(argv)

    root = pathlib.Path(args.root).resolve() if args.root else repo_root()
    try:
        version = args.version or read_version(root)
        if args.list:
            for rel in collect_files(root):
                print(rel)
            return 0
        epoch = args.epoch if args.epoch is not None else source_epoch(root)
        prefix = f"passerelle-{version}"
        output = pathlib.Path(args.output_dir)
        if not output.is_absolute():
            output = root / output
        path, digest, count, total = build_tarball(root, version, output, epoch, prefix)
        sidecar = write_checksum(path, digest)
        if args.print_sha256:
            print(digest)
            return 0
        print(f"tarball: {path}")
        print(f"members: {count} files, {total} bytes uncompressed, prefix {prefix}/")
        print(f"epoch:   {epoch}")
        print(f"sha256:  {digest}")
        print(f"checksum file: {sidecar}")
        return 0
    except ReleaseError as err:
        print(f"make_tarball: {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

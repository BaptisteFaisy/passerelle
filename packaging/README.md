# Debian packaging of the Passerelle host tools (gap 96)

This directory holds the `debian/` skeleton that builds the
`passerelle-host` binary package: the four Python console tools (`qshare`,
`qrun`, `fsproof`, `pbench`) declared in `host/pyproject.toml`. It is a
**native** source package (`3.0 (native)`), version taken from the top-level
`VERSION` file — the changelog version is kept in sync by
`host/tests/test_packaging.py` (CI-enforced).

## Layout

| File | Role |
| --- | --- |
| `debian/control` | source stanza + `passerelle-host` binary stanza (`Depends: python3`, `Recommends: cmake, qemu-system-x86`) |
| `debian/rules` | `dh` + pybuild, pointed at `host/` (where `pyproject.toml` lives) |
| `debian/changelog` | native version, **must equal `VERSION`** |
| `debian/copyright` | Apache-2.0, Format 1.0 |
| `debian/source/format` | `3.0 (native)` |

There is no `debian/install`: pybuild does the installing (the task allows
"debian/install **or** pybuild glue"; pybuild avoids duplicating the console
script list that `host/pyproject.toml` already owns).

## Why `packaging/debian/` and not `debian/`

`dpkg-buildpackage` only finds a `debian/` directory at the root of the
source tree. Keeping it under `packaging/` keeps the project root clean and
makes the packaging reviewable as one unit; the build recipe below copies it
into place in a throwaway tree.

## Building the .deb

Prerequisites (Ubuntu 24.04 / Debian trixie):

```bash
sudo apt-get install dpkg-dev debhelper dh-python python3-all python3-setuptools pybuild-plugin-pyproject
```

Build from a pristine export (never commit the produced artefacts —
compiled/built binaries do not belong in the repository):

```bash
tmp="$(mktemp -d)"
git archive HEAD | tar -x -C "$tmp"
cp -r packaging/debian "$tmp/debian"
(cd "$tmp" && dpkg-buildpackage -us -uc)
# artefacts land next to $tmp: passerelle-host_0.1.0_all.deb etc.
rm -rf "$tmp"
```

Or in-place (leaves an untracked `debian/` at the root; remove it after):

```bash
cp -r packaging/debian debian && dpkg-buildpackage -us -uc; rm -rf debian
```

## Installation for the two audiences

- **pipx / pip** (upstream way, already checked by the `install-check` job
  of `.github/workflows/release.yml`):

  ```bash
  pipx install ./host        # or: pip install ./host
  qshare --version           # must print the VERSION file's content
  ```

- **apt** (this skeleton):

  ```bash
  sudo apt install ./passerelle-host_0.1.0_all.deb
  ```

## What was verified, what was not

Verified by `host/tests/test_packaging.py` on every PR (job `host-tools`):

- `debian/control` carries the required fields (Source, Maintainer,
  Standards-Version, Build-Depends, Rules-Requires-Root) and the binary
  stanza declares `python3` in Depends, `cmake` / `qemu-system-x86` in
  Recommends;
- `debian/changelog` parses and its version equals `VERSION`;
- every source path listed in `debian/install`, should the file exist,
  resolves in the repository;
- `debian/rules` uses dh + pybuild and is executable;
- `debian/copyright` is Format 1.0 and declares Apache-2.0;
- every console script declared in `host/pyproject.toml` imports and exposes
  a callable `main` (the pipx contract).

**Not** verified here: an actual `dpkg-buildpackage` run. It needs debhelper
and dh-python, which the self-hosted runner does not provide, and it is far
too slow for the 5-minute CI budget. The result of the local attempt is
recorded in the pull request that introduces this directory.

<!--
SPDX-License-Identifier: Apache-2.0
-->

# Releasing Passerelle

This is the whole publication procedure: prepare, verify, tag, publish, check
what was published, and recover when a publication fails. What a release
*contains* is not described here - that is the job of
[CHANGELOG.md](../CHANGELOG.md).

The pieces this procedure uses:

| File | Role |
| --- | --- |
| `VERSION` | the single source of truth for the version string |
| `CHANGELOG.md` | the release notes; the GitHub release body is one of its sections |
| `host/pyproject.toml` | the Python package `passerelle-host` (`qshare`, `qrun`, `fsproof`, `pbench`) |
| `tools/release/verify_release.py` | coherence check: VERSION / CHANGELOG / pyproject / tag / checksum |
| `tools/release/make_tarball.py` | the deterministic source tarball and its `.sha256` |
| `tools/release/changelog_section.py` | extracts one version's section for the release notes |
| `tools/release/test_version_sync.py` | pytest guard: the three version strings cannot drift |
| `.github/workflows/release.yml` | what runs when a `v*` tag is pushed |
| `Makefile` | `make release-check`, `make tarball`, `make check` |

---

## 1. Versioning policy

Passerelle follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
`MAJOR.MINOR.PATCH`:

- **MAJOR** - a change that breaks a user of the driver ABI or of the host
  tools' command line (renamed option, changed wire-format constant, removed
  API in a `pgw/*.h` header).
- **MINOR** - a new requirement of the specification becomes implemented, or a
  new tool/option appears, with no breakage.
- **PATCH** - fixes and documentation only.

Two project-specific rules:

1. **While `0.x`, nothing is claimed stable.** The `0.` prefix says exactly what
   [CHANGELOG.md](../CHANGELOG.md) says in *Known limitations*: the portable
   stack is tested on the host PC, and no driver has run under QNX because no
   SDP is available. The first release that boots a Passerelle driver on a QNX
   guest is the one that deserves `1.0.0`.
2. **A version is immutable.** Once a tag is pushed, that version number is
   bound to that content forever, even if the publication half-failed. A
   correction is a new PATCH, never a re-cut of the same number. PyPI enforces
   this by refusing a second upload of the same version; the git tag and the
   GitHub release enforce it by convention.

The tag is always `v` + the content of `VERSION` (`VERSION` = `0.1.0` →
tag `v0.1.0`). `verify_release.py` fails on anything else.

### Why `host/pyproject.toml` repeats the version

`VERSION` is the single source of truth, but a published sdist or wheel only
contains the package directory: `dynamic = ["version"]` pointing at `../VERSION`
would build on a checkout and fail for everybody installing from PyPI. The
literal in `host/pyproject.toml` is therefore a deliberate copy, and
`tools/release/test_version_sync.py` fails the build (and the `verify` job of
the release workflow) when the copy and `VERSION` disagree. Update both in the
same commit - the guard is what makes that safe.

---

## 2. Prepare a release

Work on a branch, through a pull request, like any other change
([CONTRIBUTING.md](../CONTRIBUTING.md)).

```bash
# 1. Write the release notes first: they decide what the version is.
$EDITOR CHANGELOG.md      # add "## [0.2.0] - YYYY-MM-DD" with Added/Changed/Fixed
                          # and an honest "Known limitations"

# 2. Move the version, in both places, in the same commit.
echo "0.2.0" > VERSION
$EDITOR host/pyproject.toml      # version = "0.2.0"

# 3. The tree must be green.
make check                 # build + ctest + pytest + size-guard + spdx-guard

# 4. The release itself must be coherent and reproducible.
make release-check         # verify_release.py + pytest tools/release + tarball + SHA256
```

`make release-check` is the gate: it refuses a tree whose `VERSION`,
`CHANGELOG.md` and `host/pyproject.toml` disagree, whose CHANGELOG has no dated
section for the version, whose required tarball entries are missing, or whose
tarball does not match its checksum. Add `TAG=v0.2.0` once the tag exists to
check the tag as well:

```bash
make release-check TAG=v0.2.0
```

Merge the pull request. Mergify merges it when the CI is green; `main` is
protected, so nothing reaches it by a direct push.

---

## 3. Tag and publish

```bash
git switch main && git pull --ff-only
git tag -a v0.2.0 -m "Passerelle 0.2.0"
git push origin v0.2.0
```

Pushing the tag starts `.github/workflows/release.yml`, whose jobs all carry an
explicit `timeout-minutes: 5` and share one `concurrency` group per tag:

| Job | What it does |
| --- | --- |
| `verify` | `verify_release.py --tag v0.2.0 --require-tag`, then `pytest tools/release` |
| `tarball` | builds the deterministic tarball with `SOURCE_DATE_EPOCH` = the commit time of the tag, uploads it and its `.sha256` as the artefact `passerelle-source-tarball`, and prints the checksum in the job summary |
| `install-check` | `python3 -m venv` + `pip install ./host`, then requires `qshare --version` to print exactly the content of `VERSION`, the three other console scripts to run, and the test suites to pass from the installed package |
| `pypi-token` | reports whether `PYPI_API_TOKEN` is configured (it never prints it) |
| `release` | writes the notes with `changelog_section.py`, adds an artefact table with the SHA256, and creates the GitHub Release with both files attached |
| `publish-pypi` | runs **only** when `pypi-token` found the secret: `python -m build` + `twine check` + `twine upload` of `host/` |
| `pypi-skipped` | runs only in the opposite case, and says out loud that nothing was published to PyPI and why |

By default a release is therefore **tarball + GitHub release only**. PyPI is
opt-in: add the repository secret `PYPI_API_TOKEN` (a PyPI API token scoped to
the `passerelle-host` project) and re-run. Nothing is ever uploaded to a
registry that nobody configured.

The `release` job is idempotent: if the release already exists it uploads the
artefacts to it (`--clobber`) instead of failing.

---

## 4. What the tarball contains

`passerelle-<version>.tar.gz` is the repository at the state of the tag, under a
single `passerelle-<version>/` directory: every tracked file plus the untracked
files that `.gitignore` keeps, which means **no** build tree, no cache, no
compiled object. `make_tarball.py` refuses to produce an archive that lacks the
release contract:

```
LICENSE   NOTICE   README.md   VERSION   CHANGELOG.md   docs/
```

`NOTICE` is the attribution file: it names the third-party projects this work
builds upon or compares against, and their licences. It ships in every tarball
because Apache-2.0 requires the attributions to travel with the distribution.

The archive is **deterministic**, so anybody can rebuild the published bytes:

- the member list is sorted, and a directory always precedes its children;
- every member carries one timestamp: `$SOURCE_DATE_EPOCH`, else the commit time
  of `HEAD`, else `2026-11-11T00:00:00Z`;
- ownership is `0:0 (root:root)` and modes are normalised to `0644`, or `0755`
  for a file whose owner-executable bit is set;
- gzip stores no file name and `mtime = 0`.

```bash
SOURCE_DATE_EPOCH=$(git log -1 --format=%ct v0.2.0) make tarball
```

---

## 5. Verifying a download

Every release publishes the checksum twice: as the artefact
`passerelle-<version>.tar.gz.sha256`, and inside the body of the GitHub Release.
Download both files next to each other, then:

```bash
sha256sum -c passerelle-0.2.0.tar.gz.sha256
# passerelle-0.2.0.tar.gz: OK
```

Compare the printed digest with the one in the release body: the sidecar proves
the file was not corrupted in transit, the release body proves it was not
substituted.

To reproduce the checksum from a clean clone:

```bash
git clone https://github.com/Tradoooo/passerelle.git
cd passerelle && git checkout v0.2.0
SOURCE_DATE_EPOCH=$(git log -1 --format=%ct) make tarball
sha256sum dist/passerelle-0.2.0.tar.gz
```

Then check that the release really installs and runs:

```bash
tar xzf passerelle-0.2.0.tar.gz && cd passerelle-0.2.0
python3 -m venv /tmp/pgw && /tmp/pgw/bin/pip install ./host
/tmp/pgw/bin/qshare --version        # qshare 0.2.0
make check                           # build + ctest + pytest + both guards
```

---

## 6. Recovering a failed release

Diagnose first: open the workflow run of the tag and read which job failed. The
jobs are ordered so that a failure early leaves nothing published.

| Situation | What to do |
| --- | --- |
| `verify` failed | Nothing was published. Fix the incoherence it names (usually `VERSION` vs `host/pyproject.toml`, or a CHANGELOG section without a date), merge, delete the tag and re-tag the new commit - see below. |
| `tarball` or `install-check` failed | Nothing was published either (the `release` job needs both). Re-run the failed jobs from the GitHub UI; the checkout is the same commit, so the artefacts stay reproducible. |
| `release` failed after `gh release create` | The release may exist without its artefacts. Re-run the workflow **from the tag** (`Run workflow` → enter `v0.2.0`): the job detects the existing release and uploads the artefacts to it. |
| `publish-pypi` failed | The GitHub release is already out, and that is the artefact that matters. If PyPI rejected the upload because the version exists, the version is burned: bump PATCH and release again. Otherwise fix the token or the network and re-run only that job. |
| The tag itself is wrong (typo, wrong commit) | Delete it everywhere, then re-tag: <br>`git push origin :refs/tags/v0.2.0 && git tag -d v0.2.0` <br>`gh release delete v0.2.0 --yes` (only if it was created) <br>then `git tag -a v0.2.0 -m ... && git push origin v0.2.0`. Do this **only** while nothing was uploaded to PyPI; once a version is on PyPI it cannot be re-used, so bump the version instead. |

Re-running by hand, without pushing the tag again:

```
GitHub → Actions → "release (tag v*)" → Run workflow → tag: v0.2.0
```

The workflow reads the tag from its input in that case, and `verify` checks it
against `VERSION` exactly as it does for a pushed tag.

A release that cannot be repaired is withdrawn, not silently left broken:
`gh release delete v0.2.0 --yes`, delete the tag, and record in
`CHANGELOG.md` under the next version why the number was skipped.

---

## 7. Checklist

```
[ ] CHANGELOG.md has "## [X.Y.Z] - <date>" with Added / Changed / Fixed /
    Known limitations, and the entries cite the requirement identifiers
[ ] VERSION and host/pyproject.toml both say X.Y.Z (one commit)
[ ] make check is green
[ ] make release-check is green (and make release-check TAG=vX.Y.Z once tagged)
[ ] the pull request is merged; main is up to date locally
[ ] git tag -a vX.Y.Z && git push origin vX.Y.Z
[ ] the workflow is green: verify, tarball, install-check, release
[ ] the GitHub release shows the notes, the SHA256 table and both artefacts
[ ] sha256sum -c on a fresh download says OK
[ ] PyPI: published only if PYPI_API_TOKEN is configured - otherwise the
    pypi-skipped notice explains it
```

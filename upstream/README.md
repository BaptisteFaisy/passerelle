# Upstream contributions

This directory holds the changes Passerelle proposes **back** to the projects it
builds on, rather than forking them.

The specification ([`docs/spec-en.md`](../docs/spec-en.md), section 3.1) puts two
options on the table: rewrite everything, or contribute to what exists. The
project takes option 2 as its first move — a working x86_64 9P share reached by
adding a transport to an existing client is worth more than a third client that
duplicates the first two — and keeps option 3 (virtiofs, which nobody has done
for a QNX guest) as the new work. This directory is where option 2 lives.

## What is proposed, to whom, and where it stands

| Project | Licence | What we propose | State |
|---|---|---|---|
| [`gvergine/qnx-fs-9p`](https://github.com/gvergine/qnx-fs-9p) | Apache-2.0 | A virtio-pci transport plus the x86_64 target, behind its own `src/transport.h`; four patches and a cover letter in [`qnx-fs-9p/`](qnx-fs-9p/) | **Ready to send.** The series applies to its tree (`git apply --check`, then `git am`), the host build and its unit tests pass. Not yet validated on an x86_64 QNX image — see [`qnx-fs-9p/validation.md`](qnx-fs-9p/validation.md). Nothing has been pushed: the coordinator decides when and how to open the pull request. |
| [`eclipse-score/qnx_unit_tests`](https://github.com/eclipse-score/qnx_unit_tests), directory `src/common/virtio9p` | Apache-2.0 | Nothing to merge. Its PCI client is the public reference for MSI-X on QNX without a root PCI server configuration, and reading it produced a list of improvement suggestions. In [`eclipse-score/findings.md`](eclipse-score/findings.md) | **Findings only.** Written as suggestions to its maintainers, not as a patch: the changes we would make touch its error handling and its interrupt path, which is more than a drive-by patch should. |
| [`kodebooth/qnx-virtio`](https://github.com/kodebooth/qnx-virtio) | MIT | Nothing: it is evaluated for **reuse**, not for contribution. The evaluation behind decision D3 is in [`kodebooth-qnx-virtio/evaluation.md`](kodebooth-qnx-virtio/evaluation.md) | **Evaluated, not reused.** Its transport is real and well built, but every layer of Passerelle must also build and run its tests on the PC, which needs a bus-access abstraction it does not have. |

## Ground rules

- **No source file of a reference project is copied into Passerelle.** The
  patches are written against their interfaces, in our own words; the numeric
  facts that cannot be copyrighted (message numbers, field orders, errno
  values, register offsets) were cross-checked against their code and their
  documentation. `NOTICE` records this.
- **Nothing is pushed to a third-party repository** from here. There is no
  remote for them in this repository, and no pull request or issue has been
  created by this work.
- **Their public API does not change.** `qnx-fs-9p`'s `transport.h` only gains a
  second factory function; its resource manager, its 9P client and its `lib9p`
  codec are untouched, so the patches cannot regress the ARM64 behaviour that
  has been tested in a guest.
- **Every claim is labelled.** Each document says what was read, what was
  compiled, what was run, and what is still an assumption. The `qnx-fs-9p`
  patches in particular have been compiled and unit-tested on a PC, but not
  booted on an x86_64 QNX image, and the documents say so in those words.

## Layout

```
upstream/
├── README.md                          this file
├── qnx-fs-9p/
│   ├── cover-letter.md                the message to the author
│   ├── patches/000{1..4}-*.patch      git format-patch series, applies to 868a718
│   ├── validation.md                  what is proven on a PC, what is not
│   └── syntax-check/                  PC-side syntax check for the QNX-only files
├── eclipse-score/
│   └── findings.md                    improvement suggestions, and its MSI-X method
└── kodebooth-qnx-virtio/
    └── evaluation.md                  the reuse decision (D3), argued
```

## Reproducing the checks

The patch series is verified against a fresh clone of the upstream tree, at its
only commit:

```sh
git clone <qnx-fs-9p> /tmp/fs9p && cd /tmp/fs9p
for p in /path/to/passerelle/upstream/qnx-fs-9p/patches/*.patch; do
    git apply --check "$p" && git am --keep-cr "$p"
done
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

The QNX-only files are checked on a PC without the SDP, against
declarations-only stand-ins for the QNX headers:

```sh
/path/to/passerelle/upstream/qnx-fs-9p/syntax-check/syntax-check.sh /tmp/fs9p
```

Both checks are described in
[`qnx-fs-9p/validation.md`](qnx-fs-9p/validation.md).

<!--
SPDX-License-Identifier: Apache-2.0
-->

# Submission checklist — QNX Global Hackathon 2026

**Deadline: Tuesday 15 December 2026, 20:00 Paris time (UTC+1).** The plan is to
submit on the evening of **14 December**, one day of margin (spec §10).

Status is checked against the repository, not against intentions: a box is
ticked only when a file or a recorded measurement backs it. Anything still open
is listed in [`docs/gap-analysis.md`](../gap-analysis.md).

Legend: `[x]` done and verifiable · `[ ]` to do · `[!]` done but conditional,
read the note.

## 1. Repository and licence

- [x] Public repository: https://github.com/BaptisteFaisy/passerelle
      (`gh repo view` reports `visibility: PUBLIC`, `isPrivate: false`).
- [x] Open-source licence: Apache-2.0, full text in [`LICENSE`](../../LICENSE).
- [x] [`NOTICE`](../../NOTICE) attributes the projects we read, evaluated or
      patched (`gvergine/qnx-fs-9p`, `eclipse-score/qnx_unit_tests`,
      `kodebooth/qnx-virtio`).
- [x] No third-party code vendored; the patches we offer upstream live in
      [`upstream/`](../../upstream/) under their original licence.
- [x] No secret, no personal data, no compiled binary in the tree
      (`.gitignore`, and the CI job `rules`).
- [ ] Re-check the public state of the default branch the day of submission:
      no private fork, no draft-only content, issues and discussions readable.

## 2. Required content, in English

- [x] English [`README.md`](../../README.md): overview, prior art and what we
      add, architecture, build, honest verification status.
- [!] A 60-second summary block exists
      ([`readme-summary.md`](readme-summary.md)) — **to be pasted at the top of
      the README by whoever owns that file**.
- [x] Build instructions naming the QNX version, target and toolchain:
      [`docs/building.md`](../building.md) (QNX SDP 8.0, `x86_64le` and
      `aarch64le`, `qcc`; the host build needs no SDP).
- [x] Specification in English: [`docs/spec-en.md`](../spec-en.md) (the French
      original stays authoritative for wording).
- [x] Architecture: [`docs/architecture.md`](../architecture.md); decisions:
      [`docs/decisions.md`](../decisions.md) and [`docs/adr/`](../adr/).
- [x] User guide with CMake and Rust workflows:
      [`docs/user-guide.md`](../user-guide.md) (DX-04).
- [x] Tutorial for the Best Tutorial bonus:
      [`codelab/`](../../codelab/README.md), eight chapters, code compiled and
      exercised as `test_clab`.

## 3. Prior art and disclosures (spec §3.3 — mandatory)

- [x] Both existing 9P clients are named in the README, in the submission text
      and in [`docs/state-of-the-art.md`](../state-of-the-art.md), with what
      each does and does not do.
- [x] What Passerelle adds is stated as a list, not as a claim of first-ever
      invention: complete share on x86_64, virtiofs for a QNX guest, the
      differential and benchmark testbed, priority-ordered scheduling.
- [x] The absence of a public QNX virtiofs guest driver is labelled a **search
      result, not a proof** (spec §14, README).
- [x] `kodebooth/qnx-virtio` was evaluated line by line and the outcome is
      public: [ADR-003](../adr/ADR-003-write-our-own-virtio-transport.md),
      [`upstream/kodebooth-qnx-virtio/evaluation.md`](../../upstream/kodebooth-qnx-virtio/evaluation.md).
- [x] Limits are disclosed in the submission text: no QNX-side requirement is
      claimed as verified; the QNX guest we booted is 7.1, not 8.0; the
      performance numbers for our driver do not exist yet.
- [x] Failures are written down, not buried:
      [`what-did-not-work.md`](what-did-not-work.md).
- [x] AI-assisted development is described where the repository describes its
      process ([`docs/ci.md`](../ci.md)); no claim of authorship is made about
      third-party code.

## 4. Video and visuals

- [ ] **Video under 3 minutes**, in English — script, shot list and timeline are
      written ([`docs/video-script.md`](../video-script.md), target 2:52);
      recording is blocked on having something to film on the QNX side.
- [ ] Video link pasted into the Devpost form
      ([`devpost.md`](devpost.md), "Links" table) and into the README.
- [ ] **Screenshot** for the gallery: the E5 console with both protocols
      passing, or the QNX 7.1 `pci-tool -vv` output next to the host terminal.
      No image asset exists in the repository yet.
- [ ] Narration names the prior art out loud (spec §3.3) and states the SDP
      blocker in the same breath as the demonstration.

## 5. Evidence that must stay reproducible

- [x] Requirement matrix generated, not hand-written:
      `python3 tools/traceability.py --check` — **77 requirements: 50 tested,
      21 partial, 6 not started** ([`docs/traceability.md`](../traceability.md)).
- [x] Differential campaign: 3 000 000 operations, 0 divergence
      (`PGW_DIFF_OPS=1000000 build/tests/test_diff`).
- [x] Randomized-input campaign: 5 000 000 iterations per decoder family,
      0 defect, also under ASan+UBSan.
- [x] E5 (host side, real guest, both protocols):
      `python3 tests/qemu/run_share_check.py --protos 9p,virtiofs`, and the same
      run in CI outside pull requests (`.github/workflows/qemu-e2e.yml`).
- [x] QNX 7.1 device observation: [`tests/qemu/qnx71/EVIDENCE.md`](../../tests/qemu/qnx71/EVIDENCE.md).
- [x] Round-trip baseline: [`docs/measurements/linux-baseline-9p.md`](../measurements/linux-baseline-9p.md)
      (1 643 events, 822 requests).
- [x] Guards green: `tools/check-size.py` (< 500 lines/file, < 50 lines/function)
      and `tools/check-spdx.py` (Apache-2.0 header on every source file).
- [ ] Re-run all of the above on the submission commit and paste the outputs
      into the issue that tracks the submission, so the numbers in the text and
      the numbers in the tree are the same ones.

## 6. Category and outreach

- [!] **Category**: *Developer Tools* by default. The question to the organisers
      is drafted ([`docs/outreach/d1-category-question.md`](../outreach/d1-category-question.md),
      issue #7) but **nothing is posted without the owner's agreement**.
- [ ] Answer from the Discord recorded in the issue, and the category updated in
      the Devpost form if it differs.
- [ ] Upstream offer to `gvergine/qnx-fs-9p` sent, or the reason it was not sent
      recorded in [`upstream/qnx-fs-9p/cover-letter.md`](../../upstream/qnx-fs-9p/cover-letter.md).

## 7. Blocking item

- [ ] **QNX SDP 8.0 installed** (QNX Everywhere, myQNX account). This single item
      decides whether the submission shows a running QNX driver or an honestly
      documented host-side stack plus a real QNX 7.1 observation. Everything
      behind it: E1 (driver half), E2, E3, E4, PERF-02 … PERF-07, and 21 partial
      requirements.

## 8. On the day (14 December)

- [ ] Freeze the tree, tag the submission commit.
- [ ] `python3 tools/traceability.py --check`, both guards, `ctest`, pytest: all
      green on the tagged commit.
- [ ] README, video link, screenshot, Devpost form filled from
      [`devpost.md`](devpost.md).
- [ ] Submit before 20:00 Paris time on 15 December — target the evening of the
      14th.

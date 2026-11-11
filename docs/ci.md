# CI — self-hosted pipeline and the 5-minute budget

This document describes the whole CI of the Passerelle repository: what runs
on a pull request, what runs outside of it, why the budget is five minutes,
how to read a failure, how to replay the long job by hand, and who merges.

The policy reference is `~/Bureau/CI-BUDGET-5MIN.md` (user rule of
2026-10-24): **a CI run is broken when it is slow.** Every run must render
its verdict in five minutes or less, from the first job started to the last
one finished. The rule is enforced by a guard (`.github/ci-budget/`), not by
good intentions.

---

## 1. Where the jobs run

The pipeline runs on **four self-hosted runners** hosted on the R630, all
carrying the labels `self-hosted`, `Linux`, `X64`, `passerelle-worker` and
`ci-node`:

```
runs-on: [self-hosted, passerelle-worker]
```

Each runner owns its own workspace (`_work1` … `_work4`), so two jobs never
share a working directory, and one runner never runs two jobs at the same
time. The four jobs of the pull-request pipeline therefore really run in
parallel.

Toolchain available on the runners:

| Tool | Version | Notes |
|---|---|---|
| `cmake` / `ctest` | 3.31 | installed in `~/ci-tools/cmake` (see §9) |
| `gcc` | 14.2 | also provides `cc`, used by the `qnx-syntax` job |
| `python3` | 3.13 | `pytest` 9.2 is available through the user site-packages |

---

## 2. What runs on a pull request

Workflow: `.github/workflows/ci.yml` — triggered by `pull_request` (and by a
push on `main`, documentation-only pushes excluded). One `concurrency` group
per ref with `cancel-in-progress: true`: a new push cancels the previous run.

Five independent jobs, all with `timeout-minutes: 5`:

| Job | What it does | Build directory |
|---|---|---|
| `build-test` | `cmake -B build` + `cmake --build` + `ctest` (RelWithDebInfo) | `build/` |
| `sanitize` | same stack with `-DPGW_SANITIZERS=ON` (ASan + UBSan) + `ctest` | `build-asan/` |
| `host-tools` | `python3 -m pytest host/tests -q` + `python3 -m compileall` | — |
| `qnx-syntax` | compiles every `*_qnx.c` against `tests/qnx-stub/include` | — |
| `rules` | `tools/check-size.py` + `tools/check-spdx.py` + the CI budget guard | — |

The verdict of the pull request is the **slowest** of those five jobs, which
is why the budget applies to the workflow and not to each step.

The socle workflows of the repository run alongside:

| Workflow | Role | Blocking? |
|---|---|---|
| `ci-budget` | checks that no job declares more than 5 minutes | yes (`queue_conditions`) |
| `hermes-lite` | freshness gate before the merge | no — **dry-run** (`HERMES_LITE_DRY_RUN=1`) |
| `stale-base-guard` | reports how far the branch is behind `main` | no — informational |

### Explicit skips, never silent greens

`host-tools` and `qnx-syntax` depend on parts of the tree that are still
being written (`host/tests`, `tests/qnx-stub/include`, the `*_qnx.c`
sources). When an input is missing, the job prints an explicit
`::notice::` line and exits successfully:

```
::notice::tests/qnx-stub/include is missing - QNX syntax check skipped (explicit skip, not a silent green)
```

A skip is always visible in the log: a green job always says what it did
*not* check. Nothing is ever green by accident.

### The build directories are kept

On a persistent runner, a blind `git clean -ffdx` throws away every compiled
object and forces a cold rebuild on the next run (measured on another
repository: a Gradle build dropped from 77 s to 33 s once the outputs were
kept). Each job therefore:

1. cleans the workspace **before** the checkout, excluding `/build`,
   `/build-asan` and `/.ci-tools`;
2. checks out with `clean: false` and `fetch-depth: 1` (no full clone);
3. drops a build tree whose `CMakeCache.txt` points at another source
   directory — such a cache cannot be reused and would abort the configure
   step.

---

## 3. What runs outside a pull request

Workflow: `.github/workflows/qemu-e2e.yml`.

| Trigger | Allowed |
|---|---|
| `workflow_dispatch` (manual) | yes |
| `push` on `main` | yes |
| `pull_request` | **never** |

It hosts the jobs that are genuinely long: the differential conformance bench
(millions of operations), the QNX guest boot under QEMU, and the throughput
measurements. Its ceiling is explicit and justified in the YAML:

```yaml
    # 30 min : banc de conformite differential (1M operations) + boot QNX
    # sous QEMU + mesures de debit, hors PR (regle CI 5 min).
    timeout-minutes: 30
```

This is the only exception to the five-minute rule, and it is allowed
because the job never runs on a pull request. The file is therefore listed
in `SKIP_FILES` of `.github/ci-budget/check_timeouts.py` — the guard checks
every other workflow, but cannot judge a job that is allowed to be long.

---

## 4. Why five minutes

- **A slow CI is a broken CI.** Beyond a few minutes, people stop waiting
  for the verdict and start merging blind; the check becomes decoration.
- **The build dominates, not the tests.** On a previous project, 97 s of a
  118 s CI was compilation and only 4 s was the test suite. The budget is
  spent where it must be spent: on the build, never on waiting.
- **It forces the right design.** Parallel jobs, one build directory per
  job, kept caches, shallow clones, filtered triggers. Nothing is left to
  chance or to "it was fast when I tried it".
- **A verdict that arrives late is not a verdict.** The five-minute budget
  is measured from the first job started to the last one finished, so a
  workflow of ten sequential jobs is as much a violation as one slow job.

---

## 5. How to read a failure

1. Open the failed run, click on the red job, read the **first** red line —
   the rest is usually noise.
2. Every job ends with a duration notice:
   `::notice::build-test: 87s (budget 300s)`. If the job failed after
   300 s, the job hit its `timeout-minutes` and the log is truncated: look
   at the last line reached, not at the last error.
3. `::notice::` lines are information, not errors: they announce explicit
   skips and the toolchain that was picked.

Typical failures and what they mean:

| Job | Typical cause | Where to look |
|---|---|---|
| `build-test` | compiler error (`-Werror`, `-Wconversion`) | the CMake output, then the source file |
| `sanitize` | ASan/UBSan report, leak, or a slow test | the `ctest --output-on-failure` block |
| `host-tools` | a failing `pytest` test, or a Python syntax error | the `pytest` output |
| `qnx-syntax` | a `*_qnx.c` file that does not compile against the stubs | the `cc -fsyntax-only` output |
| `rules` | file ≥ 500 lines, function ≥ 50 lines, missing SPDX header, or a job over 5 minutes | the `SIZE VIOLATION` / `SPDX VIOLATION` lines |

Reproduce a failure locally with the exact commands of the job:

```bash
cd /home/baptiste/Bureau/passerelle
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j4
ctest --test-dir build --output-on-failure --no-tests=error

cmake -B build-asan -DPGW_SANITIZERS=ON
cmake --build build-asan -j4
ctest --test-dir build-asan --output-on-failure

python3 -m pytest host/tests -q
python3 tools/check-size.py --root .
python3 tools/check-spdx.py --root .
python3 .github/ci-budget/check_timeouts.py
```

---

## 6. Replaying the long job by hand

The long bench never runs on a pull request, so it is replayed explicitly:

```bash
# From the repository (on GitHub, with the gh CLI)
gh workflow run qemu-e2e.yml --ref main -f ops=1000000

# Locally, with the same commands
cmake -B build && cmake --build build -j4
PGW_BENCH_OPS=1000000 timeout 1500 python3 host/bench/run-bench.py
timeout 1500 python3 host/qshare/run-e2e.py
```

The number of operations is passed to the bench scripts through the
`PGW_BENCH_OPS` environment variable, so a bench script does not have to
know any command-line convention. Each step of the workflow is individually
bounded by `timeout`, so a hung bench is killed and reported instead of
eating the 30-minute ceiling.

---

## 7. Mergify and hermes-lite

**Mergify is the only merge pilot.** `.mergify.yml` describes the queue:
a pull request enters the queue when the required checks are successful, it
is not a draft, it has no conflict, and its base is `main`.

```
queue_conditions:
  - check-success = ci-budget
  - check-success = build-test
  - check-success = sanitize
  - check-success = host-tools
  - check-success = qnx-syntax
  - check-success = rules
  - -conflict
  - -draft
  - base = main
```

Two priority rules (`urgent` → high, `priority-low` → low) order the queue,
and `merge_queue.max_parallel_checks: 4` matches the four workers of the
pool. Mergify rebases (`merge_method: rebase`) and refreshes the branch
itself before merging.

**hermes-lite is a gate, not a merge.** It watches the freshness of the
branch, rebases it through the API when it lags behind `main`, and labels
the pull request `hermes/conflit` when a real conflict appears. It is
configured in dry-run (`HERMES_LITE_DRY_RUN=1`): it observes and alerts, but
it never merges. There is exactly one merge pilot — Mergify.

`stale-base-guard` is purely informative: it reports how many commits the
branch is behind `main` and always succeeds.

### Branch protection must NOT require up-to-date branches

Measured on 11 November 2026, on PR #13: with `required_status_checks.strict:
true` ("Require branches to be up to date before merging") on `main`, the Mergify
queue **refuses to queue anything** and posts a failing `Mergify Merge Queue`
check with the reason *"Configuration not compatible with a branch protection
setting"*. The queue tests a pull request through a draft PR built on the latest
base, then merges the original branch, which GitHub then considers out of date.

The protection of this repository is therefore:

```bash
gh api -X PUT repos/BaptisteFaisy/passerelle/branches/main/protection --input - <<'JSON'
{
  "required_status_checks": {
    "strict": false,
    "contexts": ["ci-budget", "build-test", "sanitize", "host-tools",
                 "qnx-syntax", "rules", "stale-base-guard"]
  },
  "enforce_admins": false,
  "required_pull_request_reviews": null,
  "restrictions": null
}
JSON
```

Losing `strict` costs nothing here: the queue re-tests every pull request on the
latest base together with the ones queued ahead of it, and `hermes-lite`
separately watches freshness. Two traps found the same day, worth remembering:
the update endpoint is `PUT` (a `PATCH` answers 404), and all four keys must be
supplied or GitHub answers 422.

If `strict` ever has to come back, the queue must switch to
`merge_method: fast-forward`, or to serial in-place checks
(`merge_queue.mode: serial`, `max_parallel_checks: 1`, `batch_size: 1`) — see the
Mergify documentation on GitHub rulesets.

### What the queue needs to actually run

Two things, both easy to miss because a valid configuration is not enough:

1. `pull_request_rules` with a `queue` action. `queue_rules` alone only *defines*
   the queue; nothing ever puts a pull request in it. The action takes no option
   here: `method` and `allow_merging_configuration_change` are rejected by the
   current schema (`mergify config validate` says so), and the merge method comes
   from the queue rule.
2. The Merge Queue product activated for the repository. `mergify config
   simulate` answering "1 rule matches" proves nothing about activation: it
   evaluates the rule, it does not run the engine. The signs that the engine is
   off are an empty `mergify queue status`, no event at all in
   `mergify events`, and no reaction to a `@Mergifyio queue` comment.

---

## 8. Adding a job

Any new job of the pull-request pipeline must respect the whole contract:

```yaml
  my-job:
    name: my-job
    runs-on: [self-hosted, passerelle-worker]
    timeout-minutes: 5
    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 1
          persist-credentials: false
          clean: false
```

- its own build directory, cleaned but never blindly (`git clean -ffdx`
  with the build trees excluded);
- no `sleep` longer than 30 seconds — poll a condition instead;
- if the job is genuinely long, it does not belong to a pull request: move
  it to `qemu-e2e.yml` with an explicit, justified `timeout-minutes`;
- if the job is required for the merge, add it to `queue_conditions` of
  `.mergify.yml` — otherwise the pull request waits forever for a check
  that never comes.

---

## 9. Maintenance notes

- **The CI budget guard** (`.github/ci-budget/check_timeouts.py`) fails when
  a job has no `timeout-minutes` or declares more than five minutes, and
  warns when `concurrency` or `fetch-depth: 1` are missing. Run it locally:
  `python3 .github/ci-budget/check_timeouts.py`.
- **The long-job exception** is declared in `SKIP_FILES` of that guard
  (`qemu-e2e.yml`). If the guard is refreshed from the central socle, the
  entry must be re-added.
- **The runner toolchain** is installed by hand:
  `python3 -m pip install --target ~/ci-tools/cmake "cmake<4"` on the R630
  (the system Python is externally managed, so no root install is needed).
  The workflows look for `cmake` in `PATH`, then in `~/ci-tools/cmake`, and
  finally fall back to a local `pip install --target` inside the workspace
  (cached in `.ci-tools/`, excluded from the workspace cleanup).

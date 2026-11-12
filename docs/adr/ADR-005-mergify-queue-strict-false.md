<!--
SPDX-License-Identifier: Apache-2.0
-->

# ADR-005 — `strict: false` on branch protection, because the Mergify queue refuses `strict: true`

- **Status**: Accepted (11 November 2026, measured on pull request #13)
- **Scope**: repository settings, not code — [`docs/ci.md`](../ci.md) §"Branch
  protection must NOT require up-to-date branches",
  [`.mergify.yml`](../../.mergify.yml)
- **Requirements**: none directly; it is what lets the CI of every requirement
  merge at all (the 5-minute verdict budget, `docs/ci.md` §2)

## Context

Mergify is the only merge pilot of this repository: a pull request is queued,
re-tested on the latest base together with the ones queued ahead of it, then
merged. GitHub branch protection on `main` lists the required checks
(`ci-budget`, `build-test`, `sanitize`, `host-tools`, `qnx-syntax`, `rules`,
`stale-base-guard`).

With `required_status_checks.strict: true` — the "Require branches to be up to
date before merging" box — the queue **refused to queue anything** and posted a
failing `Mergify Merge Queue` check reading *"Configuration not compatible with
a branch protection setting"*. The reason is mechanical: the queue tests a pull
request through a draft PR built on the latest base, then merges the *original*
branch, which GitHub by then considers out of date.

## Decision

Set `strict: false` and keep every required check:

```bash
gh api -X PUT repos/Tradoooo/passerelle/branches/main/protection --input - <<'JSON'
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

Two traps found the same day, both recorded so nobody pays them twice: the
endpoint is `PUT` (a `PATCH` answers 404), and **all four keys** must be
supplied or GitHub answers 422.

A valid Mergify configuration is also not enough on its own: `queue_rules` only
*defines* the queue, so a `pull_request_rules` entry with a `queue` action is
required, and the Merge Queue product must be activated for the repository.

## Consequences

- The queue works: PRs stack, are re-tested in batches (`batch_size` 1–3,
  `max_parallel_checks: 4`) and merge by rebase.
- Losing `strict` costs nothing here, because the queue already re-tests every
  pull request **on the latest base** together with the ones ahead of it — that
  is a stronger guarantee than a stale "up to date" flag — and `hermes-lite`
  separately watches freshness (in dry-run: it observes and alerts, never merges).
- A red `Verdict` check is not automatically a veto: `hermes-lite` runs with
  `cancel-in-progress: true`, so a superseded run turns red with a cancellation
  annotation. `Verdict` is therefore *not* in the required list; read
  `hermes-lite/verdict` for the real verdict.
- If `strict` ever has to come back, the queue must switch to
  `merge_method: fast-forward`, or to serial in-place checks
  (`merge_queue.mode: serial`, `max_parallel_checks: 1`, `batch_size: 1`).
- The risk accepted: a merge is only as fresh as the last batch re-test, so the
  required-context list must never be shortened to make a queue go green.

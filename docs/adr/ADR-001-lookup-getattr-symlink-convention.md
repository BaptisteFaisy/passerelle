<!--
SPDX-License-Identifier: Apache-2.0
-->

# ADR-001 — `lookup` never follows a symbolic link, `getattr` does

- **Status**: Accepted (11 November 2026)
- **Requirements**: FS-08 (symlinks, loop detection), FS-02 (`stat` fields), QNX-01
  (a resource manager answers `lstat` and `stat` differently)
- **Code**: [`rmfs/include/pgw/rmfs.h`](../../rmfs/include/pgw/rmfs.h) (the convention,
  in the header comment), [`rmfs/src/path.c`](../../rmfs/src/path.c)
- **Proof**: `test_rmfs_path` — `test_symlink_yields_redirect`,
  `test_symlink_follow_when_asked`, `test_symlink_loop_detected`,
  `test_readlink_returns_body`, `test_open_nofollow_refuses_symlink`

## Context

On QNX, path resolution is resumed by the *client*: when a path component is a
symbolic link, the resource manager answers with a redirect (`_RESMGR_REDIRECT`)
and the caller walks the target itself. For that to work, the layer-3 walk must
*see* the link.

The backend interface of layer 3 exposes two attribute calls, `lookup` and
`getattr`. The first draft of the interface did not say which one follows a
final symbolic link, and the two sides of the tree answered differently: `rmfs`
assumed one convention, the POSIX truth backend and the 9P adapter assumed the
other. The observable result was that `lstat` on a link loop answered `ELOOP`
while `stat` on the same path answered `OK` — inverted — and FS-08 (loop
detection) was unreachable, because the walk never met a link to redirect on.

The defect was found by *running* the differential bench
([`tests/e2e`](../../tests/e2e)), not by reading the code.

## Decision

The convention is written once, in the header both sides include, and it is
this:

| Call | Semantics | May fail with |
|---|---|---|
| `lookup(impl, relpath, ...)` | attributes **without** following a final symbolic link — `lstat(2)` | — |
| `getattr(impl, relpath, ...)` | attributes **following** a final symbolic link — `stat(2)` | `PGW_ELOOP`, `PGW_ENOENT` |

The walk uses `lookup`, so a link component is seen and turned into a redirect
answer for the caller. `rmfs` resolves links itself, through `readlink`, before
asking for followed attributes.

An adapter whose protocol cannot distinguish the two — 9P2000.L: a `Twalk` ends
on the link itself — may implement both callbacks with the same call, because
`rmfs` does the resolution. What must never happen is `lookup` following links.

## Consequences

- `lstat`, `stat`, `readlink`, loop detection and `open(O_NOFOLLOW)` are all
  consistent, and FS-08 is reachable and tested.
- Every new backend (a future network transport, the reverse-sharing server of
  SRV-01) inherits the rule from the header instead of guessing it.
- The cost is one extra `readlink` round trip when the caller asks for followed
  attributes on a link; that is the price of resolving links in one place.
- The convention is now part of the interface contract: a backend that inverts
  it again fails `test_rmfs_path` immediately, and the differential bench fails
  on the first symlink it generates.

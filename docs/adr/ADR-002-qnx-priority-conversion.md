<!--
SPDX-License-Identifier: Apache-2.0
-->

# ADR-002 — one conversion point from QNX priority to scheduler urgency

- **Status**: Accepted (11 November 2026)
- **Requirements**: QNX-03 (request runs at the caller priority), QNX-04
  (priority-ordered submission), PERF-05 (a high-priority read must not wait
  behind a bulk copy)
- **Code**: [`common/include/pgw/prio.h`](../../common/include/pgw/prio.h)
  (`pgw_prio_from_qnx`, `pgw_prio_to_qnx`),
  [`drivers/common/src/prio_qnx.c`](../../drivers/common/src/prio_qnx.c),
  [`rmfs/src/sched.c`](../../rmfs/src/sched.c)
- **Proof**: `prio` (`common/tests/test_prio.c`) — `test_most_urgent_qnx_priority_wins`,
  `test_conversion_is_an_involution`, `test_out_of_range_is_never_most_urgent`;
  `test_rmfs_sched` — `test_high_priority_overtakes_bulk`, `test_equal_priority_is_fifo`

## Context

Two priority conventions meet in this project and they point in opposite
directions:

- **QNX** numbers thread priorities so that **1 is the most urgent** and 255 the
  least — a real-time kernel, smaller is better;
- the **layer-3 submission scheduler** orders pending requests by **descending
  urgency value**, because that keeps the insertion test trivial and the queue a
  plain array.

The first implementation passed the QNX priority straight through to the
scheduler. The consequence was measured by the directed case written for PERF-05:
a background copy at QNX priority 250 **overtook** a control-loop read at
priority 10. On the target, PERF-05 was inverted — exactly the behaviour the
requirement forbids, and the one a real-time user would notice first.

## Decision

Convert **once**, at the boundary, and make the rest of the project speak a
single convention:

- `pgw_prio_from_qnx(qnx)` maps 1…255 onto 255…1 (`256 - prio`);
- everywhere else in the tree, `prio` means *bigger is more urgent*;
- an out-of-range or unknown value maps to the **least** urgent class, never to
  the most urgent one: a mis-read priority must not let a request jump the queue;
- `pgw_prio_to_qnx()` is the inverse, used only for logging and for the kernel
  trace events of QNX-09, so a trace reads in QNX terms.

The conversion is a `static inline` in a header, so there is no second place to
get it wrong, and the driver re-applies the caller priority for the whole
exchange (`drivers/common/src/prio_qnx.c`).

## Consequences

- PERF-05 behaves as specified on the PC bench: the priority request overtakes
  the eight low-priority ones already queued, and requests of equal priority stay
  FIFO.
- The failure mode is now safe by construction: garbage in gives "least urgent"
  out, which degrades latency for one request instead of inverting the schedule.
- Conversion is an involution, pinned by a test, so the round trip
  QNX → scheduler → trace cannot drift.
- What is still missing is the *measurement* half of QNX-03: showing the
  inherited priority in a QNX kernel trace needs a guest built with the SDP. The
  conversion itself is proven; the trace is not taken.

<!--
SPDX-License-Identifier: Apache-2.0
-->

# ADR-004 — performance is measured in server round trips, starting with a published Linux baseline

- **Status**: Accepted (11 November 2026)
- **Requirements**: PERF-01 (round-trip counts measured and published), and the
  comparative form of PERF-02 … PERF-07
- **Evidence**: [`docs/measurements/linux-baseline-9p.md`](../measurements/linux-baseline-9p.md),
  [`docs/measurements/README.md`](../measurements/README.md)
- **Code**: [`host/bench/trace.py`](../../host/bench/trace.py) (the QEMU `v9fs_*`
  parser), [`host/bench/roundtrips.py`](../../host/bench/roundtrips.py) (the
  table), [`host/bench/report.py`](../../host/bench/report.py)
- **Proof**: `test_cli_roundtrips`, `test_compare_tables_flags_extra_roundtrips`,
  `test_cli_roundtrips_rejects_empty_trace` (host pytest suite)

## Context

Under QEMU, wall-clock timings depend mostly on the host hardware, the runner
load and the cache mode: two honest runs on two machines do not compare, and a
judging panel cannot reproduce either. The specification (§6.5) therefore asks
for performance to be formulated *comparatively* and in **server round trips**.

Two options were on the table for the reference:

1. publish throughput numbers (MiB/s, IOPS) as the headline;
2. publish the number of protocol requests a POSIX call costs, read from a QEMU
   trace, and keep wall-clock as a secondary column.

## Decision

**Option 2, and publish the reference first.** The unit of account is the round
trip — one request sent, one reply received — counted from a real QEMU trace,
and the first published table is the **Linux guest baseline** of spec §7.5, not
our own driver:

```bash
python3 tests/qemu/run_share_check.py --protos 9p --workload 30 \
        --trace baseline.log --workdir /tmp/passerelle-baseline
```

Recorded on 11 November 2026: QEMU 8.2.2, `virtio-9p-pci` with
`security_model=mapped-xattr`, a minimal Alpine guest, **30 create/stat/read/unlink
cycles**. The trace carried **1 643 events → 822 requests, 822 replies, 63
errors**, and every POSIX operation costs **1.00 round trip per call**.

Two rules make the table trustworthy:

- the parser **never guesses**: an unparsable line is reported as a parse error
  rather than dropped silently, and an empty trace exits non-zero;
- the trace carries **no timestamps**, so latencies are simply *not* reported
  rather than invented — round trips per call do not depend on them.

## Consequences

- PERF-01 is satisfied by a measurement anyone can reproduce with one command,
  on any host, and the number does not move when the hardware does.
- Every later claim (PERF-02 … PERF-07) has a fixed reference to be compared
  against, including for the two existing clients, so the comparative report is
  apples to apples.
- The honest cost: **Passerelle's own column is empty.** Filling it needs a QNX
  guest, which needs the SDP; until then `docs/measurements/README.md` lists the
  three missing tables as absent rather than estimated.
- Counting round trips rewards protocol efficiency (chunking, `msize`, caching
  policy) and does not hide a slow host; it also cannot show a latency
  regression, which is why the throughput and priority workloads of
  `host/bench/loads.py` stay in the harness for the day a guest exists.

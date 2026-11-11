# Measurements

Numbers that were actually produced by running something, with the command that
produced them. Nothing here is an estimate, and nothing here is a promise: a row
that has no measurement says so.

| File | What it measures | Client under test |
|---|---|---|
| [linux-baseline-9p.md](linux-baseline-9p.md) | 9P round trips per POSIX operation, from a real QEMU trace | **Linux** guest (the reference baseline of spec 7.5) |
| *(absent)* | Same table for the Passerelle QNX driver | needs the QNX SDP — not measured |
| *(absent)* | Throughput of a 1 GiB sequential read/write | needs a QNX guest — not measured |
| *(absent)* | Priority read during a bulk low-priority copy (PERF-05) | needs a QNX guest — not measured |

The last three rows are the ones the hackathon submission needs, and they are the
reason the QNX SDP is the critical path (see
[../verification-status.md](../verification-status.md)).

## Reproducing the Linux baseline

One command, from the repository root:

```bash
python3 tests/qemu/run_share_check.py --protos 9p --workload 30 \
        --trace baseline.log --workdir /tmp/passerelle-baseline
```

It boots a minimal Linux guest on the options `qshare` emits, runs 30
create/stat/read/unlink cycles on the share, and hands the QEMU trace to
`pbench roundtrips`, which prints the table. Requirements: `qemu-system-x86_64`,
`virtiofsd` (only for the virtiofs protocol), `curl`, `cpio`, `gzip`, `unzstd`,
a readable host kernel with its modules. About 15 s per protocol.

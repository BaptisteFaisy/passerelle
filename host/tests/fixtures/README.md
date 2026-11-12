# The 9P trace fixtures

Two files, two different stories. Both are read by `host/bench/trace.py`, the
parser of the QEMU trace format (`-trace 'v9fs_*'`), and both are checked by
`host/tests/test_trace.py`.

## `qemu-9p-trace.log` — generated, and gated

This one is **produced by the code**, not written by hand:

```sh
tools/regen-fixtures.py --write     # rewrite it
tools/regen-fixtures.py --check     # fail when it drifted
tools/regen-fixtures.py --verify    # two captures, byte for byte
```

The generator runs the production client of `proto9p/` against the
independent reference server of `tests/ref9p/` (the harness of that
directory is reused as it is), captures every frame of the exchange and
renders it in the QEMU trace format. The run is deterministic: the tree has
fixed names and fixed bytes, the clock is the injectable monotonic clock of
`common/` (`pgw/time.h`), and only the protocol facts that do not depend on
the machine are rendered — no inode, no uid/gid, no wall-clock time. Two
runs therefore give the same bytes, on any machine.

The gate is in two places, both on every pull request:

* `host/tests/test_fixture_drift.py` — a pytest that fails when the
  committed file no longer matches what the code produces;
* `tests/tracecap/` — the same check as a CTest case
  (`tracecap-fixture`), plus `tracecap-deterministic`, which captures twice
  and compares the two renderings byte for byte.

The exchange the fixture describes: version, attach, walk, getattr, open,
three reads (the client chunks a read that the server answers short,
P9-08), a write, fsync, clunk, a walk to a directory, readdir, an open that
fails — the server answers `Rlerror`, which the trace shows as
`v9fs_rerror` — and finally a walk to a file that is not there.

## `qemu-9p-real-trace.log` — captured by hand, and *not* regenerated

This one is a **real QEMU 8.2 trace**, captured from a Linux guest on the
host that produced it. It is deliberately left out of the regeneration path,
and the reason is exact: it carries the wall-clock timestamps, the pids and
the inode numbers of that machine, and reproducing it would need the same
QEMU, the same guest image and the same moment. Faking those would make the
fixture *look* reproducible while proving nothing.

It is kept as the ground truth of the trace **format** — the one the parser
is written against — and `host/tests/test_trace.py` asserts the properties a
real trace has (no timestamps, an `xattr` error, a full roundtrip table).

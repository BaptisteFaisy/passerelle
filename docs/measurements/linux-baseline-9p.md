# Roundtrips per POSIX operation — Linux guest baseline (PERF-01)

Produced on **11 November 2026, 11:35 CET** by:

```bash
python3 tests/qemu/run_share_check.py --protos 9p --workload 30 \
        --trace e5-9p-trace.log --workdir /tmp/pgw-e5-trace \
        --cache ~/.cache/passerelle
```

The workload runs in a minimal Linux guest booted by QEMU 8.2.2 with
`-fsdev local,...,security_model=mapped-xattr` and `-device virtio-9p-pci` (the
exact options `qshare` emits). It executes:

* mount,
* a read of a file written by the host before the boot,
* a write of a file read back by the host after the boot,
* **30 cycles** of: create+write, `ls -l` (stat), `cat` (read), `rm` (unlink),
* one `df -h` on the mount.

QEMU recorded 1 643 events (`v9fs_*`), handed to `pbench roundtrips`. This is
the **reference baseline** of spec 7.5 against which Passerelle's own numbers
must be compared once measured on a QNX guest.

| POSIX operation | 9P requests | replies | errors | roundtrips/call | mean ms | max ms |
|---|---:|---:|---:|---:|---:|---:|
| close | 108 | 108 | 0 | 1.00 | n/a | n/a |
| creat / open(O_CREAT) | 31 | 31 | 0 | 1.00 | n/a | n/a |
| getxattr | 31 | 31 | 31 | 1.00 | n/a | n/a |
| lookup / path resolution | 247 | 247 | 32 | 1.00 | n/a | n/a |
| mount (Tattach) | 2 | 2 | 0 | 1.00 | n/a | n/a |
| mount (Tversion) | 1 | 1 | 0 | 1.00 | n/a | n/a |
| open | 31 | 31 | 0 | 1.00 | n/a | n/a |
| read | 123 | 123 | 0 | 1.00 | n/a | n/a |
| stat / fstat | 217 | 217 | 0 | 1.00 | n/a | n/a |
| write | 31 | 31 | 0 | 1.00 | n/a | n/a |
| **total** | **822** | **822** | **63** | | | |

The trace carries no timestamps, so latencies are not reported; roundtrips per
call do not depend on them.

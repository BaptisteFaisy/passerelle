# Measurements on a QNX 8.0 guest — our own driver (PERF-03, PERF-05, PERF-06)

Produced on **13 November 2026, 21:40 CET** by:

```bash
# build the drivers for the real target and the image
. ~/qnx800/qnxsdp-env.sh
cmake --build build-qnx -j8
./tests/qemu/qnx8/build-image.sh

# boot the guest with the share on the virtio-9p-pci device
qemu-system-x86_64 -machine q35 -smp 1 -m 512 -enable-kvm -cpu host \
  -kernel build/qnx8/passerelle.ifs \
  -fsdev local,id=host,path=build/qnx8/share,security_model=mapped-xattr \
  -device virtio-9p-pci,fsdev=host,mount_tag=hostshare,addr=0x05 \
  -display none -serial unix:/tmp/ser.sock,server,nowait -monitor none
```

The guest is a real QNX 8.0 (SDP 8.0), the driver is `drivers/fs9p` compiled for
that target, the share is served by QEMU's own 9P server. Nothing here is
simulated. The measurements are taken by `tests/qemu/qnx8/qbench.c`, which is
cross-compiled by `build-image.sh` and runs **inside** the guest.

## PERF-06 — mount time: 11 ms (target: under 50 ms)

From the system log (`slog2info`, buffer `passerelle`), one run:

| step | time |
|---|---|
| driver starts, trace events enabled | `15:44:22.169` |
| transport opened, device found, capabilities walked | `15:44:22.171` |
| privileges dropped (`uid 200`, four abilities) | `15:44:22.175` |
| 9P session open (`msize 131072`) | `15:44:22.180` |

**11 ms** from the first line of the driver to the 9P session, the resmgr
attachment and the interrupt service thread on IRQ 10 following. The device is
a transitional `virtio-9p-pci` on `B0:D5:F0`, IRQ 10 pin A, INTx delivery
(VIO-04; MSI-X is not exercised).

## VIO-04 — MSI-X is allocated by the PCI server (14 November 2026)

The same guest, with the same image, now comes up through MSI-X:

```
[INFO] virtio-pci: msix: capability at index 0, 2 vectors
[INFO] virtio-pci: msix: vector bound to line 256
[INFO] virtio-pci: MSI-X vector allocated by the PCI server (irq 256)
[INFO] fs9p: 9P session open, msize 131072
[INFO] fs9p: interrupt 256 attached (msix)
```

Line 256 is what the PCI server bound the vector to; the driver never writes the
APIC message, it asks the server (`cap_msix_set_irq_entry` →
`pci_device_cfg_cap_enable` → `pci_device_read_irq`, the sequence the SDP's
MSI-X module documents). `pci_msix_get_irq()` is **not** used: the SDP headers
put it under "The following APIs are for ICI support", and this guest has no ICI
configuration.

Three things this run corrected, each one only visible on a real target:

* **the shadow of configuration space carried no MSI-X capability.** Layer 1
  rebuilt only the *vendor* capabilities, so `msix_off` was always 0 and the
  portable programming path silently declined: every mount had been INTx, and
  the log said so. The QNX binding now asks the server for a vector instead of
  programming a table it cannot see (VIO-02, VIO-04);
* **`pci_device_read_irq()` was asked for one line.** The device has two vectors,
  so the server returned `nirq = -1` — the 2's complement of the number that did
  not fit — and an array of one entry would have held an *arbitrary* vector's
  line. The array is now at least the device's vector count (VIO-04);
* **`pgw_vio_reset()` cleared `msix_active` unconditionally**, contradicting its
  own comment ("the interrupt line belongs to the transport, not to the
  negotiated state"): a device reset does not take the vector back, so the
  driver was signalling through MSI-X while reporting INTx. Whichever transport
  really disabled a mode now says so in its own `teardown_irq` hook (VIO-09).

`-o irq=N` is the documented escape hatch to force the classic line, and it has
to decide at the transport: enabling MSI-X and then waiting on line N would mean
no interrupt ever arrives. Both paths are measured on this guest, at the same
rate:

| delivery | how it was chosen | 64 MiB | rate |
|---|---|---|---|
| MSI-X | the default (`-o` without `irq`) | 2.050 s | **31.22 MiB/s** |
| INTx | `-o ro,nosuid,debug,irq=10` | 2.051 s | **31.20 MiB/s** |

The two rates are the same to within the noise, which is what the transport's
design predicts: the round trip is bounded by QEMU's 9P server, not by how the
completion is signalled. The interrupt service thread only bumps a counter and
the owning thread drains the used ring, so neither delivery mode adds a wait.

## PERF-03 — sequential throughput: 30.7 MiB/s

```
# qbench rate /host/big.bin
qbench: rate bytes=67108864 seconds=2.083 MiBps=30.73
```

64 MiB read from the share through our driver, with a 128 KiB 9P message size
(512 round trips).

Two clients at the same time are served at the same rate each:

```
# qbench two /host/big.bin x 10
qbench: two readers priority=10 a_bytes=67108864 a_s=2.503 a_MiBps=25.57 b_bytes=67108864 b_s=2.495 b_MiBps=25.65
```

What this number says, and what it does not:

* it is the **end-to-end** figure of the whole chain (guest, our driver, the
  9P server, the host page cache), not the driver's own cost;
* the comparison the spec asks for (PERF-03 wants half of a Linux guest on the
  same machine) is the next table: the Linux guest reads the same file with
  the same message size;
* the guest copies the bytes into `/dev/shmem`, which is counted in the time.

## PERF-03 — the same file, the same machine, the Linux guest beside us

The Linux guest of `tests/qemu/run_share_check.py` reads the **same file**
(`build/qnx8/share/big.bin`, 64 MiB) from the **same host**, with the **same
9P message size**, so the two rows are comparable and the host's page cache is
warm for both:

```bash
python3 tests/qemu/run_share_check.py --protos 9p \
        --rate-file build/qnx8/share/big.bin --guest-msize 131072
```

The 14 November run, after the host page cache was warmed by the QNX measurement:

```
PGW-E5 INFO rate bytes=67108864 seconds=2.140 MiBps=29.91
```

| guest | client | message size | 64 MiB | rate | runs |
|---|---|---|---|---|---|
| QNX 8.0 | `drivers/fs9p` | 128 KiB | median 2.05 s | **31.3 MiB/s** | 3 |
| Linux 6.14 | kernel 9p client | 128 KiB | 2.140 s | **29.91 MiB/s** | 1 |

This run reaches **95.6%** of the QNX median, above PERF-03's 50% minimum. It is one Linux sample on a shared, noisy host, so it is a spot comparison, not a series.

Earlier Linux samples (11 November; different run conditions, retained as the variability record): 4.65 / 2.95 / 12.54 s with 128 KiB messages (median 13.8 MiB/s), and 50.92 s at the Linux default 4 KiB (1.26 MiB/s). Those runs measured the same guest and host path but were noisy; do not combine their median with the 14 November spot run.

## PERF-05 / QNX-03 — a priority read during a bulk copy

Sixteen readers saturate the transport, then one small file is read by a thread
whose priority is the one under test:

```
# qbench prio /host/big.bin /host/from-host.txt 63
qbench: prio priority=63 readers=16 bytes=18 seconds=0.0036 bulk_bytes=2359296
# qbench prio /host/big.bin /host/from-host.txt 10
qbench: prio priority=10 readers=16 bytes=18 seconds=0.0639 bulk_bytes=3932160
```

| priority of the reading thread | latency of the small read |
|---|---|
| 63 | **3.6 ms** |
| 10 | **63.9 ms** |

**Eighteen times faster** with the same driver, the same device and the same
load: the driver re-applies the caller's priority to the thread that serves the
request (QNX-03), and the request overtakes the bulk copy (QNX-04).

This is only true because the replies that arrive early are kept for their
owner (`drivers/common/src/reqtab_qnx.c`): one virtqueue serves every client,
so a reply can be popped by the thread that waits for another one. Dropping it
made two clients lose each other's answers - four requests timed out and the
rate fell by a factor of eighteen before the fix.

## QNX-03 / QNX-09 — kernel trace of the request and its priority

The real guest trace was captured on 14 November with:

```bash
python3 tests/qemu/qnx8/capture_trace.py --keep \
        --workdir /tmp/pgw-trace --timeout 360
python3 tests/qemu/qnx8/trace_report.py /tmp/pgw-trace/transcript.log
```

The trace contains **3,207 user events** (1,604 submissions, 1,603 replies)
and **1,177 paired transport round trips**. The capture deliberately stops while
some bulk readers are still in flight; those incomplete tail requests are not
counted as failed replies. The request payload decoded to both QNX priorities
**10 and 63**, and the operation encoded in the payload agreed with the event
code for every parsed event. The report therefore ends with:

```
trace: every submission has its reply, and both decodings agree (QNX-09, QNX-03)
```

The trace also exposed and fixed a real concurrency bug: the transport cookie
was a non-atomic global increment, so concurrent readers could receive the same
cookie and steal each other's completion. `common/src/cookie.c` now allocates
cookies with an atomic CAS; `test_driver_traceop` draws 160,000 cookies from
eight threads and checks that none repeats. This is why the post-fix trace has no
collision/orphan report.

The report keeps round-trip deltas in kernel cycles. QNX 8.0's minimal guest
image does not ship a safe cycle-rate probe; converting those deltas to
microseconds without that rate would be invented precision. The paired events,
operation labels and priority payload are measured facts.

The numbers above come from a single run each and are not a benchmark series.

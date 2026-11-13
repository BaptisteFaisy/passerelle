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
* it is **not** yet the comparison the spec asks for (PERF-03 wants half of a
  Linux guest on the same machine). The Linux baseline harness exists
  (`docs/measurements/linux-baseline-9p.md`) but it counts round trips per
  POSIX operation, not a bulk byte rate, so the two rows are not comparable
  yet;
* the guest copies the bytes into `/dev/shmem`, which is counted in the time.

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

The numbers above come from a single run each and are not a benchmark series.

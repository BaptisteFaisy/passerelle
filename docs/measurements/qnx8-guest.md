# Measurements on a QNX 8.0 guest — our own driver (PERF-03, PERF-06)

Produced on **13 November 2026, 16:45 CET** by:

```bash
# build the image and the drivers for the real target
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

The guest is a real QNX 8.0 (SDP 8.0), the driver is `drivers/fs9p` compiled
for that target, the share is served by QEMU's own 9P server. Nothing here is
simulated.

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

## PERF-03 — sequential throughput: 64 MiB in 9.60 s (6.7 MiB/s)

Inside the guest:

```
# time cat /host/big.bin > /dev/shmem/big.out
    9.60s real     0.06s user     0.00s system
# ls -l /dev/shmem/big.out
-rw-rw-rw-  2 root 0 67108864 2026-11-13 15:44 /dev/shmem/big.out
```

64 MiB read from the share and written to the guest's shared memory, through
our driver, with a 128 KiB 9P message size (512 round trips).

What this number says, and what it does not:

* it is the **end-to-end** figure of the whole chain (guest, our driver, the
  9P server, the host page cache), not the driver's own cost;
* it is **not** yet the comparison the spec asks for (PERF-03 wants half of a
  Linux guest on the same machine). The Linux baseline harness exists
  (`docs/measurements/linux-baseline-9p.md`) but it counts round trips per
  POSIX operation, not a bulk byte rate, so the two rows are not comparable
  yet;
* the guest copies the bytes into `/dev/shmem`, which is counted in the time.

The numbers above are the ones the gap analysis quotes; they come from a
single run and are not a benchmark series.

<!--
SPDX-License-Identifier: Apache-2.0
-->

# The host-side vhost-user front end

Gap 58 of [gap-analysis.md](gap-analysis.md), rank 3 of the backlog: *drive the
real `virtiofsd` through the real vhost-user protocol*. Until this, the virtiofs
path was only ever exercised against our own fake
(`protofuse/tests/fake_virtiofsd.c`) and against the Python reference server of
`tests/fuse_ref/`. Both are ours, so a mistake in our reading of the wire could
be repeated on both sides and still pass.

`host/vhost/` removes that self-reference for the transport layer: the peer is
now `virtiofsd` 1.10.0, an independent implementation, and it answers.

## What is implemented

A vhost-user **front end** (the hypervisor/device side of the socket) in Python,
standard library only:

| Module | Role |
| --- | --- |
| `protocol.py` | request numbers, header flags, virtio and protocol feature bits, payload codecs |
| `transport.py` | UNIX socket, 12-byte header framing, `SCM_RIGHTS`, reply correlation |
| `memory.py` | `os.memfd_create` + `mmap` region, the `SET_MEM_TABLE` payload |
| `vring.py` | split virtqueue: desc table, avail ring, used ring, buffers, `eventfd` kick/call |
| `device.py` | the handshake and the per-queue setup, in the order the protocol requires |
| `fuse_client.py` | FUSE requests posted on the queue, replies harvested and decoded |
| `fake_backend.py` | a vhost-user **backend** in Python, so unit tests need no `virtiofsd` |
| `driver.py` | launches `virtiofsd`, runs the sequence, compares with the truth |

The FUSE wire table is not declared a second time: `host/pgwutil/fuse_wire.py`
is now the single Python declaration, used by this front end *and* by
`tests/fuse_ref/fuse_ref_wire.py`. Its C twin stays `protofuse/src/fusep_abi.h`,
and two tests compare the two tables field by field.

## The address convention (the part that is easy to get wrong)

`vhost` 0.10 maps the memfd wherever it likes and translates through the memory
table, so the two kinds of address in play are **not** the same:

* the three ring addresses of `SET_VRING_ADDR` are front-end virtual addresses;
  the backend converts them with `vmm_va_to_gpa()` using the `userspace_addr`
  we reported, so we report the address of our own `mmap`;
* the `addr` field of every descriptor is a **guest physical address**, which
  the backend converts with its own mapping.

With one region based at guest physical address 0, a descriptor address is
simply the offset in the shared region. `host/vhost/fake_backend.py` maps the
memfd at an address of its own and translates through the table, exactly like
the crate, so the unit tests would catch a front end that confused the two.

Two more details that only show up when running against a real backend:

* `GET_QUEUE_NUM` is refused unless the `MQ` protocol feature was negotiated;
* a queue is armed by `SET_VRING_KICK` but stays **disabled** until
  `SET_VRING_ENABLE`, which itself requires the `VHOST_USER_F_PROTOCOL_FEATURES`
  virtio bit (bit 30) to have been acked.

## What this proves

Running `python3 -m pytest host/tests/test_vhost_virtiofsd.py -q` with
`virtiofsd` installed:

* the vhost-user handshake completes against a real backend: `GET_FEATURES`
  returns `INDIRECT_DESC|EVENT_IDX|PROTOCOL_FEATURES|VERSION_1`, we ack
  `PROTOCOL_FEATURES|VERSION_1`, and `REPLY_ACK` is negotiated (every later
  request is answered by an acknowledgement we check);
* the memfd published over `SCM_RIGHTS` is really shared: `virtiofsd` reads our
  descriptors and writes its replies into our mapping;
* one split virtqueue, driven only by `eventfd` kick/call pairs, carries FUSE
  traffic both ways;
* `INIT` negotiates FUSE 7.38 with a 1 MiB `max_write`;
* `LOOKUP` returns the inode, size and mode `os.stat` reports on the host;
* `GETATTR` agrees with the same truth;
* `OPEN` + `READ` return the exact bytes of the file;
* `WRITE` + `FLUSH` + `RELEASE` put the bytes **on the host disk**, verified by
  reading the file back with `open()`;
* `OPENDIR` + `READDIR` return exactly the entries of the directory (dot
  entries included, as a FUSE server does);
* `FORGET` gives the lookup reference back (VFS-04).

The whole scenario runs in about one second, well inside the 5-minute CI budget,
and it is skipped with an explicit reason when `virtiofsd` is absent.

## What this does NOT prove

Stated plainly, because the gap is real:

* **No QNX guest.** Nothing here runs on the target. The guest side of the
  driver (`drivers/fsfuse`, `rmfs`) is untouched and unexercised by this work.
* **No virtio PCI or MMIO device.** We are the device side of a vhost-user
  socket; there is no capability walk, no BAR, no doorbell, no MSI-X. The
  transport of `virtio/` (`pci_modern.c`, `mmio.c`) is still only tested against
  its own simulated transport.
* **No QEMU.** `qshare` still emits the QEMU options, but this proof bypasses
  QEMU entirely: our process is the hypervisor side.
* **One request queue, one region, synchronous traffic.** No multiqueue (`MQ`
  is not negotiated), no hiprio traffic, no indirect descriptors, no packed
  virtqueue, no `EVENT_IDX` (we use legacy kick/call signalling), no in-flight
  tracking, no live migration, no `IOTLB`/`ACCESS_PLATFORM`.
* **A handful of FUSE operations.** `SETATTR`, `CREATE`, `MKDIR`, `UNLINK`,
  `RENAME`, `SYMLINK`, `LINK`, `READLINK`, `FSYNC`, `STATFS`, `INTERRUPT` and
  `BATCH_FORGET` are not driven through `virtiofsd` here; they remain covered
  against our own fake and the reference server.
* **No performance claim.** One-shot operations, no throughput measurement, so
  nothing is said about PERF-03.
* **The sandbox is off.** `driver.py` runs `virtiofsd` with `--sandbox=none
  --seccomp=none` so an unprivileged CI worker can run it; that is fine for a
  temporary directory, and it is not how a guest should be served.

## Reproduce

```sh
cd host
python3 -m vhost --share /tmp/pgw-shared          # the report, line by line
python3 -m pytest tests/test_vhost_virtiofsd.py -q # the same, as a test
python3 -m pytest tests/test_vhost_protocol.py \
    tests/test_vhost_vring.py \
    tests/test_vhost_fuse_client.py -q            # no virtiofsd needed
```
